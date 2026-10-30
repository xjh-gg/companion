#include "buzzer.h"
#include "xl9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 板载有源蜂鸣器接在 XL9555 的 P0.3（BEEP_IO），低电平触发：
 * BEEP=0 鸣叫，BEEP=1 关闭 */

/* 编译开关：0=测试期间屏蔽蜂鸣器鸣叫（on/beep 为空操作），1=使能 */
#ifndef BUZZER_ENABLED
#define BUZZER_ENABLED 0
#endif

void buzzer_init(void)
{
    xl9555_pin_write(BEEP_IO, 1);   /* 初始关闭（无论开关，确保静音） */
}

void buzzer_on(void)
{
#if BUZZER_ENABLED
    xl9555_pin_write(BEEP_IO, 0);
#endif
}

void buzzer_off(void)
{
    xl9555_pin_write(BEEP_IO, 1);
}

void buzzer_beep(uint32_t ms)
{
#if BUZZER_ENABLED
    (void)ms;
    buzzer_on();
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_off();
#endif
}
