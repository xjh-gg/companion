#ifndef __KEY_H
#define __KEY_H

#include <stdint.h>

/* 板载 BOOT 键，接 GPIO0，低电平有效（按下为 0） */
#define BOOT_KEY_PIN     0

/* 初始化 BOOT 按键为上拉输入 */
void key_init(void);

/* 扫描 BOOT 键。返回：1=按下，0=无。
 * 不支持连按：一次按下只返回一次，松开后才能再次触发 */
uint8_t key_scan(void);

#endif
