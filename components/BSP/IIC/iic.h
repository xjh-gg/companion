#ifndef __IIC_H
#define __IIC_H

#include "driver/i2c_master.h"
#include "esp_err.h"

/* 引脚定义（正点原子 DNESP32-S3 板载硬件 I2C 总线） */
#define IIC_PORT        I2C_NUM_0
#define IIC_SDA_PIN     GPIO_NUM_41
#define IIC_SCL_PIN     GPIO_NUM_42
#define IIC_SPEED_HZ    400000

/* 总线句柄（add_device 时要用） */
extern i2c_master_bus_handle_t g_i2c_bus;

/* 函数声明 */
esp_err_t iic_init(void);
void iic_scan(void);

#endif
