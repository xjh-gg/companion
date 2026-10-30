#ifndef __STATE_MACHINE_H
#define __STATE_MACHINE_H

#include <stdint.h>

#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 5 种心情状态 */
typedef enum {
    STATE_IDLE = 0,    /* 待机 */
    STATE_HAPPY,       /* 被关注 */
    STATE_INTERACT,    /* 互动中 */
    STATE_SLEEPY,      /* 犯困 */
    STATE_ALARM,       /* 报警 */
} companion_state_t;

/* 初始化状态机（默认待机，犯困定时 60 分钟） */
void state_machine_init(void);

/* 投递事件：内部做状态迁移并联动 LED / 蜂鸣器 */
void state_machine_on_event(event_t evt);

/* 周期调用（建议 1s）：超时检测 + 犯困定时 + 报警节拍 */
void state_machine_tick(void);

/* 获取当前状态 */
companion_state_t state_machine_get_state(void);

/* 配置犯困定时（分钟），默认 60；M3 接配置模块 */
void state_machine_set_sleepy_timeout_min(uint32_t min);

/* 获取当前犯困定时（分钟） */
uint32_t state_machine_get_sleepy_timeout_min(void);

#ifdef __cplusplus
}
#endif

#endif /* __STATE_MACHINE_H */