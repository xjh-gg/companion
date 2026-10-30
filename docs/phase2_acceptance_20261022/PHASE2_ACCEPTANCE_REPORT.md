# PHASE2_ACCEPTANCE_REPORT — 情感陪伴终端阶段二验收

检查日期：2026-10-22
检查范围：陪伴业务链路、语音提醒链路、定时链路、MQTT/MCP 关联、现有测试与实机证据。
执行约束：首轮只读；未修改业务代码；未编译；未烧录；未擦除 NVS/Flash；未改动网络凭据。

## 1. 工程与证据基线

| 项目 | 实际值 |
|---|---|
| 工作区 | `D:\Projects\workspace\companion` |
| 实际源码树 | `<xiaozhi_source>` |
| Git | 工作区与源码树均**不是 Git 仓库**，无法提供 commit；以交付补丁为版本锚点 |
| 交付补丁 | `backups/delivery_20261022/companion_fixes_20261022.patch`，SHA-256 `523DA580A4EA269BDD6831CBAA0E98124086539A55932CFF78E2BA8F6DE17895` |
| 当前固件 | `backups/delivery_20261022/firmware/xiaozhi.bin`，2,754,432 字节，SHA-256 `397A41E0867F890957C7EF0E1CA723AA25A553E154913042626E5312E6A96C9C` |
| 烧录/启动证据 | `flash_20261022.log`、`boot_log_20261022_boot.log`；COM11、MAC `a4:cb:8f:d6:12:64` |
| 主机测试 | `docs/phase2_acceptance_20261022/PHASE2_HOST_TESTS.log`；C 73 PASS/0 FAIL，C++ 5 PASS/0 FAIL，退出码均 0 |

## 2. 当前实现程度（结论先行）

**已经连通的链路：**

1. QMA6100P → `motion_detect_update` → `EVT_PICKED_UP/EVT_SHAKE` → 事件队列 → `companion_main_task` → 状态机 → 事件历史。
2. AP3216C ALS/PS → 阈值/去抖 → `EVT_DARK/EVT_BRIGHT/EVT_HAND_NEAR` → 队列 → 状态机/历史（ALS 仅记录；PS 可进入 HAPPY）。
3. 状态机 5 状态、30s 回待机、IDLE 60 分钟犯困、语音交互状态轮询。
4. 事件历史容量 24、最新在前、`EVT_NONE` 忽略、MCP `get_recent_events` 读取真实环形历史。
5. MCP `get_status` 读取真实快照/状态机；两个 companion 工具已在实机启动日志中注册。
6. 音频驱动初始化、I2S 全双工、ES8388 输出使能、XL9555 P0.2 功放使能、0x2E–0x31 音量 0dB。
7. Wi-Fi/MQTT 实机连接成功，45 秒无重启/panic/WDT。

**只完成单模块、尚未形成真实硬件闭环的部分：**

1. 拿起/摇晃只改变状态机并写日志；LED 适配是空实现，没有可见反馈。
2. 按键交互未接入陪伴业务（`EVT_KEY_PRESSED` 无生产者）。
3. 报警状态不可达（`EVT_ALARM` 无生产者）。
4. 犯困提醒只在进入 SLEEPY 时请求一次；音频忙时直接丢弃，无重试。
5. 犯困定时事件不进事件历史，MCP `get_recent_events` 看不到。
6. 当前固件没有实际播放记录和人工听音证据；扬声器是否发声未验收。
7. MCP 注册成功不等于外部调用成功；`tools/call` 无证据。
8. MQTT 仅承载小智协议，不上报陪伴事件；`mqtt_state` 固定 `unsupported`。

## 3. 运动感知完整链路

- 读取：`companion_sensor_task` 每 50ms 轮询 QMA6100P（`companion_service.cc:181-227`）。
- 识别：`motion_detect_update`（`motion_detect.c:56-118`）。拿起需连续 5 次静止校准，`|Δz| > 0.45g`，锁定 400ms；摇晃为 1s 窗口内三轴符号翻转 ≥3 次，阈值 0.3g。无低通滤波/滑动平均，仅阈值与时间去抖。
- 事件：`event_queue_send(EVT_PICKED_UP/SHAKE)`（214/217 行），队列深度 8，非阻塞。
- 消费：`companion_main_task` 每 100ms 排空队列（335–338 行），调用 `state_machine_on_event` 与 `companion_event_log_record`。
- 状态：IDLE/SLEEPY 收到拿起/摇晃进入 HAPPY；HAPPY 中重复活动只刷新 30s 计时（`state_machine.c:121-143,151-153,165-178`）。
- 反馈：`enter_state(HAPPY)` 请求 `COMPANION_LED_BLINK_FAST`（76–78 行），但 `companion_led_request` 只打印日志，不驱动 GPIO1（`companion_led.cc:15-34`）。不触发语音、蜂鸣器或 MQTT。
- 预期行为：设计文档把“拿起/摇晃/靠近/按键”映射为 HAPPY 状态与快闪 LED；没有定义运动事件必须触发语音。因此当前缺口是 **LED 物理反馈未实现**，不是“缺少语音”。
- 实机证据：只有 QMA 初始化与 `传感器=ok/ok`；没有真实拿起/摇晃日志。结论：算法 PASS，物理闭环 BLOCKED。

## 4. AP3216C 接近与光照链路

- 读取：同一 50ms 传感器任务读取 AP3216C（231 行）；预热 10 拍；ALS 双阈值 50/60、3 次去抖；PS 阈值 50、3 次去抖、边沿触发（238–285 行）。
- 业务消费：
  - `EVT_HAND_NEAR` → 状态机：IDLE/SLEEPY → HAPPY；HAPPY 中刷新计时。
  - `EVT_DARK/EVT_BRIGHT` → 只写事件历史与日志；状态机不处理（符合 `docs/EVENT_MODEL.md` 和 `docs/需求分析.md:85` 的“仅记录”更新）。
- 实机：ALS 91–95、PS 0–16，读取成功；PS 未超过 50，未观察到手靠近事件。
- 结论：读取 PASS；ALS 业务符合“仅记录”设计；PS 触发链路 PARTIAL（缺真实手靠近证据）。

## 5. 事件队列与状态机

- 队列：深度 8，`event_queue_send` 非阻塞；满时丢弃并累计 `s_drop_count`，日志第 1 次和每 100 次打印（`event_queue.c:30-51`）。
- 消费者：单消费者 `companion_main_task`；生产者当前只有传感器任务（5 种事件）。`EVT_INTERACT_START/END` 直送状态机；`EVT_TIMER_SLEEPY` 在 tick 内直送状态机。
- 关键缺口：
  - `EVT_KEY_PRESSED` 无生产者 → 按键分支不可达。
  - `EVT_ALARM` 无生产者 → STATE_ALARM 不可达。
  - `EVT_TIMER_SLEEPY` 不进队列、不进历史 → MCP 最近事件看不到犯困触发。
- 并发：
  - 传感器快照有 `s_snapshot_mutex`；事件历史有 `portMUX_TYPE` 临界区；音频 pending 是 `std::atomic<bool>`；事件队列由 FreeRTOS 保证线程安全。
  - 状态机是普通静态变量，由 `companion_main_task` 写；MCP 回调经 `Application::Schedule` 在主事件循环读（`mcp_server.cc:557`），没有锁/原子 → 存在数据竞争（T09 FAIL）。32 位对齐读在 Xtensa 上通常不会撕裂，但多字段快照可能不一致；未观察到崩溃。
- 完整流程图见 `PHASE2_CALL_GRAPH.md`。

## 6. 语音播放链路（四层判定）

| 层级 | 状态 | 证据 |
|---|---|---|
| 1. 音频驱动初始化成功 | PASS | 实机日志：Es8388AudioCodec initialized、I2S 全双工、ES8388 Start、输出使能 |
| 2. 播放接口被业务调用 | PARTIAL | 仅“进入 SLEEPY”调用 `Application::PlaySound(OGG_POPUP)`；未到 60 分钟，无实机调用记录 |
| 3. 播放执行且无已知错误 | PARTIAL | `PlaySound` 无返回值；解码/写入错误不向业务传播；无当前固件播放日志 |
| 4. 扬声器实际输出预期声音 | BLOCKED | 无播放请求、无人工听音证据 |

补充事实：

- 功放：板级 `atk_dnesp32s3.cc:124` 通过 XL9555 P0.2 拉低使能 MD8002A；`Es8388AudioCodec::EnableOutput` 将 0x2E–0x31 写为 0x1E（0dB）（`es8388_audio_codec.cc:185-193`）。
- 播放是异步：`companion_audio_task` 调用 `PlaySound` 后最多等 5 秒看 `AudioService::IsIdle()`；不阻塞主循环。
- 请求可能被丢弃：设备非 idle 或音频队列忙时返回 false，状态机只打印警告，不重试；进入 SLEEPY 只请求一次。
- 与正常语音对话共用 `AudioService`/I2S/ES8388；`IsIdle()` 只保证队列为空，不能保证实际出声，也不能阻止随后进入的语音会话抢占。
- 运动/靠近/ALS 事件不触发语音；这是当前设计，不是漏调用。

## 7. 定时提醒完整链路

- 参数：60 分钟固定（`SLEEPY_DEFAULT_MIN 60`，`state_machine.c:16`）；`state_machine_set_sleepy_timeout_min` 无调用者；无编译宏/运行配置覆盖。
- 计时起点：启动进入 IDLE 时设置 `s_state_enter_tick`；每次状态切换由 `enter_state` 重置；只在 IDLE 累计（`state_machine.c:57-67,95-105,205-210`）。
- 周期：主任务约每 1s 调 tick（`companion_service.cc:363-366`）；tick 差值计算，回绕安全。
- 触发：IDLE 满 3600s → `enter_state(SLEEPY, EVT_TIMER_SLEEPY)`；请求一次 LED 占位与音频提醒。
- 重复：SLEEPY 内不累计、不重复提醒。
- 退出：INTERACT_START、拿起/摇晃/手靠近可离开 SLEEPY；按键分支不可达。
- 重启：无持久化，重启从 IDLE 重新计时。
- 历史：`EVT_TIMER_SLEEPY` 未写入事件历史（T25 NOT IMPLEMENTED）。
- 提醒失败：音频忙/设备非 idle → 请求被拒，SLEEPY 保持但不再提醒；无重试。
- 实机：未等待 60 分钟，完整闭环 BLOCKED；主机测试用虚拟 tick 验证了 3599/3600s 边界与只请求一次。

## 8. MQTT / MCP

- MQTT：小智协议承载层，处理 hello/goodbye/音频/MCP；**没有**陪伴事件上报代码。`get_status.mqtt_state` 固定 `"unsupported"`（`companion_service.cc:150`），`network_connected` 读取真实 Wi-Fi 状态。
- MCP `get_status`：真实读取状态机、传感器快照、事件累计、队列丢弃、音频忙状态；`led_available=false`、`actuator_state=not_installed` 如实暴露未接入执行器。
- MCP `get_recent_events`：真实读取 24 条环形历史，最新在前，`limit` 1–24、默认 8；`EVT_NONE` 不入历史；边界有主机测试。
- 注册 vs 调用：实机日志只证明注册；没有 `tools/call` 请求/响应证据，端到端 BLOCKED。
- 网络断开：传感器任务与主任务不依赖网络，静态上可继续运行；未制造断网，BLOCKED。

## 9. 测试矩阵汇总

| 状态 | 数量 |
|---|---|
| PASS | 18 |
| PARTIAL | 7 |
| FAIL | 1 |
| BLOCKED | 6 |
| NOT IMPLEMENTED | 5 |
| NOT TESTED | 0 |
| 合计 | 37 |

完整矩阵见 `PHASE2_TEST_MATRIX.md`。本轮直接运行现有测试二进制（未编译），C 73 PASS/0 FAIL、C++ 5 PASS/0 FAIL、退出码 0，日志已保存。

## 10. 发现的缺陷与风险

| 优先级 | 问题 | 证据 | 影响 | 状态 |
|---|---|---|---|---|
| P1 | 陪伴 LED 仅占位 | `companion_led.cc:15-34` | 拿起/摇晃/靠近无可见反馈，核心体验不闭环 | NOT IMPLEMENTED |
| P1 | 按键未接入 | 全树无 `event_queue_send(EVT_KEY_PRESSED)` | F4 按键交互缺失；SLEEPY 无法按键退出 | NOT IMPLEMENTED |
| P1 | 犯困提醒忙时丢弃且不重试 | `companion_audio.cc:69-89`、`state_machine.c:85` | 对话/音频忙时提醒永久丢失 | PARTIAL/FAIL 风险 |
| P2 | 状态机与 MCP 并发读无同步 | `companion_service.cc:117-166`、`mcp_server.cc:557` | 多字段快照可能不一致 | FAIL（静态） |
| P2 | 定时事件不进历史 | `state_machine.c:209` 无 `event_log_record` | MCP 最近事件缺少犯困触发 | NOT IMPLEMENTED |
| P2 | 报警状态不可达 | 无 `EVT_ALARM` 生产者 | STATE_ALARM 为死状态 | NOT IMPLEMENTED |
| P2 | MQTT 不上报陪伴事件 | `mqtt_protocol.cc` 无 companion 逻辑 | M4“数据上报”未落地 | NOT IMPLEMENTED |
| P2 | `mqtt_state` 固定 unsupported | `companion_service.cc:150` | MCP 状态字段可能误导 | PARTIAL |
| P3 | 驱动初始化写错误未检查 | `ap3216c.c:66-80`、`qma6100p.c:80-101` | 初始化失败可能被延后暴露 | PARTIAL |
| P3 | 文档与代码不一致 | `docs/状态机设计.md:44-52` 仍写“光照过暗→报警”；`docs/ARCHITECTURE.md:32` 写 MCP 在协议任务；`EVENT_MODEL.md:39` 写 59 PASS | 后续维护/发布依据不可靠 | 文档问题 |

## 11. 未验证项与原因

- 真实拿起/摇晃 → HAPPY → 可见反馈：BLOCKED，需人工物理动作。
- 真实手靠近 PS>50 → HAPPY：BLOCKED，需人工遮挡/靠近。
- 当前固件扬声器实际发声：BLOCKED，需播放触发与人工听音。
- 60 分钟实机提醒：BLOCKED，需真实等待或安全的测试入口。
- MCP `tools/call` 端到端：BLOCKED，需外部 MCP 客户端。
- 断网时本地业务与提醒：BLOCKED，需可控断网条件。
- 状态机并发压力测试：NOT TESTED，主机无法复现 MCP 与主任务并发。

## 12. 结论

阶段二业务链路的核心“传感器→事件→队列→状态机→历史/MCP”在代码层成立，并已由主机测试和当前固件启动日志部分证实。真正未闭环的是**物理反馈层**（LED/按键/语音提醒可靠性）和**外部验证层**（扬声器、MCP、断网、60 分钟实机）。项目可按“算法与调度完成、物理交互待验收”的定位进入文档整理和 GitHub 发布准备；不宜宣称完整陪伴体验已验收。

本阶段未修改业务代码。后续修复建议见 `PHASE2_NEXT_ACTIONS.md`。

## 13. 2026-10-22 21:25 执行更新

用户授权后已执行 P1-1、P1-2、P1-3：

- LED：新增 `Led::SetCompanionEffect`，SingleLed 实现闪烁/常亮；陪伴适配仅在 XiaoZhi idle 时应用，回到 idle 自动恢复。启动日志确认 `blink_slow` 已应用，物理闪烁待人工确认。
- 按键：XL9555 PORT1 KEY0–3 每 50ms 扫描、3 拍去抖、下降沿产生 `EVT_KEY_PRESSED`；非 abort 读取，无崩溃。物理按键待人工确认。
- 提醒重试：进入犯困态首次请求，失败后每 30s 重试，最多 3 次，受理后停止；新增 4 项主机测试。
- 主机测试：82 PASS / 0 FAIL（77 C + 5 C++）。
- 构建/烧录：成功，新固件 `xiaozhi_p1_led_key.bin`，SHA-256 `7FFA56D15177FAA4829A1D34B5AE6A7486B188757871D133C80009852FC6010C`；启动 40 秒无 panic/WDT。
- 更新后矩阵统计：PASS 19、PARTIAL 8、FAIL 1、BLOCKED 6、NOT IMPLEMENTED 3。
- 仍待人工：LED 目视、KEY0–3 实际按键、60 分钟实机提醒、扬声器听音、MCP 外部调用、断网重连。

详细执行记录见 `PHASE2_P1_EXECUTION_REPORT.md`。
