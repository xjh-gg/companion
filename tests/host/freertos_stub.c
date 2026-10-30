#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

#define STUB_QUEUE_CAP 64
#define STUB_MAX_ITEM_SIZE 16

static uint8_t s_buf[STUB_QUEUE_CAP][STUB_MAX_ITEM_SIZE];
static UBaseType_t s_cap;
static UBaseType_t s_len;
static UBaseType_t s_head;
static UBaseType_t s_item_size;

QueueHandle_t xQueueCreate(UBaseType_t queue_length, UBaseType_t item_size)
{
    s_cap = (queue_length > STUB_QUEUE_CAP) ? STUB_QUEUE_CAP : queue_length;
    s_len = 0;
    s_head = 0;
    s_item_size = (item_size > STUB_MAX_ITEM_SIZE) ? STUB_MAX_ITEM_SIZE
                                                   : item_size;
    return (QueueHandle_t)s_buf;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (queue == NULL || item == NULL || s_len >= s_cap) {
        return pdFALSE;
    }
    memcpy(s_buf[(s_head + s_len) % s_cap], item, s_item_size);
    s_len++;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (queue == NULL || buffer == NULL || s_len == 0) {
        return pdFALSE;
    }
    memcpy(buffer, s_buf[s_head], s_item_size);
    s_head = (s_head + 1) % s_cap;
    s_len--;
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return (queue == NULL) ? 0 : s_len;
}
