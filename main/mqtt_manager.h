#ifndef __MQTT_MANAGER_H
#define __MQTT_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 创建并启动 mqtt client（内部读取 COMPANION_MQTT_BROKER_URI）。
 * 需在 WiFi 连上后调用；断线重连由 esp-mqtt 内部处理。幂等。 */
esp_err_t mqtt_manager_start(void);

/* 上报一次 JSON 状态到主题 companion/status（含心情、RSSI、运行时长）。
 * 未连上时内部跳过并打 WARN，不阻塞。 */
void mqtt_manager_publish_status(void);

/* 上报任意 payload 到指定 topic（qos 0~2）。
 * 未连上时内部跳过并打 WARN，不阻塞。 */
void mqtt_manager_publish(const char *topic, const char *payload, int qos);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_MANAGER_H */