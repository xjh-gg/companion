#include "led.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "LED";

/* LEDC 配置：低速通道，8 位分辨率，1kHz */
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_8_BIT
#define LEDC_FREQ_HZ        1000

/* 定时器周期 50ms，所有模式在此节拍上计数 */
#define TICK_PERIOD_MS      50

static led_mode_t s_mode = LED_OFF;
static esp_timer_handle_t s_timer = NULL;
static uint32_t s_tick = 0;          /* 节拍计数 */
static int s_breathe_dir = 1;        /* 呼吸方向：1=渐亮，-1=渐暗 */
static uint32_t s_breathe_duty = 0;  /* 呼吸当前占空比 */

/* 设置 LEDC 占空比（0~255） */
static void led_set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/* 50ms 定时器回调：根据当前模式驱动 LED */
static void led_timer_cb(void *arg)
{
    s_tick++;

    switch (s_mode) {
    case LED_OFF:
        led_set_duty(0);
        break;

    case LED_ON:
        led_set_duty(255);
        break;

    case LED_BLINK_SLOW:
        /* 2 秒一周期 = 40 个 tick，亮 20 / 灭 20 */
        led_set_duty((s_tick % 40 < 20) ? 255 : 0);
        break;

    case LED_BLINK_FAST:
        /* 0.2 秒一周期 = 4 个 tick，亮 2 / 灭 2 */
        led_set_duty((s_tick % 4 < 2) ? 255 : 0);
        break;

    case LED_BLINK_ALARM:
        /* 0.1 秒一周期 = 2 个 tick，亮 1 / 灭 1 */
        led_set_duty((s_tick % 2) ? 255 : 0);
        break;

    case LED_BREATH:
        /* 呼吸：占空比 0↔255 渐变，每 tick 变化 3，约 4 秒一轮回 */
        s_breathe_duty += s_breathe_dir * 3;
        if (s_breathe_duty >= 255) {
            s_breathe_duty = 255;
            s_breathe_dir = -1;
        } else if (s_breathe_duty == 0) {
            s_breathe_dir = 1;
        }
        led_set_duty(s_breathe_duty);
        break;

    default:
        led_set_duty(0);
        break;
    }
}

void led_init(void)
{
    /* LEDC 定时器配置 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    /* LEDC 通道配置，绑定到 GPIO1，开启输出反转（LED 低电平点亮） */
    ledc_channel_config_t ch_cfg = {
        .gpio_num = LED_PIN,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,    /* 反转输出：duty 越大 LED 越亮 */
    };
    ledc_channel_config(&ch_cfg);

    /* 创建 50ms 周期软件定时器 */
    esp_timer_create_args_t timer_args = {
        .callback = led_timer_cb,
        .name = "led_timer",
    };
    esp_timer_create(&timer_args, &s_timer);
    esp_timer_start_periodic(s_timer, TICK_PERIOD_MS * 1000);

    s_mode = LED_OFF;
    ESP_LOGI(TAG, "初始化成功 GPIO%d", LED_PIN);
}

void led_set_mode(led_mode_t mode)
{
    if (mode == s_mode) return;
    s_mode = mode;
    s_tick = 0;
    s_breathe_duty = 0;
    s_breathe_dir = 1;
    ESP_LOGD(TAG, "切换模式 -> %d", mode);
}

led_mode_t led_get_mode(void)
{
    return s_mode;
}
