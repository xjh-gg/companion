#include "motion_detect.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct motion_detect {
    /* ---- 拿起检测状态 ---- */
    float   z_ref;              /* 参考 z 值：稳定静止时的 z */
    bool    z_ref_valid;        /* z_ref 是否有效（已稳定） */
    uint32_t stable_count;      /* 连续静止计数，用于建立 z_ref */
    bool    picked_up;          /* 是否处于"拿起"态 */
    uint32_t pick_lock_tick;    /* 拿起态锁定直到该 tick */

    /* ---- 摇晃检测状态 ---- */
    int     last_sign[3];       /* 各轴上一采样方向（1/-1/0） */
    uint32_t flips;             /* 当前窗口内翻转次数 */
    uint32_t window_start;      /* 窗口起始 tick */
    bool    window_active;      /* 窗口是否在计数 */
};

#define STABLE_SAMPLES_TO_CALIB    5   /* 连续 5 次静止后建立 z_ref */

/* 各轴方向翻转计数：轴值跨越 0 视为一次方向变化 */
static void shake_feed(motion_detect_t *md, float g[3], uint32_t now)
{
    int sign[3];
    bool flipped = false;

    for (int i = 0; i < 3; i++) {
        sign[i] = (g[i] > 0.3f) ? 1 : ((g[i] < -0.3f) ? -1 : 0);
        if (sign[i] != 0 && md->last_sign[i] != 0 && sign[i] != md->last_sign[i]) {
            flipped = true;
        }
    }
    memcpy(md->last_sign, sign, sizeof(sign));

    if (flipped) {
        if (!md->window_active) {
            md->window_active = true;
            md->window_start = now;
            md->flips = 1;
        } else {
            md->flips++;
        }
    }
}

motion_detect_t *motion_detect_create(void)
{
    motion_detect_t *md = calloc(1, sizeof(motion_detect_t));
    return md;
}

motion_event_t motion_detect_update(motion_detect_t *md,
                                    float gx, float gy, float gz)
{
    if (!md) return MOTION_NONE;

    uint32_t now = xTaskGetTickCount();
    float g[3] = {gx, gy, gz};
    float mag = sqrtf(gx * gx + gy * gy + gz * gz);

    /* ---- 摇晃检测（优先级低于拿起锁定） ---- */
    shake_feed(md, g, now);

    /* 窗口超时则结算：足够翻转次数即判定摇晃 */
    if (md->window_active) {
        uint32_t elapsed = now - md->window_start;
        if (elapsed >= pdMS_TO_TICKS(SHAKE_WINDOW_MS)) {
            if (md->flips >= SHAKE_FLIP_COUNT) {
                md->window_active = false;
                md->flips = 0;
                return MOTION_SHAKE;
            }
            md->window_active = false;
            md->flips = 0;
        }
    }

    /* ---- 拿起检测 ---- */
    /* 静止判定：加速度模长接近 1g 且各轴变化小 */
    bool stationary = (fabsf(mag - 1.0f) < 0.25f);

    if (stationary) {
        if (!md->z_ref_valid) {
            md->stable_count++;
            if (md->stable_count >= STABLE_SAMPLES_TO_CALIB) {
                md->z_ref = gz;
                md->z_ref_valid = true;
                md->picked_up = false;
            }
        }
    } else {
        md->stable_count = 0;

        if (md->z_ref_valid && !md->picked_up) {
            /* 拿起：|z| 相比参考大幅减小（设备被拿起，重力分量转移） */
            float dz = fabsf(gz) - fabsf(md->z_ref);
            if (dz < -PICKUP_DZ_THRESHOLD) {
                md->picked_up = true;
                md->pick_lock_tick = now + pdMS_TO_TICKS(PICKUP_HOLD_MS);
                return MOTION_PICK_UP;
            }
        }
    }

    /* 拿起态保持：锁定期间不再重复触发 */
    if (md->picked_up) {
        if (now >= md->pick_lock_tick) {
            md->picked_up = false;
            md->z_ref_valid = false;   /* 需重新校准 */
        }
    }

    return MOTION_NONE;
}

void motion_detect_reset(motion_detect_t *md)
{
    if (!md) return;
    memset(md, 0, sizeof(*md));
}

void motion_detect_destroy(motion_detect_t *md)
{
    free(md);
}
