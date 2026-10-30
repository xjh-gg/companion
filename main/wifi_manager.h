#ifndef __WIFI_MANAGER_H
#define __WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 WiFi（NVS + 事件注册 + STA 配置 + 非阻塞连接）。
 * 调用后立即返回，WiFi 连接在后台进行，断线自动重连，不阻塞调用方。 */
esp_err_t wifi_manager_init(void);

/* 当前是否已连上（已拿到 IP）。 */
bool wifi_manager_is_connected(void);

/* 当前 RSSI（dBm）。-127 表示不可用。 */
int32_t wifi_manager_get_rssi(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_MANAGER_H */