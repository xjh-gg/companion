#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "mqtt_manager.h"
#include "wifi_manager.h"
#include "state_machine.h"
#include "config.h"

static const char *TAG = "MQTT_MGR";

#define MQTT_STATUS_TOPIC  "companion/status"
#define MAX_MSG_LEN        192

static esp_mqtt_client_handle_t s_client   = NULL;
static bool                    s_connected = false;

/* 心情状态名（与 companion_state_t 枚举一一对应） */
static const char *ms_state_names[] = {
    "idle", "happy", "interact", "sleepy", "alarm",
};

/* ---------------- 事件处理 ---------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event->event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT 连接成功 (broker=%s)", config_get().mqtt_broker_uri);
            /* 连接上后立即上报一次当前状态 */
            mqtt_manager_publish_status();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "MQTT 断开连接，esp-mqtt 内部自动重连...");
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle != NULL) {
                ESP_LOGE(TAG, "MQTT 错误 (error_type=%d)", event->error_handle->error_type);
            }
            break;
        default:
            break;
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t mqtt_manager_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;  /* 已启动，幂等 */
    }

    companion_config_t app_cfg = config_get();
    const char *broker_uri = app_cfg.mqtt_broker_uri;
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT client 创建失败");
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client 启动失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT client 已启动，URI=%s", broker_uri);
    return ESP_OK;
}

/* 构造一条 JSON 状态（snprintf 拼串，不引第三方 JSON 库，减少依赖） */
static void build_status_json(char *buf, size_t len)
{
    companion_state_t st = state_machine_get_state();
    const char *name = (st <= STATE_ALARM) ? ms_state_names[st] : "unknown";
    int64_t uptime_us = esp_timer_get_time();
    int32_t rssi = wifi_manager_get_rssi();

    snprintf(buf, len,
             "{\"device\":\"ESP32-S3 Companion\","
             "\"current_state\":\"%s\",\"state_id\":%d,"
             "\"rssi\":%d,\"uptime_s\":%lld}",
             name, (int)st, (int)rssi, (long long)(uptime_us / 1000000));
}

void mqtt_manager_publish_status(void)
{
    char buf[MAX_MSG_LEN];
    build_status_json(buf, sizeof(buf));
    mqtt_manager_publish(MQTT_STATUS_TOPIC, buf, 0);
}

void mqtt_manager_publish(const char *topic, const char *payload, int qos)
{
    if (s_client == NULL || !s_connected) {
        ESP_LOGW(TAG, "MQTT 未连接，跳过上报 (topic=%s)", topic);
        return;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT 上报失败 (topic=%s)", topic);
    }
}