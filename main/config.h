#ifndef __CONFIG_H
#define __CONFIG_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 默认配置值 */
#define DEFAULT_SLEEPY_MIN   60    /* 犯困提醒间隔（分钟） */
#define DEFAULT_VOLUME       80    /* 音量 0~100（预留） */
#define DEFAULT_MODE_ENABLED 1     /* 模式开关 1=使能（预留） */

/* 配置结构体（存储于 AT24C02 从地址 0 开始的区域） */
typedef struct {
    uint32_t sleepy_timeout_min;   /* 犯困提醒间隔（分钟）默认 60 */
    uint8_t  volume;               /* 音量 0~100，默认 80（预留） */
    uint8_t  mode_enabled;         /* 模式开关 1=使能，0=禁用（预留） */
    char     wifi_ssid[33];        /* WiFi SSID（运行时写入 AT24C02，不入固件） */
    char     wifi_password[65];    /* WiFi 密码（运行时写入 AT24C02，不入固件） */
    char     mqtt_broker_uri[64];  /* MQTT broker URI，如 mqtt://ip:port */
    uint8_t  check;                /* 校验字节（其余字段异或） */
} companion_config_t;

/* 初始化配置：从 AT24C02 读取，失败/校验不通过则回退默认值并写回 */
void config_init(void);

/* 获取当前配置快照 */
companion_config_t config_get(void);

/* 写配置回 EEPROM（自动更新校验字节） */
esp_err_t config_save(const companion_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* __CONFIG_H */