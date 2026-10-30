# ARCHITECTURE — AI Companion

> **历史说明**：本文档部分内容来自阶段一自研原型；当前 XiaoZhi 交付实现以 `docs/phase2_acceptance_20261022/PHASE2_FINAL_ACCEPTANCE_REPORT.md` 为准。

## 分层

```
┌────────────────────────────────────────────┐
│ AI / MCP（XiaoZhi 现有）                    │
│  get_status / get_recent_events            │
├────────────────────────────────────────────┤
│ State（应用层）                             │
│  state_machine.c + companion_service.cc    │
├────────────────────────────────────────────┤
│ Event（应用层）                             │
│  event_queue.c + companion_event_log.c     │
├────────────────────────────────────────────┤
│ Sensor / Input（应用层 + BSP 驱动）          │
│  AP3216C、QMA6100P、motion_detect          │
├────────────────────────────────────────────┤
│ Platform（XiaoZhi / ESP-IDF，不改动）       │
│  Application、AudioService、MQTT、ES8388   │
└────────────────────────────────────────────┘
```

## 任务职责

| 任务/上下文 | 职责 |
|---|---|
| `companion_sensor`（50ms，优先级 3） | 读 AP3216C/QMA6100P；光照/接近边沿事件；快照更新 |
| `companion_main`（100ms，优先级 2） | 消费事件队列、记录历史、状态机 tick、轮询语音状态、心跳 |
| `companion_audio`（优先级 2） | 仅调用 `Application::PlaySound` 播放犯困提醒 |
| MQTT 事件任务 / `udp_receive` / `main_event_loop` | XiaoZhi 原有语音/协议/AES 链路，未修改 |
| MCP 回调 | 在协议任务执行；只读快照、事件历史；不阻塞 |

## 数据流

1. 传感器任务采样 → 去抖/阈值 → `event_queue_send`（光照/接近/运动）。
2. 主任务排空队列 → `state_machine_on_event` + `companion_event_log_record`。
3. 主任务轮询 XiaoZhi `DeviceState` → 产生 `EVT_INTERACT_START/END`（直送状态机并记历史）。
4. MCP `get_status` 读互斥快照；`get_recent_events` 从环形历史复制，最新在前。
5. 状态变化 → `companion_led_request`（占位）、`companion_audio_request_reminder`（仅犯困）。

## 并发与安全

- 传感器快照：`std::mutex s_snapshot_mutex`。
- 事件历史：`portMUX_TYPE` 临界区；单写（主任务）多读（MCP）。
- 音频：只使用官方 `Application::PlaySound`/`AudioService::IsIdle`，不初始化硬件。
- 不对未知 GPIO 输出；执行器未接入。
