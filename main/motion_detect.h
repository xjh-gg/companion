#ifndef __MOTION_DETECT_H
#define __MOTION_DETECT_H

#include "esp_err.h"
#include <stdbool.h>

/* 运动事件类型 */
typedef enum {
    MOTION_NONE = 0,   /* 无运动事件 */
    MOTION_PICK_UP,    /* 拿起 */
    MOTION_SHAKE,      /* 摇晃 */
} motion_event_t;

/* 拿起检测：z 轴重力差分阈值（g）。静止时 |z|≈1g，拿起后减小超过该值即触发 */
#define PICKUP_DZ_THRESHOLD   0.45f

/* 拿起后需要持续保持"拿起"状态的最小时间（ms），避免瞬时抖动误判 */
#define PICKUP_HOLD_MS        400

/* 摇晃检测：1 秒窗口内方向翻转次数达到该值判定为摇晃 */
#define SHAKE_FLIP_COUNT      3

/* 摇晃检测时间窗（ms） */
#define SHAKE_WINDOW_MS       1000

/* 运动检测句柄（不透明，内部持有滑动窗口状态） */
typedef struct motion_detect motion_detect_t;

/* 创建运动检测器，返回句柄；失败返回 NULL */
motion_detect_t *motion_detect_create(void);

/* 周期喂数据（x/y/z 单位 g），内部做拿起/摇晃判定。
 * 建议以 50ms 周期调用。返回本次检测到的事件（无则 MOTION_NONE） */
motion_event_t motion_detect_update(motion_detect_t *md,
                                    float gx, float gy, float gz);

/* 复位检测器内部状态（如设备重新校准） */
void motion_detect_reset(motion_detect_t *md);

/* 销毁并释放句柄 */
void motion_detect_destroy(motion_detect_t *md);

#endif
