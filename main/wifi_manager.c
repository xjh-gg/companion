#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "config.h"

static const char *TAG = "WIFI_MGR";

/* 连接完成标志（由 IP_EVENT_STA_GOT_IP 事件置位，断线时清除） */
static bool s_connected = false;

/* ---------------- 事件处理 ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* 启动后发起连接 */
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = event_data;
        s_connected = false;
        ESP_LOGW(TAG, "WiFi 断开 (reason=%u)，正在重连...", disconnected->reason);
        /* 简单退避重连：延时 1s 后再次连接，断线不阻塞本地任务 */
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = event_data;
        s_connected = true;
        companion_config_t app_cfg = config_get();
        ESP_LOGI(TAG, "WiFi 连接成功 (SSID=%s, IP=" IPSTR ")",
                 app_cfg.wifi_ssid, IP2STR(&evt->ip_info.ip));
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t wifi_manager_init(void)
{
    /* NVS：WiFi 运行需要（用于存储校准/配置）。失败时擦除原生重试一次。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除，执行 nvs_flash_erase");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败 (%s)，继续", esp_err_to_name(ret));
    }

    /* 网络栈库与默认事件循环 */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init 失败");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop 创建失败");

    /* 注册事件处理器（放在 wifi_init 之前注册，避免遗漏启动事件） */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL));

    /* 创建 STA 网卡并初始化 WiFi */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init 失败");

    wifi_config_t wifi_cfg = { 0 };
    companion_config_t app_cfg = config_get();
    strncpy((char *)wifi_cfg.sta.ssid, app_cfg.wifi_ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, app_cfg.wifi_password,
            sizeof(wifi_cfg.sta.password) - 1);
    /* 允许通过 Kconfig 关闭安全（省电/兼容）不在此做，保持默认 WPA2 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "设置 WiFi 配置失败");
    if (wifi_cfg.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi SSID 为空，跳过连接（可先串口首配）");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start 失败");
    esp_wifi_connect();  /* 非阻塞：即连不上也不影响本地功能 */

    ESP_LOGI(TAG, "WiFi 初始化完成，目标 SSID=%s", app_cfg.wifi_ssid);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

int32_t wifi_manager_get_rssi(void)
{
    if (!s_connected) {
        return -127;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return -127;
    }
    return (int32_t)ap_info.rssi;
}