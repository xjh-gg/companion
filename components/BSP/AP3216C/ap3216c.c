#include "ap3216c.h"
#include "iic.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AP3216C";
static i2c_master_dev_handle_t dev_handle = NULL;

/* I2C 读取失败重试次数（需求：重试至多 3 次） */
#define AP32_I2C_RETRY    3

/* ====== 寄存器地址 ====== */
#define REG_SYSTEM_CONFIG   0x00   /* 系统配置 / 模式控制 */
#define REG_INT_STATUS      0x01   /* 中断状态 */
#define REG_INT_CLEAR       0x02   /* 中断清除 */
#define REG_IR_DATA_LOW     0x0A   /* IR 数据低字节 */
#define REG_IR_DATA_HIGH    0x0B   /* IR 数据高字节 */
#define REG_ALS_DATA_LOW    0x0C   /* ALS 数据低字节 */
#define REG_ALS_DATA_HIGH   0x0D   /* ALS 数据高字节 */
#define REG_PS_DATA_LOW     0x0E   /* PS 数据低字节 */
#define REG_PS_DATA_HIGH    0x0F   /* PS 数据高字节 */

/* 模式值 */
#define MODE_POWER_DOWN     0x00   /* 掉电 */
#define MODE_ENABLE_ALL     0x03   /* 启用 ALS + PS + IR */
#define MODE_SW_RESET       0x04   /* 软件复位 */

/* 写单个寄存器 */
static esp_err_t ap32_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

/* 从 reg 开始连续读 len 字节（带重试，避免瞬时 I2C 错误中断主流程） */
static esp_err_t ap32_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < AP32_I2C_RETRY; i++) {
        ret = i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, 100);
        if (ret == ESP_OK) return ESP_OK;      /* 成功即返回 */
        if (i < AP32_I2C_RETRY - 1) {
            vTaskDelay(pdMS_TO_TICKS(5));       /* 重试前短暂等待 */
        }
    }
    ESP_LOGW(TAG, "I2C 读取失败重试 %d 次仍失败: %s",
             AP32_I2C_RETRY, esp_err_to_name(ret));
    return ret;
}

esp_err_t ap3216c_init(void)
{
    /* 添加 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .device_address = AP3216C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加设备失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 软件复位 */
    ap32_write_reg(REG_SYSTEM_CONFIG, MODE_SW_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 启用 ALS + PS + IR */
    ap32_write_reg(REG_SYSTEM_CONFIG, MODE_ENABLE_ALL);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* AP3216C 无 chip ID，通过写读 SYSTEM_CONFIG 验证设备 */
    uint8_t val = 0;
    ap32_read_reg(REG_SYSTEM_CONFIG, &val, 1);
    if (val != MODE_ENABLE_ALL) {
        ESP_LOGE(TAG, "设备验证失败: 期望 0x%02X, 实际 0x%02X", MODE_ENABLE_ALL, val);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "初始化成功 (0x%02X) 模式=ALS+PS+IR", AP3216C_ADDR);
    return ESP_OK;
}

esp_err_t ap3216c_read(ap3216c_data_t *data)
{
    uint8_t buf[6];
    /* 与正点原子官方例程一致：0x0A~0x0F 逐寄存器读取，每次 1 字节；
     * 任意一次失败立即返回，不解析不完整数据 */
    for (uint8_t i = 0; i < 6; i++) {
        esp_err_t ret = ap32_read_reg(REG_IR_DATA_LOW + i, &buf[i], 1);
        if (ret != ESP_OK) return ret;
    }

    /* IR：bit7 为溢出标志，1=无效 */
    if (buf[0] & 0x80) {
        data->ir = 0;
    } else {
        data->ir = ((uint16_t)buf[1] << 2) | (buf[0] & 0x03);
    }

    /* ALS：16 位 */
    data->als = ((uint16_t)buf[3] << 8) | buf[2];

    /* PS：bit6 为溢出标志，1=无效；有效位为 bit11:4 */
    if (buf[4] & 0x40) {
        data->ps = 0;
    } else {
        data->ps = ((uint16_t)(buf[5] & 0x3F) << 4) | (buf[4] & 0x0F);
    }

    return ESP_OK;
}

esp_err_t ap3216c_read_als(uint16_t *als)
{
    ap3216c_data_t d;
    esp_err_t ret = ap3216c_read(&d);
    if (ret == ESP_OK) *als = d.als;
    return ret;
}

esp_err_t ap3216c_read_ps(uint16_t *ps)
{
    ap3216c_data_t d;
    esp_err_t ret = ap3216c_read(&d);
    if (ret == ESP_OK) *ps = d.ps;
    return ret;
}
