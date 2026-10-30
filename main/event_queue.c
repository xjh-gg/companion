#include "event_queue.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 队列深度 */
#define EVENT_QUEUE_LEN   8

static QueueHandle_t s_queue = NULL;

esp_err_t event_queue_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;          /* 幂等：已初始化直接返回 */
    }
    s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t event_queue_send(event_t evt, uint32_t timeout_ms)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (evt == EVT_NONE) {
        return ESP_OK;          /* 忽略空事件，非空即投递 */
    }
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY
                                                     : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(s_queue, &evt, ticks) != pdPASS) {
        return ESP_ERR_TIMEOUT; /* 队列满，丢弃不阻塞生产者 */
    }
    return ESP_OK;
}

bool event_queue_recv(event_t *evt, uint32_t timeout_ms)
{
    if (s_queue == NULL || evt == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY
                                                     : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive(s_queue, evt, ticks) == pdPASS);
}