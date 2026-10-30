#ifndef __EVENT_LOG_H
#define __EVENT_LOG_H

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化环形事件日志：校验头部，非法则清空（EEPROM 从偏移 16 起的 240B） */
esp_err_t event_log_init(void);

/* 记录一条事件，索引自动回绕（TTL 最新覆盖最旧） */
esp_err_t event_log_record(event_t evt);

/*
 * 按时间从新到旧读取最多 max 条到 out，返回实际条数（<= max）。
 * out 为空或 max<=0 时返回 0。
 */
int event_log_read_recent(event_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* __EVENT_LOG_H */