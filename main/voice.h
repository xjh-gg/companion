#ifndef __VOICE_H
#define __VOICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * 语音抽象接口层（M5 预留）
 * ---------------------------------------------------------------------
 * 本头文件定义上层（状态机 / 未来 UI）使用的"语音会话 / 播放 / 录音"
 * 三块抽象接口，作为未来接入音频 Codec（ES8388 + I2S）与小智 AI 固件的
 * 隔离层。M5 阶段仅提供空实现占位，不接真实硬件。
 *
 * 上层只需依赖本头文件，即可在 M6 及以后无缝切换到真实实现，
 * 无需改动调用方代码。
 * ===================================================================== */

/* 语音会话状态 */
typedef enum {
    VOICE_IDLE = 0,      /* 空闲，未在对话 */
    VOICE_LISTENING,     /* 聆听中（录音） */
    VOICE_THINKING,      /* 等待 AI 回复 */
    VOICE_SPEAKING,      /* 播放语音中 */
} voice_session_state_t;

/* 会话结束原因（供上层状态机决定迁移） */
typedef enum {
    VOICE_END_NORMAL = 0,   /* 正常结束 */
    VOICE_END_ERROR,        /* 出错中断 */
    VOICE_END_TIMEOUT,      /* 空闲超时 */
    VOICE_END_ABORT,        /* 用户打断 */
} voice_end_reason_t;

/**
 * 初始化语音子系统（当前为预留空实现，M6 填接 ES8388 + 小智）
 * 返回 ESP_OK 表示子系统可用；M5 实现返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t voice_init(void);

/**
 * 开始一次语音会话。
 * @param on_end 可选回调：会话结束时由本层回调，传结束原因 reason；可为 NULL。
 * 当前空实现直接返回 ESP_ERR_NOT_SUPPORTED（未启用）。
 * 上层可在会话开始时投递 EVT_INTERACT_START、结束时投递 EVT_INTERACT_END。
 */
esp_err_t voice_session_start(void (*on_end)(voice_end_reason_t reason));

/**
 * 结束当前会话（reason 传给 on_end），空实现。
 * 上层可在会话结束时借此收到实际结束原因。
 */
esp_err_t voice_session_stop(voice_end_reason_t reason);

/**
 * 播放语音字节流（当前空实现：播放 wav/pcm 数据），后续接 I2S/ES8388。
 * @param data 音频数据指针（PCM/WAV 等，M6 定义内部格式）
 * @param len  数据字节数
 */
esp_err_t voice_play(const uint8_t *data, size_t len);

/* 开始录音（当前空实现），后续接麦克风（ES8388 ADC 输入） */
esp_err_t voice_record_start(void);

/* 结束录音（当前空实现），后续接麦克风 */
esp_err_t voice_record_stop(void);

/* 查询当前会话状态 */
voice_session_state_t voice_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_H */