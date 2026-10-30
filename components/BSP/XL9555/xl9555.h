#ifndef __XL9555_H
#define __XL9555_H

#include "esp_err.h"
#include <stdbool.h>

/* XL9555 I2C 地址 */
#define XL9555_ADDR    0x20

/* XL9555 IO 位定义（16位，PORT0低8位 + PORT1高8位） */
#define AP_INT_IO      0x0001   /* PORT0 bit0: AP3216C 中断，输入 */
#define QMA_INT_IO     0x0002   /* PORT0 bit1: QMA6100P 中断，输入 */
#define SPK_EN_IO      0x0004   /* PORT0 bit2: 喇叭使能，输出 */
#define BEEP_IO        0x0008   /* PORT0 bit3: 蜂鸣器，输出 */
#define KEY0_IO        0x8000   /* PORT1 bit15: 按键0，输入 */
#define KEY1_IO        0x4000   /* PORT1 bit14: 按键1，输入 */
#define KEY2_IO        0x2000   /* PORT1 bit13: 按键2，输入 */
#define KEY3_IO        0x1000   /* PORT1 bit12: 按键3，输入 */

/* 函数声明 */
esp_err_t xl9555_init(void);
int       xl9555_pin_read(uint16_t pin);
esp_err_t xl9555_pin_write(uint16_t pin, int val);
uint8_t   xl9555_key_scan(void);

#endif
