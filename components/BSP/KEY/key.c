#include "key.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 板载 BOOT 键接 GPIO0，内部上拉，按下接地读到 0 */

void key_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_KEY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

uint8_t key_scan(void)
{
    static uint8_t key_up = 1;   /* 松开标志 */
    uint8_t keyval = 0;

    if (key_up && (gpio_get_level(BOOT_KEY_PIN) == 0)) {
        vTaskDelay(pdMS_TO_TICKS(10));     /* 消抖 */
        key_up = 0;
        if (gpio_get_level(BOOT_KEY_PIN) == 0) {
            keyval = 1;
        }
    } else if (gpio_get_level(BOOT_KEY_PIN) == 1) {
        key_up = 1;
    }
    return keyval;
}
