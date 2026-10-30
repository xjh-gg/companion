#ifndef __QMA6100P_H
#define __QMA6100P_H

#include "esp_err.h"
#include <stdint.h>

/* QMA6100P I2C 地址（AD0 接 GND 时为 0x12） */
#define QMA6100P_ADDR        0x12

/* 设备 ID（CHIP_ID 寄存器默认值） */
#define QMA6100P_CHIP_ID     0x90

/* 量程枚举 */
typedef enum {
    QMA6100P_RANGE_2G  = 0x01,
    QMA6100P_RANGE_4G  = 0x02,
    QMA6100P_RANGE_8G  = 0x04,
    QMA6100P_RANGE_16G = 0x08,
} qma6100p_range_t;

/* 三轴原始数据（14 位有符号，右对齐） */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} qma6100p_raw_t;

/* 初始化 QMA6100P：软复位 + 量程 8G + 带宽 100Hz + 激活 */
esp_err_t qma6100p_init(void);

/* 读取芯片 ID，用于检测设备是否在线 */
esp_err_t qma6100p_read_chip_id(uint8_t *id);

/* 读取三轴加速度原始值 */
esp_err_t qma6100p_read_raw(qma6100p_raw_t *raw);

/* 读取三轴加速度（单位 g，浮点数） */
esp_err_t qma6100p_read_g(float *gx, float *gy, float *gz);

#endif
