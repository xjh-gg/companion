# PHASE2_FINAL_ACCEPTANCE_REPORT

检查日期：2026-10-23
工程源码：`<xiaozhi_source>`
工作区：`D:\Projects\workspace\companion`
交付目录：`backups/delivery_20261022`
固件：`xiaozhi_concurrency_fix_20261023.bin`，2,748,256 字节，SHA-256 `237F6E753C34158A7488DBE38ACCC9CE82CA62DE52C381874AB32D4A224020CF`

## 1. 本轮范围

- 状态机与 MCP 并发读取数据竞争：根因确认、最小修复、主机回归、目标构建与烧录启动验证。
- 60 分钟提醒与语音播放链路：复用已有实机日志与代码追踪。
- MCP 两个工具：注册、Schema、数据源静态核查。
- 网络断开/恢复：代码路径核查。
- 全量回归与证据整理。

## 2. 并发问题结论

### 根因（已证实）

状态机字段为普通静态变量，由 `companion_main_task` 写；MCP 工具回调经 `Application::Schedule` 在 Application 主循环执行，直接调用 `state_machine_get_*` 读取同一组字段。两侧没有共同同步机制，属于 C11/C++ 内存模型意义上的数据竞争；多字段分次读取还可能产生不一致快照。

### 修复

- `state_machine.c` 增加 `portMUX_TYPE` 短临界区，所有状态字段写与 getter 读均受保护；LED/音频/日志等外部动作在释放锁后执行。
- 新增 `state_machine_get_snapshot()`，一次临界区内复制状态、计时、最近事件等字段，MCP `get_status` 改为使用单次快照，避免多 getter 之间的状态漂移。
- `companion_led` 当前模式改为 `std::atomic<int>`，MCP 可安全读取 `led_state`。
- 未使用 `volatile` 替代同步，未把整段业务逻辑包进长锁，未改变 MCP 工具名称、参数和 JSON 字段名。

### 验证

- 主机 C 测试：82 PASS / 0 FAIL（新增 5 项快照一致性测试）。
- 主机 C++ 协议测试：5 PASS / 0 FAIL。
- 目标构建：成功，退出码 0。
- 烧录：COM11、MAC `a4:cb:8f:d6:12:64`、偏移 `0x20000`、460800，哈希校验通过，退出码 0。
- 烧录后启动：待机心跳、传感器 `ok/ok`、LED 慢闪、无 panic/WDT。
- 限制：未在目标端做高频 MCP 并发压力测试；FreeRTOS 运行时并发行为由短临界区代码保证，未使用 ThreadSanitizer。

## 3. 语音与 60 分钟提醒

调用链：

`state_machine_tick`（IDLE 满 3600s）→ `enter_state(SLEEPY)` → `companion_audio_request_reminder` → `companion_audio_task` → `Application::PlaySound(OGG_POPUP)` → `AudioService::PlaySound` → ES8388/I2S → 扬声器。

证据层级：

| 层级 | 状态 | 证据 |
|---|---|---|
| 驱动初始化 | PASS | 启动日志 Es8388AudioCodec initialized、I2S 全双工、输出使能 |
| 业务发出播放请求 | PASS | 60 分钟日志 `COMPANION_AUDIO: 播放犯困提醒` |
| 播放接口执行 | PASS | `Adev_Codec: Open codec device OK`、`Set output enable to true`、`OpusHead` |
| 实际扬声器出声 | BLOCKED | 无当前固件人耳听音确认 |

正式周期为 60 分钟，计时只在 IDLE 累计，状态切换重置；重启后从 IDLE 重新计时。提醒失败每 30 秒重试，最多 3 次。实际出声仍需人工听音。

## 4. MCP

- 注册证据：启动日志 `MCP: Add tool: self.companion.get_status`、`get_recent_events`。
- `get_status`：读取真实状态机快照、传感器快照、事件累计、队列丢弃、音频忙、LED 模式；`mqtt_state` 仍为 `"unsupported"`（MQTT 仅承载小智协议）。
- `get_recent_events`：读取真实 24 条环形历史，最新在前，`limit` 1–24，`EVT_NONE` 不入历史。
- 外部 `tools/call`：**BLOCKED**。当前无可用 MCP 客户端/授权环境，未伪造调用结果。

## 5. 网络断开与恢复

静态核查：

- Wi-Fi 断开事件清连接位，未超过 `MAX_RECONNECT_COUNT` 时自动 `esp_wifi_connect()`；超过后重新扫描并 10 秒后重试。
- MQTT 断开回调启动重连定时器，按间隔重新连接。
- 陪伴传感器/状态机任务不依赖网络，网络断开时仍可继续运行；提醒本地 OGG 播放不依赖 MQTT。
- 未发现网络路径直接创建陪伴任务或重复注册陪伴 MCP 工具。

实际断网/恢复：**BLOCKED**。未获得可控、可恢复的断网条件，未修改路由器或设备配网信息。

## 6. 回归结果

| 项目 | 结果 |
|---|---|
| ESP-IDF 6.1 构建 | PASS，退出码 0 |
| C 主机测试 | PASS，82/82 |
| C++ 协议测试 | PASS，5/5 |
| 状态机快照测试 | PASS，5 项 |
| 事件队列/历史 | PASS，既有测试 |
| 运动检测 | PASS，既有测试 |
| LED/按键 | PASS，实机确认 |
| 目标启动 | PASS，无 panic/WDT |

## 7. 最终结论

并发数据竞争已确认并修复，主机回归、目标构建和启动验证通过。LED、按键、运动、状态机、60 分钟提醒软件链路均已有实机证据。MCP 外部调用、实际扬声器听音、真实断网恢复仍为 BLOCKED，不能据此宣称完整端到端验收通过。

验收状态汇总：PASS 19、PARTIAL 2、BLOCKED 3、FAIL 0、NOT IMPLEMENTED 1。
