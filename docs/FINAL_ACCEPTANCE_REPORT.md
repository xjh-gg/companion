# FINAL ACCEPTANCE REPORT — AI Companion 多模态具身终端

> **2026-10-22 17:43 复核状态**：COM17 已不存在，设备端验证无法继续；当前仅确认 59 项主机测试与增量构建（固件哈希仍为 03E6…）。本报告下方 15:35–15:50 的设备记录为历史证据，不能视为当前设备状态。最新只读审查见 `backups/final_acceptance_20261022_1605/`。

验收时间：2026-10-22 15:35–15:50
验收目录：`backups/final_acceptance_20261022_1535/`
结论：**可复现构建、主机测试与启动/传感器/MQTT 验证通过；语音多轮、物理传感器、MCP 端到端、断网重连与长稳仍为人工/外部条件待办。不得标记为“完整实机验收通过”。**

---

## 1. 当前版本身份

| 项 | 值 |
|---|---|
| 真实构建源 | `<xiaozhi_source>` |
| 工作区（非构建源） | `D:\Projects\workspace\companion` |
| 版本 | XiaoZhi 2.0.3 |
| IDF | ESP-IDF v6.1.0 |
| 目标 | ESP32-S3（QFN56 rev v0.2，8MB PSRAM），ATK-DNESP32S3 |
| 构建目录 | `build_companion` |
| 正式固件 | `build_companion\xiaozhi.bin` / `backups\xiaozhi_companion_embodied_20261022.bin` |
| 大小 | 2,753,616 B |
| SHA-256 | `03E69216E083A948208F2CA170392A4B1BA43B179AF31A7C2D8FC3E2CE4AA885` |
| 本次构建 | 2026-10-22 15:48，exit 0，0 warning/0 error；哈希与烧录版一致 |
| 分区余量 | 1,375,152 B（约 33.3%） |

源码清单与哈希：`backups/final_acceptance_20261022_1535/SOURCE_MANIFEST.txt`。

关键源码哈希：
- `companion_service.cc`：`A82D63E7…F17C378`
- `companion_event_log.c`：`3BA07412…F1D740`
- `companion_event_log.h`：`0074A6C3…A77807`
- `mqtt_protocol.cc`：`ACE6E97E…2008ADB`（AES 修复保留）
- `mqtt_protocol.h`：`2CF3281D…82AFD33`

---

## 2. 测试矩阵

| 测试项 | 测试方式 | 实际证据 | 结果 | 未通过原因/下一步 |
|---|---|---|---|---|
| 主机测试 | 自动化 | `final_acceptance_20261022_1535/host_tests.log`：`RESULT: PASS=59 FAIL=0`，exit 0 | **PASS** | 未覆盖真实并发/PSA/音频 |
| 固件构建 | ESP-IDF 6.1 | `build.log`：exit 0，0 warning/error；哈希 `03E6…` | **PASS** | 哈希与已烧录版一致 |
| 普通烧录 | 串口 | 上一轮 `embodied_flash_20261022.log`：exit 0，Hash verified，无 erase；本次构建哈希未变，无需重复烧录 | **PASS（无需重烧）** | — |
| 启动与传感器 | 实机 COM17 | `final_acceptance_20261022_1535/boot.log`：AP3216C 0x1E、QMA6100P 0x90、Companion 启动 | **PASS** | — |
| MQTT 连接 | 实机 | `WifiStation: Got IP`、`MQTT: Connected to endpoint`、`STATE: idle` | **PASS** | — |
| 语音回归（03E6） | 真人+日志 | 本轮 28s 捕获无唤醒；上一版 F80C 有 1 轮完整会话 | **PARTIAL / NOT RUN** | 需真人完成 3 轮 |
| 实际扬声器 | 真人确认 | 无听音确认 | **NOT RUN** | 需人耳确认 |
| 环形事件历史 | 自动化 | 13 项新测试（容量 24、最新在前、limit、EVT_NONE、无效标志） | **PASS** | MCP 序列化未端到端 |
| 光照事件 | 物理操作 | 代码：ALS<50/≥60、3 次去抖、预热 10 拍；本轮未遮挡 | **NOT RUN（代码已实现）** | 需遮挡/恢复操作 |
| 运动事件 | 物理操作 | 代码：拿起/摇晃/手靠近；本轮未操作 | **NOT RUN（代码已实现）** | 需拿起/摇晃 |
| MCP 端到端 | 真实客户端 | 仅启动日志 `Add tool`；无 `tools/call` | **BLOCKED** | 需服务端/客户端 |
| 断网重连 | 实机 | 未断开热点（禁止擅改主机网络） | **BLOCKED** | 需人工控制热点 |
| 通道重建 | 实机 | 未触发会话超时/重建 | **NOT RUN** | 需语音触发 |
| 长时间稳定性 | 实机 | 03E6 连续日志约 2 分钟；F80C 约 7.5 分钟 | **PARTIAL** | 未达 30–60 分钟 |
| LED 控制 | — | `led_available=false`，无工具 | **NOT IMPLEMENTED** | 硬件归属未确认 |
| 执行器 | — | `actuator_state=not_installed` | **NOT IMPLEMENTED** | 无硬件/驱动 |
| MQTT 自定义遥测 | — | `mqtt_state=unsupported` | **NOT IMPLEMENTED** | 未扩展协议 |

---

## 3. 缺陷与风险

| 等级 | 项 | 证据 | 性质 | 影响 | 是否需修 |
|---|---|---|---|---|---|
| P1（条件性） | `last_incoming_time_`、`error_occurred_` 无同步 | `protocol.cc:36`、`mqtt_protocol.cc:136/415`、`IsAudioChannelOpened` | 既有数据竞争（低概率） | 可能误判超时/错误状态，不直接崩溃 | 生产前建议修 |
| P1（条件性） | `EspUdp::Disconnect` 10s 超时后对象释放 | `esp_udp.cc:16-23,69-81` | 既有 UAF 风险 | 极端的任务卡死场景 | 需改 `esp_udp.cc`，本轮禁止 |
| P2 | 迟到 ServerHello 无请求 ID 关联 | `mqtt_protocol.cc:562` pending 窗口 | 条件性 | 低概率会话参数错配 | 记录，暂不修 |
| P2 | MCP 端到端未验证 | 无 `tools/call` 日志 | 外部条件不足 | 不能声称 MCP 可用 | 人工/客户端 |
| P2 | 物理传感器事件未验证 | 无遮挡/摇晃日志 | 人工条件 | 不能声称实物识别通过 | 人工 |
| P3 | `network_connected` 只代表 WiFi | `WifiStation::IsConnected` | 设计说明 | 不代表 MQTT/服务端 | 文档已注明 |
| P3 | LED/执行器未实现 | `companion_led.cc` 占位、无驱动 | 未实现 | 不能用于演示执行器 | 后续 |

未发现本轮新增代码导致的崩溃、死锁或越界证据；主机测试与启动验证通过。

---

## 4. 人工待办清单（集中）

1. **3 轮语音**：说「你好小智」，每轮提一个问题并等待回答；确认扬声器实际出声。
   - 预期：`Wake word detected`、`STATE: listening → speaking`、`<<` 回复；无 `Failed to decrypt`。
2. **光照**：遮住 AP3216C（U5）约 3 秒再移开。
   - 预期：`环境进入暗光` → `环境光照恢复`；事件历史出现 `EVT_DARK/EVT_BRIGHT`。
3. **运动**：拿起、轻摇设备。
   - 预期：`检测到拿起` / `检测到摇晃`；状态 `待机 -> 被关注`。
4. **MCP**：通过语音或客户端调用 `self.companion.get_status`、`self.companion.get_recent_events`（limit=8/24/0/25）。
   - 预期：字段与真实传感器一致；非法 limit 被拒绝。
5. **断网重连**：在设备联网/空闲时断开热点 5–10 秒后恢复，重复 5–10 次。
   - 预期：WiFi/MQTT 自动恢复、回到 idle、语音可用；记录每次耗时。
6. **通道重建**：等待一次会话超时或结束后再次唤醒，确认新会话正常、无旧包污染。
7. **长稳**：连续运行 30–60 分钟，记录重启、解密错误、队列丢弃、内存。

---

## 5. 回滚说明

- 本次验收未修改源码；当前源码为上一轮扩展版本。
- 修改前备份：`backups/embodied_20261022_1440/`（`companion_service.cc`、`event_queue.h`、`state_machine.c`、`CMakeLists.txt`、测试文件）。
- 只回滚代码：
  1. 覆盖上述备份到原路径；
  2. 删除 `main/companion/companion_event_log.c/.h`；
  3. 重新构建。
- 固件：
  - 当前正式：`backups/xiaozhi_companion_embodied_20261022.bin`（`03E69216…4AA885`）。
  - 上一版 AES 修复：`backups/xiaozhi_companion_mqtt_aes_fix_20261022.bin`（`F80C03E2…AA2910`）。
  - 1.9.4 整片：`backups/xiaozhi_194_flash_20261021_read1.bin`（仅在明确授权时使用）。
- **禁止用整片擦除作为常规回滚**；本轮未擦除 NVS。

---

## 6. 简历与演示建议

### 可写入简历

> 基于 XiaoZhi 2.0.3 / ESP32-S3 的多模态陪伴终端：集成 AP3216C、QMA6100P 事件建模与 24 条事件历史，扩展 MCP 状态/事件查询；完成 MQTT AES 并发修复（互斥、ServerHello 原子提交、序号一致性）；通过 ESP-IDF 6.1 构建、59 项主机测试与设备启动/传感器/MQTT 验证。

### 不能声称

- 3 轮语音与听感全部通过（当前仅上一版 1 轮日志）。
- MCP 端到端、断网重连、通道重建、30–60 分钟稳定性通过。
- LED/电机/舵机控制已实现。
- “AGI”或完整具身智能。

### 5 分钟演示顺序

1. 上电看启动与两个 MCP 工具注册；
2. 说「你好小智」问设备状态；
3. 拿起/轻摇，展示 `检测到拿起/摇晃` 与状态迁移；
4. 遮挡/恢复光照，展示 `EVT_DARK/EVT_BRIGHT`；
5. 语音或客户端查询 `get_status`/`get_recent_events`；
6. 说明 LED/执行器为后续扩展。

### 面试追问与答题要点

1. **为什么用事件队列而不是在传感器任务直接做业务？** 解耦采样与状态机；队列满有丢弃计数；慢操作不阻塞采样。
2. **如何保证已验证的语音链路不被改坏？** 只调用官方 `PlaySound`/`AudioService`，不改 ES8388/I2S/DMA；音频文件保持官方原版。
3. **MQTT AES 并发修复的关键是什么？** 独立 `aes_mutex_`、`channel→aes` 固定锁序、ServerHello 先校验后原子提交、解密后再推进序号。
4. **环形事件历史如何保证线程安全？** 单写（主任务）+ `portMUX` 临界区，MCP 读取时复制；容量 24，覆盖最旧。
5. **如何避免 AI 直接控制硬件？** MCP 只暴露只读状态/事件；LED/执行器未开放；未来执行器需白名单、参数校验、超时停止。
