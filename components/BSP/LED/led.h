#ifndef __LED_H
#define __LED_H

#include <stdint.h>

/* 板载红色 LED 接 GPIO1，低电平点亮（共阳极） */
#define LED_PIN     GPIO_NUM_1

/* LED 情绪模式（对应状态机的 5 种心情） */
typedef enum {
    LED_OFF = 0,         /* 灭灯 */
    LED_ON,              /* 常亮 */
    LED_BLINK_SLOW,      /* 慢闪：待机平静，2 秒一周期 */
    LED_BLINK_FAST,      /* 快闪：被关注开心，0.2 秒一周期 */
    LED_BLINK_ALARM,     /* 急促闪：报警，0.1 秒一周期 */
    LED_BREATH,          /* 呼吸：柔和渐变（PWM 方式） */
} led_mode_t;

/* 初始化 LED（GPIO 输出 + 软件定时器） */
void led_init(void);

/* 设置 LED 情绪模式（非阻塞，内部定时器自动翻转） */
void led_set_mode(led_mode_t mode);

/* 获取当前模式 */
led_mode_t led_get_mode(void);

#endif
