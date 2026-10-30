#ifndef __VOICE_PLAYER_H
#define __VOICE_PLAYER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 播放来源标识（仅用于日志区分；数据本身不绑定来源） */
typedef enum {
    VOICE_SRC_ALARM = 0,   /* 报警提示音 */
    VOICE_SRC_SLEEPY,      /* 犯困提醒 */
    VOICE_SRC_OTHER,       /* 其他/AI 语音 */
} voice_src_t;

/**
 * 初始化独立播放模块并创建后台播放任务。
 * 需在 voice_init() 成功后调用；幂等，重复调用直接返回 ESP_OK。
 */
esp_err_t voice_player_init(void);

/**
 * 非阻塞提交一段 16kHz/16bit/双声道/小端 PCM 播放请求。
 * 播放中（忙）时立即返回 ESP_ERR_INVALID_STATE 并记日志（丢弃，不排队）。
 * 调用方不阻塞；嵌入式只读 PCM 常驻无需复制，其余数据需在排空完成前保持有效。
 */
esp_err_t voice_player_play(const uint8_t *data, size_t len, voice_src_t src);

/* 是否正在播放（忙） */
bool voice_player_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_PLAYER_H */