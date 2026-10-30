#include "xl9555.h"
#include "iic.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "XL9555";
static i2c_master_dev_handle_t dev_handle = NULL;

/* XL9555 寄存器地址 */
#define REG_INPUT0    0x00   /* PORT0 输入（连续读 2 字节 = PORT0+PORT1） */
#define REG_OUTPUT0   0x02   /* PORT0 输出（连续写 2 字节 = PORT0+PORT1） */
#define REG_CONFIG0   0x06   /* PORT0 方向（连续写 2 字节 = PORT0+PORT1），1=输入 0=输出 */

/* 从 reg 开始连续读 len 字节（最多2字节，对应 PORT0/PORT1） */
static esp_err_t xl9555_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, 100);
}

/* 从 reg 开始连续写 len 字节（最多2字节） */
static esp_err_t xl9555_write(uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t buf[3];
    buf[0] = reg;
    for (size_t i = 0; i < len && i < 2; i++) {
        buf[i + 1] = data[i];
    }
    return i2c_master_transmit(dev_handle, buf, len + 1, 100);
}

esp_err_t xl9555_init(void)
{
    /* 添加 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .device_address = XL9555_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加设备失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 上电先读一次，清除中断标志 */
    uint8_t rdata[2];
    xl9555_read(REG_INPUT0, rdata, 2);

    /* 方向配置（与正点原子官方一致）：
     * PORT0 = 0x03：P0.0/P0.1 输入（AP_INT、QMA_INT），其余输出
     * PORT1 = 0xF0：P1.4~P1.7 输入（KEY3~KEY0），P1.0~P1.3 输出 */
    uint8_t cfg[2] = {0x03, 0xF0};
    xl9555_write(REG_CONFIG0, cfg, 2);

    /* 输出初值：BEEP(bit3)=1 关闭蜂鸣器（低电平触发），其余为 0 */
    uint8_t out[2] = {0x08, 0x00};
    xl9555_write(REG_OUTPUT0, out, 2);

    ESP_LOGI(TAG, "初始化成功 (0x%02X)", XL9555_ADDR);
    return ESP_OK;
}

/* 读取某个 IO 的电平，返回 1=高，0=低 */
int xl9555_pin_read(uint16_t pin)
{
    uint8_t rdata[2];
    xl9555_read(REG_INPUT0, rdata, 2);
    uint16_t val = (uint16_t)(((uint16_t)rdata[1] << 8) | rdata[0]);
    return (val & pin) ? 1 : 0;
}

/* 设置某个 IO 的输出电平，val=1 高，val=0 低 */
esp_err_t xl9555_pin_write(uint16_t pin, int val)
{
    uint8_t wdata[2];
    xl9555_read(REG_OUTPUT0, wdata, 2);
    uint16_t cur = (uint16_t)(((uint16_t)wdata[1] << 8) | wdata[0]);
    if (val) {
        cur |= pin;
    } else {
        cur &= ~pin;
    }
    wdata[0] = cur & 0xFF;
    wdata[1] = (cur >> 8) & 0xFF;
    return xl9555_write(REG_OUTPUT0, wdata, 2);
}

/* 扫描 KEY0~KEY3（接在 P1.4~P1.7，低电平有效）。
 * 返回：0=无按键，1=KEY0，2=KEY1，3=KEY2，4=KEY3。
 * 不支持连按：一次按下只返回一次键值，松开后才能再次触发。 */
uint8_t xl9555_key_scan(void)
{
    static uint8_t key_up = 1;   /* 按键松开标志 */
    uint8_t keyval = 0;

    int k0 = xl9555_pin_read(KEY0_IO);
    int k1 = xl9555_pin_read(KEY1_IO);
    int k2 = xl9555_pin_read(KEY2_IO);
    int k3 = xl9555_pin_read(KEY3_IO);

    if (key_up && (k0 == 0 || k1 == 0 || k2 == 0 || k3 == 0)) {
        vTaskDelay(pdMS_TO_TICKS(10));     /* 消抖 */
        key_up = 0;
        if (xl9555_pin_read(KEY0_IO) == 0)      keyval = 1;
        else if (xl9555_pin_read(KEY1_IO) == 0) keyval = 2;
        else if (xl9555_pin_read(KEY2_IO) == 0) keyval = 3;
        else if (xl9555_pin_read(KEY3_IO) == 0) keyval = 4;
    } else if (k0 == 1 && k1 == 1 && k2 == 1 && k3 == 1) {
        key_up = 1;                        /* 全部松开，允许下一次触发 */
    }
    return keyval;
}
