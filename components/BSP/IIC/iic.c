#include "iic.h"
#include "esp_log.h"

static const char *TAG = "IIC";

i2c_master_bus_handle_t g_i2c_bus = NULL;

esp_err_t iic_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = IIC_PORT,
        .sda_io_num                   = IIC_SDA_PIN,
        .scl_io_num                   = IIC_SCL_PIN,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup  = true,
    };

    esp_err_t ret = i2c_new_master_bus(&cfg, &g_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C 初始化成功  SDA=%d SCL=%d %dHz",
             IIC_SDA_PIN, IIC_SCL_PIN, IIC_SPEED_HZ);
    return ESP_OK;
}

void iic_scan(void)
{
    ESP_LOGI(TAG, "扫描 I2C 总线...");
    int found = 0;
    for (uint8_t addr = 1; addr < 128; addr++) {
        if (i2c_master_probe(g_i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  -> 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "  未发现任何设备");
    else            ESP_LOGI(TAG, "共 %d 个设备在线", found);
}
