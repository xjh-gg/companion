#ifndef __EVENT_QUEUE_H
#define __EVENT_QUEUE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一事件枚举（覆盖所有事件源） */
typedef enum {
    EVT_NONE = 0,        /* 空事件 */
    EVT_KEY_PRESSED,     /* 按键按下 */
    EVT_PICKED_UP,       /* 拿起检测 */
    EVT_SHAKE,           /* 摇晃检测 */
    EVT_HAND_NEAR,       /* 手靠近 */
    EVT_DARK,            /* 环境过暗 */
    EVT_TIMER_SLEEPY,    /* 犯困定时到（默认 60 分钟） */
    EVT_ALARM,           /* 紧急报警 */
    EVT_INTERACT_START,  /* M2 预留：语音对话开始（当前无生产者） */
    EVT_INTERACT_END,    /* M2 预留：语音对话结束（当前无生产者） */
} event_t;

/* 初始化事件队列（幂等），队列深度 8 */
esp_err_t event_queue_init(void);

/* 投递事件（生产者用）。非满即投递，不阻塞；timeout_ms 超时返回 ESP_ERR_TIMEOUT */
esp_err_t event_queue_send(event_t evt, uint32_t timeout_ms);

/* 接收事件（状态机用）。阻塞 timeout_ms，收到返回 true，超时返回 false */
bool event_queue_recv(event_t *evt, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __EVENT_QUEUE_H */