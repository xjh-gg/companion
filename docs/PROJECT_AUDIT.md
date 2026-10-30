# PROJECT AUDIT — AI Companion 多模态具身终端

审计时间：2026-10-22 15:30
审计范围：当前实际构建源 `<xiaozhi_source>`；工作区 `D:\Projects\workspace\companion`。

## 1. 工程定位

| 项 | 实际情况 |
|---|---|
| 产品固件源 | XiaoZhi 2.0.3 源码 + `main/companion/` 应用层 |
| 工作区 | `D:\Projects\workspace\companion` 保留旧自研工程、文档、备份与主机测试；不是当前构建源 |
| 构建目录 | `build_companion` |
| IDF / 目标 | ESP-IDF v6.1.0 / ESP32-S3（ATK-DNESP32S3） |
| 音频 | ES8388 + XiaoZhi `AudioService`（未改动） |
| 传感器 | AP3216C（0x1E）、QMA6100P（0x12），共享板级 I2C |
| 网络/MCP | XiaoZhi MQTT 协议 + 现有 MCP 框架 |

## 2. 架构与主要调用链

```
Board 构造 (atk_dnesp32s3.cc)
  → InitializeI2c / SPI / Display / Buttons / Camera
  → companion_service_start(i2c_bus_)
      → companion_led_init / companion_audio_init
      → ap3216c_init / qma6100p_init
      → event_queue_init / companion_event_log_init / state_machine_init
      → companion_sensor_task (50ms) / companion_main_task (100ms)
      → MCP: get_status / get_recent_events

语音：Application::MainEventLoop → Protocol → AudioService → ES8388
状态：companion_main_task 轮询 Application::GetDeviceState()
```

关键文件：
- `main/companion/companion_service.cc`
  - 状态 JSON：L106
  - 传感器任务：L181
  - 主任务：L323
  - 服务启动：L386
  - MCP 注册：L423、L432
- `main/companion/companion_event_log.c`：init L15、record L26、copy L48
- `main/companion/state_machine.c`：5 状态迁移
- `main/companion/event_queue.c`：深度 8、丢弃计数
- `main/protocols/mqtt_protocol.cc`：AES 互斥、ServerHello 原子提交（已保留）
- `main/application.cc` / `main/audio/audio_service.cc`：官方音频链路（未改动）

## 3. 功能状态

| 功能 | 状态 | 证据 |
|---|---|---|
| 小智唤醒/ASR/LLM/TTS | 已实现，已实机验证 1 轮（旧 AES 修复固件） | `Wake word detected: 你好小智`、`>>`/`<<`、`STATE: listening/speaking` |
| MQTT AES 并发修复 | 已实现，已保留 | `aes_mutex_`、ServerHello 原子提交、序号处理；构建通过 |
| AP3216C 光照/接近 | 已实现，实机读数正常 | 心跳 ALS≈108–114、PS 0–18、`传感器=ok/ok` |
| QMA6100P 拿起/摇晃 | 已实现，未做现场动作验证 | `motion_detect.c` 阈值 0.45g/400ms、3 次/1s |
| 光照事件 | 已实现，未做遮挡验证 | ALS<50/≥60、3 次去抖，产生 `EVT_DARK`/`EVT_BRIGHT` |
| 事件队列/历史 | 已实现 | 深度 8；环形历史 24 条（新模块） |
| 状态机 | 已实现，主机测试覆盖 | 59 PASS / 0 FAIL |
| MCP `get_status` | 已实现并注册 | 启动日志 `MCP: Add tool` |
| MCP `get_recent_events` | 已实现并注册，未端到端调用 | 同上 |
| LED 状态反馈 | **未实现（占位）** | `companion_led.cc` 只打印日志；官方 `SingleLed(GPIO1)` 独占 |
| 执行器/电机 | **未实现** | 无硬件与驱动 |
| MQTT 自定义遥测 | **未实现（协议未扩展）** | 复用 XiaoZhi 会话，不新增 topic |

## 4. 已知风险

1. `error_occurred_`、`last_incoming_time_` 仍为基类无同步读写（既有，低风险）。
2. `EspUdp::Disconnect` 10s 超时后对象释放存在既有 UAF 风险（未改 `esp_udp.cc`）。
3. 迟到 ServerHello 在 pending 窗口缺少请求 ID 关联（低概率）。
4. LED 硬件能力未确认，禁止猜测 GPIO；当前无控制工具。
5. MCP 端到端、动作识别、光照事件与断网重连仍需人工实机验证。

## 5. 下一步

- 实机完成 3 轮语音、晃动/遮挡、MCP 调用、断网重连。
- 确认 LED 驱动归属后再增加受控 LED 工具。
- 需要电机时再引入执行器白名单与超时停止接口，不在本轮臆造 GPIO。
