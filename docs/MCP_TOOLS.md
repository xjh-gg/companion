# MCP TOOLS — 设备能力

## 1. `self.companion.get_status`

只读，无参数。

返回主要字段：

| 字段 | 含义 |
|---|---|
| `state` / `state_code` / `state_duration_ms` | 陪伴状态及持续时间 |
| `sleepy_timeout_min` / `sleepy_elapsed_sec` / `sleepy_remaining_sec` | 犯困计时 |
| `als` / `ps` | 最近有效原始值（AP3216C） |
| `light_dark` | 当前是否处于暗光判定 |
| `last_light_event` / `last_light_event_age_ms` | 最近光照事件 |
| `gx/gy/gz` | 加速度（g） |
| `ap3216c_ok` / `qma6100p_ok` / `sensor_ok` | 最近读取与新鲜度 |
| `ap_data_fresh` / `qma_data_fresh` | 2 秒新鲜度 |
| `last_event` / `last_event_age_ms` | 最近事件 |
| `speech_state` | XiaoZhi 语音状态（idle/listening/speaking/…） |
| `network_connected` | WiFi 连接状态 |
| `mqtt_state` | `unsupported`（当前无公开 MQTT 连接查询接口） |
| `event_total` | 累计事件条数 |
| `event_queue_dropped` / `event_queue_depth` | 队列诊断 |
| `led_available` / `led_state` | `false` / `placeholder` |
| `actuator_state` | `not_installed` |
| `last_error` / `last_error_age_ms` | 最近传感器错误（存在时返回） |

## 2. `self.companion.get_recent_events`

参数：

```json
{ "limit": 8 }
```

- `limit`：整数，1–24，默认 8。
- 返回数组（最新在前）：

```json
[
  { "seq": 42, "uptime_ms": 125000, "type": "手靠近", "als": 112, "ps": 68, "valid": true }
]
```

- 无历史返回 `[]`；超过 24 条按环形覆盖。
- 该工具只读，不触发任何硬件动作。

## 3. 未开放的工具

| 工具 | 原因 |
|---|---|
| `self.companion.set_led_state` | LED 由 XiaoZhi `SingleLed(GPIO1)` 独占，未确认安全仲裁；不猜 GPIO |
| 执行器/电机工具 | 无硬件与驱动；未实现白名单/超时停止前不开放 |

## 4. 验收状态

- 本地注册：**已确认**（启动日志 `MCP: Add tool`）。
- 参数校验：`limit` 范围由 MCP 框架的 Property min/max 校验。
- 端到端调用：**待人工**（需要服务端/客户端实际调用 `tools/call`）。
