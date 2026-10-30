#include "config.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "at24c02.h"
#include "state_machine.h"

static const char *TAG = "CONFIG";

/* 配置在 EEPROM 中的起始地址与保留区大小（环形日志从 176 开始） */
#define CONFIG_EEPROM_OFFSET   0
#define CONFIG_EEPROM_SIZE     176

/* EEPROM 功能开关（AT24C02_ENABLED 为 0 时 config 整体回退默认值） */
#if AT24C02_ENABLED

static companion_config_t s_config;

/* 串口首配辅助（定义见下，先声明供 config_init 使用） */
static void config_prompt_credentials(void);

/* 计算校验字节：对除 check 外所有字节做异或 */
static uint8_t config_check(const companion_config_t *cfg)
{
    uint8_t tmp[sizeof(*cfg)];
    memset(tmp, 0, sizeof(tmp));        /* 清填充字节保证确定性 */
    memcpy(tmp, cfg, sizeof(*cfg));

    const size_t check_off = offsetof(companion_config_t, check);
    uint8_t xor_sum = 0;
    for (size_t i = 0; i < sizeof(*cfg); i++) {
        if (i == check_off) {
            continue;
        }
        xor_sum ^= tmp[i];
    }
    return xor_sum;
}

/* 读取并校验，成功返回 ESP_OK */
static esp_err_t config_load(companion_config_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_err_t ret = at24c02_read_buffer(CONFIG_EEPROM_OFFSET, (uint8_t *)out,
                                        sizeof(*out));
    if (ret != ESP_OK) {
        return ret;
    }
    /* 校验：check 须与其余字节异或一致 */
    if (out->check != config_check(out)) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

void config_init(void)
{
    esp_err_t ret = config_load(&s_config);
    if (ret == ESP_OK && s_config.wifi_ssid[0] != '\0') {
        ESP_LOGI(TAG, "配置加载成功，犯困间隔=%u分钟",
                 (unsigned)s_config.sleepy_timeout_min);
    } else {
        /* 首次上电或配置无效：清空凭据并进入串口首配流程 */
        ESP_LOGW(TAG, "读取配置失败 (%s) 或无 WiFi 配置，进入首次配置",
                 esp_err_to_name(ret));
        s_config.sleepy_timeout_min = DEFAULT_SLEEPY_MIN;
        s_config.volume           = DEFAULT_VOLUME;
        s_config.mode_enabled     = DEFAULT_MODE_ENABLED;
        s_config.wifi_ssid[0]     = '\0';
        s_config.wifi_password[0] = '\0';
        s_config.mqtt_broker_uri[0] = '\0';
        config_prompt_credentials();
        esp_err_t sret = config_save(&s_config);
        if (sret != ESP_OK) {
            /* EEPROM 暂不可写（如 WP 置高）：本次凭据仅驻留在内存，
             * 仍可让本机运行并在本次启动尝试联网，但断电后丢失。 */
            ESP_LOGE(TAG, "配置保存失败 (%s)：本次凭据仅存内存可用，"
                          "断电不保留。请检查 AT24C02 写保护(WP)",
                     esp_err_to_name(sret));
        } else {
            ESP_LOGI(TAG, "凭据已保存到 AT24C02，正在重启使其生效...");
            /* 短暂保留输出时间后重启，让新配置在下一次启动即生效 */
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }

    /* 联动状态机：犯困定时使用配置值 */
    state_machine_set_sleepy_timeout_min(s_config.sleepy_timeout_min);
}

companion_config_t config_get(void)
{
    return s_config;
}

esp_err_t config_save(const companion_config_t *cfg)
{
    companion_config_t tmp = *cfg;
    tmp.check = config_check(cfg);       /* 更新校验字节 */

    esp_err_t ret = at24c02_write_buffer(CONFIG_EEPROM_OFFSET, (uint8_t *)&tmp,
                                         sizeof(tmp));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "配置保存失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_config = tmp;                      /* 更新缓存 */
    ESP_LOGI(TAG, "配置已保存，犯困间隔=%u分钟", (unsigned)tmp.sleepy_timeout_min);
    return ESP_OK;
}

/* ---------------- 串口首次配置辅助 ---------------- */

/* 单行读取：等待用户输入真正的非空行。
 * 说明：USB-Serial/JTAG 作为主控制台时，复位后 host 侧串口需要一点时间重新
 * 连接。若直接 fgets，在 CDC 接收缓冲为空的瞬间可能立即返回空，导致首配被整段
 * 跳过。这里循环短延时重试，直至读到非空行或等待窗口超时。 */
static void read_line(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    const uint32_t window_ms = 120000;           /* 单条首配等待窗口 */
    const TickType_t start = xTaskGetTickCount();
    do {
        if (fgets(buf, (int)len, stdin) == NULL) {
            buf[0] = '\0';
            clearerr(stdin);                     /* 清除可能的 EOF 标志，继续等待 */
        }
        /* 去掉末尾换行符 */
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n > 0) {
            return;                              /* 收到非空行 */
        }
        buf[0] = '\0';
        vTaskDelay(pdMS_TO_TICKS(50));
    } while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(window_ms));
}

static void prompt_field(const char *label, char *out, size_t len)
{
    printf("%s: ", label);
    fflush(stdout);
    read_line(out, len);
}

/* 通过串口读取 WiFi/MQTT 凭据，仅写入 AT24C02，不外泄到构建产物 */
static void config_prompt_credentials(void)
{
    printf("\r\n======== 首次配置 Companion ========\r\n");
    printf("请输入以下信息（仅保在板载 AT24C02，不写入固件）：\r\n");
    prompt_field("WiFi SSID", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    prompt_field("WiFi 密码", s_config.wifi_password, sizeof(s_config.wifi_password));
    prompt_field("MQTT Broker URI (如 mqtt://192.168.137.1:1883)",
                 s_config.mqtt_broker_uri, sizeof(s_config.mqtt_broker_uri));
    printf("配置读取完成，正在保存...\r\n");
}

#else /* !AT24C02_ENABLED */
/* 无 EEPROM 降级：全部走默认值 */

static companion_config_t s_config;

void config_init(void)
{
    s_config.sleepy_timeout_min = DEFAULT_SLEEPY_MIN;
    s_config.volume             = DEFAULT_VOLUME;
    s_config.mode_enabled       = DEFAULT_MODE_ENABLED;
    s_config.check              = 0;

    ESP_LOGW(TAG, "AT24C02 未启用，使用默认配置，犯困间隔=%u分钟",
             (unsigned)s_config.sleepy_timeout_min);
    state_machine_set_sleepy_timeout_min(s_config.sleepy_timeout_min);
}

companion_config_t config_get(void)
{
    return s_config;
}

esp_err_t config_save(const companion_config_t *cfg)
{
    ESP_LOGW(TAG, "AT24C02 未启用，配置保存跳过");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif /* AT24C02_ENABLED */