#include "qma6100p.h"
#include "iic.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QMA6100P";
static i2c_master_dev_handle_t dev_handle = NULL;

/* ====== 寄存器地址 ====== */
#define REG_CHIP_ID      0x00   /* 设备 ID，默认 0x90 */
#define REG_XOUT_L       0x01   /* X 轴低 8 位 */
#define REG_XOUT_H       0x02   /* X 轴高 8 位 */
#define REG_YOUT_L       0x03
#define REG_YOUT_H       0x04
#define REG_ZOUT_L       0x05
#define REG_ZOUT_H       0x06
#define REG_RANGE        0x0F   /* 量程设置 */
#define REG_BW_ODR       0x10   /* 带宽 / 输出数据率 */
#define REG_PWR_MGMT     0x11   /* 电源管理，bit7=active */
#define REG_RESET        0x36   /* 软复位，写 0xB6 */

/* 8G 量程下每 LSB 对应的 g 数（14 位 ADC，±8g → 16384 LSB/g） */
#define SENSITIVITY_8G   (1.0f / 1024.0f)

/* I2C 读取失败重试次数（需求：重试至多 3 次） */
#define QMA_I2C_RETRY    3

/* 写单个寄存器 */
static esp_err_t qma_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

/* 从 reg 开始连续读 len 字节（带重试，避免瞬时 I2C 错误中断主流程） */
static esp_err_t qma_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < QMA_I2C_RETRY; i++) {
        ret = i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, 100);
        if (ret == ESP_OK) return ESP_OK;      /* 成功即返回 */
        if (i < QMA_I2C_RETRY - 1) {
            vTaskDelay(pdMS_TO_TICKS(5));       /* 重试前短暂等待 */
        }
    }
    ESP_LOGW(TAG, "I2C 读取失败重试 %d 次仍失败: %s",
             QMA_I2C_RETRY, esp_err_to_name(ret));
    return ret;
}

esp_err_t qma6100p_read_chip_id(uint8_t *id)
{
    return qma_read_reg(REG_CHIP_ID, id, 1);
}

esp_err_t qma6100p_init(void)
{
    /* 添加 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .device_address = QMA6100P_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加设备失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 读取芯片 ID 验证设备 */
    uint8_t id = 0;
    qma6100p_read_chip_id(&id);
    if (id != QMA6100P_CHIP_ID) {
        ESP_LOGE(TAG, "芯片 ID 不匹配: 期望 0x%02X, 实际 0x%02X", QMA6100P_CHIP_ID, id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "芯片 ID 校验通过: 0x%02X", id);

    /* 软复位 */
    qma_write_reg(REG_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(5));
    qma_write_reg(REG_RESET, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ====== 手册 6.3 Initial sequence 要求的初始化序列 ====== */
    qma_write_reg(REG_PWR_MGMT, 0x80);     /* standby */
    qma_write_reg(REG_PWR_MGMT, 0x84);
    qma_write_reg(0x4a, 0x20);
    qma_write_reg(0x56, 0x01);
    qma_write_reg(0x5f, 0x80);
    vTaskDelay(pdMS_TO_TICKS(1));
    qma_write_reg(0x5f, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 设置量程 8G */
    qma_write_reg(REG_RANGE, QMA6100P_RANGE_8G);
    /* 设置带宽 100Hz */
    qma_write_reg(REG_BW_ODR, 0x00);
    /* 激活（active 模式，bit7=1，MCLK=51.2KHz） */
    qma_write_reg(REG_PWR_MGMT, 0x80 | 0x04);

    ESP_LOGI(TAG, "初始化成功 (0x%02X) 量程=8G 带宽=100Hz", QMA6100P_ADDR);
    return ESP_OK;
}

esp_err_t qma6100p_read_raw(qma6100p_raw_t *raw)
{
    uint8_t buf[6];
    esp_err_t ret = qma_read_reg(REG_XOUT_L, buf, 6);
    if (ret != ESP_OK) return ret;

    /* 14 位有符号数，低字节在前，右移 2 位对齐 */
    int16_t x = (int16_t)((buf[1] << 8) | buf[0]) >> 2;
    int16_t y = (int16_t)((buf[3] << 8) | buf[2]) >> 2;
    int16_t z = (int16_t)((buf[5] << 8) | buf[4]) >> 2;

    raw->x = x;
    raw->y = y;
    raw->z = z;
    return ESP_OK;
}

esp_err_t qma6100p_read_g(float *gx, float *gy, float *gz)
{
    qma6100p_raw_t raw;
    esp_err_t ret = qma6100p_read_raw(&raw);
    if (ret != ESP_OK) return ret;

    *gx = raw.x * SENSITIVITY_8G;
    *gy = raw.y * SENSITIVITY_8G;
    *gz = raw.z * SENSITIVITY_8G;
    return ESP_OK;
}
