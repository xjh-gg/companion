#ifndef __AP3216C_H
#define __AP3216C_H

#include "esp_err.h"
#include <stdint.h>

/* AP3216C I2C 地址（固定 0x1E） */
#define AP3216C_ADDR        0x1E

/* 三合一传感器数据 */
typedef struct {
    uint16_t als;    /* 环境光强度（0~65535，16 位） */
    uint16_t ps;     /* 接近距离（0~1023，10 位） */
    uint16_t ir;     /* 红外强度（0~1023，10 位） */
} ap3216c_data_t;

/* 初始化 AP3216C：软复位 + 启用 ALS+PS+IR */
esp_err_t ap3216c_init(void);

/* 读取 ALS / PS / IR 三合一数据 */
esp_err_t ap3216c_read(ap3216c_data_t *data);

/* 单独读取环境光强度 */
esp_err_t ap3216c_read_als(uint16_t *als);

/* 单独读取接近值 */
esp_err_t ap3216c_read_ps(uint16_t *ps);

#endif
