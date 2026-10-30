#include "state_machine.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "led.h"
#include "buzzer.h"
#include "event_log.h"
#include "voice_player.h"

static const char *TAG = "STATE";

/* 被关注 30s 无操作回到待机 */
#define HAPPY_TIMEOUT_MS    30000

/* 犯困定时默认 60 分钟 */
#define SLEEPY_DEFAULT_MIN  60

static companion_state_t s_state = STATE_IDLE;
static uint32_t s_sleepy_timeout_min = SLEEPY_DEFAULT_MIN;
static uint32_t s_state_enter_tick = 0;  /* 进入当前状态的 tick */
static uint32_t s_idle_elapsed_sec = 0;  /* 待机累计秒数 */

/* 状态/事件中文名（日志用） */
static const char *state_name(companion_state_t s)
{
    static const char *name[] = { "待机", "被关注", "互动中", "犯困", "报警" };
    return (s <= STATE_ALARM) ? name[s] : "未知";
}

static const char *evt_name(event_t e)
{
    switch (e) {
        case EVT_NONE:            return "无";
        case EVT_KEY_PRESSED:     return "按键";
        case EVT_PICKED_UP:       return "拿起";
        case EVT_SHAKE:           return "摇晃";
        case EVT_HAND_NEAR:       return "手靠近";
        case EVT_DARK:            return "光照过暗";
        case EVT_TIMER_SLEEPY:    return "犯困定时到";
        case EVT_ALARM:           return "紧急报警";
        case EVT_INTERACT_START:  return "开始语音对话";
        case EVT_INTERACT_END:    return "对话结束";
        default:                  return "未知";
    }
}

/* 犯困提醒提示音（嵌入式 PCM，16kHz/16bit/双声道/小端） */
extern const uint8_t sleepy_pcm_start[] asm("_binary_sleepy_pcm_start");
extern const uint8_t sleepy_pcm_end[]   asm("_binary_sleepy_pcm_end");

/* 进入状态：重置计时并联动 LED / 蜂鸣器 */
static void enter_state(companion_state_t next, event_t evt)
{
    companion_state_t prev = s_state;
    if (prev == next) {
        return;
    }
    s_state = next;
    s_state_enter_tick = xTaskGetTickCount();
    s_idle_elapsed_sec = 0;

    ESP_LOGI(TAG, "状态迁移: %s -> %s (事件: %s)",
             state_name(prev), state_name(next), evt_name(evt));

    /* M3：记录触发迁移的事件到环形日志（EVT_NONE 不记录） */
    if (evt != EVT_NONE) {
        event_log_record(evt);
    }

    switch (next) {
        case STATE_IDLE:
            led_set_mode(LED_BLINK_SLOW);
            break;
        case STATE_HAPPY:
            led_set_mode(LED_BLINK_FAST);
            buzzer_beep(80);          /* 短嘀反馈 */
            break;
        case STATE_INTERACT:
            led_set_mode(LED_BREATH); /* 随声节奏预留 */
            break;
        case STATE_SLEEPY:
            led_set_mode(LED_BLINK_SLOW);
            buzzer_beep(150);         /* 提醒休息 */
            /* 进入犯困态只触发一次提醒音（忙时由 voice_player 丢弃） */
            voice_player_play(sleepy_pcm_start,
                              (size_t)(sleepy_pcm_end - sleepy_pcm_start),
                              VOICE_SRC_SLEEPY);
            break;
        case STATE_ALARM:
            led_set_mode(LED_BLINK_ALARM);
            buzzer_beep(150);
            break;
    }
}

void state_machine_init(void)
{
    s_state = STATE_IDLE;
    s_state_enter_tick = xTaskGetTickCount();
    s_idle_elapsed_sec = 0;
    s_sleepy_timeout_min = SLEEPY_DEFAULT_MIN;
    ESP_LOGI(TAG, "状态机初始化完成，当前状态: %s", state_name(s_state));
}

void state_machine_on_event(event_t evt)
{
    if (evt == EVT_NONE) {
        return;
    }

    switch (s_state) {
        case STATE_IDLE:
            switch (evt) {
                case EVT_KEY_PRESSED:
                case EVT_PICKED_UP:
                case EVT_SHAKE:
                case EVT_HAND_NEAR:
                    enter_state(STATE_HAPPY, evt);
                    break;
                case EVT_TIMER_SLEEPY:
                    enter_state(STATE_SLEEPY, evt);
                    break;
                /* 暗光日志由 sensor_task 直接输出；EVT_DARK 当前无生产者，
                 * 即使收到也不参与状态迁移，在此被忽略 */
                case EVT_ALARM:
                    enter_state(STATE_ALARM, evt);
                    break;
                default:
                    break;
            }
            break;

        case STATE_HAPPY:
            if (evt == EVT_INTERACT_START) {
                enter_state(STATE_INTERACT, evt);
            } else if (evt == EVT_ALARM) {
                enter_state(STATE_ALARM, evt);
            } else if (evt == EVT_KEY_PRESSED || evt == EVT_PICKED_UP ||
                       evt == EVT_SHAKE || evt == EVT_HAND_NEAR) {
                /* 再次被关注：重置 30s 无操作计时 */
                s_state_enter_tick = xTaskGetTickCount();
            }
            break;

        case STATE_INTERACT:
            if (evt == EVT_INTERACT_END) {
                enter_state(STATE_HAPPY, evt);    /* 对话结束 -> 被关注 */
            } else if (evt == EVT_ALARM) {
                enter_state(STATE_ALARM, evt);
            }
            break;

        case STATE_SLEEPY:
            if (evt == EVT_KEY_PRESSED) {
                enter_state(STATE_IDLE, evt);     /* 按键确认 -> 待机 */
            } else if (evt == EVT_ALARM) {
                enter_state(STATE_ALARM, evt);
            }
            break;

        case STATE_ALARM:
            if (evt == EVT_KEY_PRESSED) {
                enter_state(STATE_IDLE, evt);     /* 按键确认 -> 待机 */
            }
            break;

        default:
            break;
    }
}

void state_machine_tick(void)
{
    uint32_t now = xTaskGetTickCount();

    /* 被关注 30s 无操作 -> 待机 */
    if (s_state == STATE_HAPPY) {
        if (now - s_state_enter_tick >= pdMS_TO_TICKS(HAPPY_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "被关注 30s 无操作，回到待机");
            enter_state(STATE_IDLE, EVT_NONE);
        }
        return;
    }

    /* 犯困定时：仅在待机状态累计 */
    if (s_state == STATE_IDLE) {
        s_idle_elapsed_sec++;
        if (s_idle_elapsed_sec >= s_sleepy_timeout_min * 60) {
            s_idle_elapsed_sec = 0;
            ESP_LOGI(TAG, "犯困定时到（%u 分钟）", (unsigned)s_sleepy_timeout_min);
            enter_state(STATE_SLEEPY, EVT_TIMER_SLEEPY);
        }
        return;
    }

    /* 报警基线：周期性短鸣提醒 */
    if (s_state == STATE_ALARM) {
        buzzer_beep(100);
    }
}

companion_state_t state_machine_get_state(void)
{
    return s_state;
}

void state_machine_set_sleepy_timeout_min(uint32_t min)
{
    if (min == 0) {
        return;
    }
    s_sleepy_timeout_min = min;
    ESP_LOGI(TAG, "犯困定时设置为 %u 分钟", (unsigned)min);
}

uint32_t state_machine_get_sleepy_timeout_min(void)
{
    return s_sleepy_timeout_min;
}