#ifndef __AT24C02_H
#define __AT24C02_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AT24C02 I2C 地址（8 引脚 EEPROM，A2A1A0 接地时为 0x50） */
#define AT24C02_ADDR        0x50

/* AT24C02 容量：256 字节，8 页 × 32 字节 */
#define AT24C02_SIZE        256

/*
 * 功能开关：设为 0 时所有函数返回 ESP_ERR_NOT_SUPPORTED，
 * 便于硬件无 EEPROM 时优雅降级（config 会回退默认值）。
 */
#ifndef AT24C02_ENABLED
#define AT24C02_ENABLED     1
#endif

/* 初始化 EEPROM：登记 I2C 从设备 */
esp_err_t at24c02_init(void);

/* 写单字节（地址 + 数据两字节），写后回读校验，不一致返回错误 */
esp_err_t at24c02_write(uint8_t addr, uint8_t data);

/* 读单字节 */
esp_err_t at24c02_read(uint8_t addr, uint8_t *data);

/* 块写：从 addr 连续写 len 字节（逐字节写 + 5ms 内部写周期） */
esp_err_t at24c02_write_buffer(uint8_t addr, const uint8_t *buf, size_t len);

/* 块读：从 addr 连续读 len 字节 */
esp_err_t at24c02_read_buffer(uint8_t addr, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __AT24C02_H */