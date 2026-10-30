#include "event_log.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "at24c02.h"

static const char *TAG = "EVENT_LOG";

/* 环形日志区域：位于 AT24C02 偏移 176 起，共 80B（config 从 0 起占 176B） */
#define LOG_OFFSET          176
#define LOG_REGION_SIZE     80

/* 头部：2 字节（小端）存当前写入槽索引；其后为槽区 */
#define LOG_HEADER_SIZE     2

/* 每槽 2 字节：低字节=事件类型，高字节=序号取模 */
#define SLOT_SIZE           2
#define LOG_SLOTS           ((LOG_REGION_SIZE - LOG_HEADER_SIZE) / SLOT_SIZE)

/* 事件枚举最大有效值（用于过滤未写过的槽） */
#define EVENT_T_MAX         ((int)EVT_INTERACT_END)

#if AT24C02_ENABLED

static uint16_t read_index(void)
{
    uint8_t hdr[LOG_HEADER_SIZE] = {0, 0};
    if (at24c02_read_buffer(LOG_OFFSET, hdr, LOG_HEADER_SIZE) != ESP_OK) {
        return 0;
    }
    uint16_t idx = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    if (idx >= LOG_SLOTS) {
        return 0;                     /* 非法索引视为 0 */
    }
    return idx;
}

static esp_err_t write_index(uint16_t idx)
{
    uint8_t hdr[LOG_HEADER_SIZE] = { (uint8_t)(idx & 0xFF), (uint8_t)(idx >> 8) };
    return at24c02_write_buffer(LOG_OFFSET, hdr, LOG_HEADER_SIZE);
}

static void slot_addr(uint16_t slot, uint8_t *out)
{
    *out = (uint8_t)(LOG_OFFSET + LOG_HEADER_SIZE + slot * SLOT_SIZE);
}

esp_err_t event_log_init(void)
{
    /* 校验头部索引：仅当非法时清空（无需逐槽清零，读侧按类型过滤） */
    uint8_t hdr[LOG_HEADER_SIZE] = {0, 0};
    esp_err_t ret = at24c02_read_buffer(LOG_OFFSET, hdr, LOG_HEADER_SIZE);
    if (ret != ESP_OK) {
        return ret;
    }
    uint16_t idx = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    if (idx >= LOG_SLOTS) {
        uint8_t zero[LOG_HEADER_SIZE] = {0, 0};
        at24c02_write_buffer(LOG_OFFSET, zero, LOG_HEADER_SIZE);
        ESP_LOGW(TAG, "日志头部非法，已重置索引");
    }
    ESP_LOGI(TAG, "环形事件日志初始化成功 (%u 槽)", (unsigned)LOG_SLOTS);
    return ESP_OK;
}

esp_err_t event_log_record(event_t evt)
{
    uint16_t idx = read_index();
    uint8_t slot[SLOT_SIZE] = {
        (uint8_t)evt,                  /* 低字节：事件类型 */
        (uint8_t)(idx % 256),          /* 高字节：序号取模，用于区分先后 */
    };
    uint8_t addr = 0;
    slot_addr(idx, &addr);
    esp_err_t ret = at24c02_write_buffer(addr, slot, SLOT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "事件日志写入失败: %s", esp_err_to_name(ret));
        return ret;
    }
    uint16_t next = (uint16_t)((idx + 1) % LOG_SLOTS);
    return write_index(next);
}

int event_log_read_recent(event_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    uint16_t idx = read_index();
    int count = 0;
    int limit = (max < LOG_SLOTS) ? max : (int)LOG_SLOTS;

    /* 从最新槽（idx-1）向前回溯 */
    int slot = (int)idx - 1;
    for (int i = 0; i < limit; i++) {
        if (slot < 0) {
            slot = LOG_SLOTS - 1;
        }
        uint8_t addr = 0;
        uint8_t slot_data[SLOT_SIZE] = {0, 0};
        slot_addr((uint16_t)slot, &addr);
        if (at24c02_read_buffer(addr, slot_data, SLOT_SIZE) == ESP_OK) {
            if ((int)slot_data[0] <= EVENT_T_MAX) {   /* 有效事件类型才记录 */
                out[count++] = (event_t)slot_data[0];
            }
        }
        slot--;
    }
    return count;
}

#else /* !AT24C02_ENABLED */

esp_err_t event_log_init(void)
{
    ESP_LOGW(TAG, "AT24C02 未启用，日志初始化跳过");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t event_log_record(event_t evt)
{
    ESP_LOGW(TAG, "AT24C02 未启用，日志记录跳过");
    return ESP_ERR_NOT_SUPPORTED;
}

int event_log_read_recent(event_t *out, int max)
{
    return 0;
}
#endif /* AT24C02_ENABLED */