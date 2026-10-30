#ifndef __BUZZER_H
#define __BUZZER_H

#include <stdint.h>

/* 蜂鸣器初始化（关闭蜂鸣器）。依赖 xl9555_init() 先执行 */
void buzzer_init(void);

/* 蜂鸣器开 / 关（有源蜂鸣器，低电平触发） */
void buzzer_on(void);
void buzzer_off(void);

/* 鸣叫 ms 毫秒（阻塞式，内部用 vTaskDelay） */
void buzzer_beep(uint32_t ms);

#endif
