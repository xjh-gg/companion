#include "at24c02.h"

#include "iic.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AT24C02";
static i2c_master_dev_handle_t dev_handle = NULL;

/* 单次字节写后需要等待的内部写周期（AT24C02 最大 5ms）。
 * 采用 ACK 轮询等待写周期结束：写周期内芯片不响应 I2C(NACK)，
 * 轮询回读到重新 ACK 即代表编程完成，比固定延时更可靠。 */
#define AT24C02_WRITE_DELAY_MS   10             /* 轮询间隔 */
#define AT24C02_POLL_MAX_TRIES   50             /* 轮询上限（总约 500ms） */

/* 功能开关：关闭时整模块降级 */
#if AT24C02_ENABLED

esp_err_t at24c02_init(void)
{
    i2c_device_config_t dev_cfg = {
        .device_address = AT24C02_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加设备失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "初始化成功 (0x%02X)", AT24C02_ADDR);
    return ESP_OK;
}

esp_err_t at24c02_write(uint8_t addr, uint8_t data)
{
    /* addr 为 uint8_t（0~255），无需范围检查：编译器已证 256 越界不可能发生。
     * 越界防护由块操作（size_t 运算）承担。 */
    uint8_t buf[2] = {addr, data};
    esp_err_t ret = i2c_master_transmit(dev_handle, buf, 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "写失败(addr=0x%02X): %s", addr, esp_err_to_name(ret));
        return ret;
    }
    /* 等待内部写周期完毕：写周期内芯片不响应 I2C(NACK)，轮询回读直至重新
     * 应答(ACK)，比固定延时更可靠地规避 tWR 超时导致的 "回读失败"。 */
    uint8_t verify = 0;
    int tries = 0;
    do {
        vTaskDelay(pdMS_TO_TICKS(AT24C02_WRITE_DELAY_MS));
        ret = at24c02_read(addr, &verify);
        tries++;
    } while (ret != ESP_OK && tries < AT24C02_POLL_MAX_TRIES);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "回读失败(addr=0x%02X, %d 次): %s", addr, tries,
                 esp_err_to_name(ret));
        return ret;
    }
    if (verify != data) {
        ESP_LOGW(TAG, "写入校验失败(addr=0x%02X): 期望 0x%02X, 实际 0x%02X",
                 addr, data, verify);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t at24c02_read(uint8_t addr, uint8_t *data)
{
    if (data == NULL) {
        ESP_LOGE(TAG, "读参数非法: data=NULL");
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(dev_handle, &addr, 1, data, 1, 100);
}

static esp_err_t at24c02_write_buffer_impl(uint8_t addr, const uint8_t *buf, size_t len)
{
    size_t start = addr;
    if (start > AT24C02_SIZE || len > (AT24C02_SIZE - start)) {
        ESP_LOGE(TAG, "块写越界: addr=0x%02X len=%u, 超出 256B 范围",
                 addr, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 逐字节写 + 5ms 内部写周期：规避页末尾回绕问题，简单可靠 */
    for (size_t i = 0; i < len; i++) {
        esp_err_t ret = at24c02_write((uint8_t)(addr + i), buf[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "块写中断于第 %u 字节", (unsigned)i);
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t at24c02_read_buffer_impl(uint8_t addr, uint8_t *buf, size_t len)
{
    size_t start = addr;
    if (start > AT24C02_SIZE || len > (AT24C02_SIZE - start)) {
        ESP_LOGE(TAG, "块读越界: addr=0x%02X len=%u, 超出 256B 范围",
                 addr, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 借用 I2C 总线的 transmit_receive 连续读取（AT24C02 支持顺序读） */
    uint8_t reg = addr;
    return i2c_master_transmit_receive(dev_handle, &reg, 1, buf, len, 100);
}

esp_err_t at24c02_write_buffer(uint8_t addr, const uint8_t *buf, size_t len)
{
    return at24c02_write_buffer_impl(addr, buf, len);
}

esp_err_t at24c02_read_buffer(uint8_t addr, uint8_t *buf, size_t len)
{
    return at24c02_read_buffer_impl(addr, buf, len);
}

#else /* !AT24C02_ENABLED */
/* 降级：全部返回不支持，config 走默认值回退 */

esp_err_t at24c02_init(void)
{
    ESP_LOGW(TAG, "未启用，初始化跳过");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t at24c02_write(uint8_t addr, uint8_t data)
{
    ESP_LOGW(TAG, "未启用，写入跳过 (addr=0x%02X)", addr);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t at24c02_read(uint8_t addr, uint8_t *data)
{
    ESP_LOGW(TAG, "未启用，读取跳过 (addr=0x%02X)", addr);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t at24c02_write_buffer(uint8_t addr, const uint8_t *buf, size_t len)
{
    ESP_LOGW(TAG, "未启用，块写跳过 (addr=0x%02X len=%u)", addr, (unsigned)len);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t at24c02_read_buffer(uint8_t addr, uint8_t *buf, size_t len)
{
    ESP_LOGW(TAG, "未启用，块读跳过 (addr=0x%02X len=%u)", addr, (unsigned)len);
    return ESP_ERR_NOT_SUPPORTED;
}
#endif /* AT24C02_ENABLED */