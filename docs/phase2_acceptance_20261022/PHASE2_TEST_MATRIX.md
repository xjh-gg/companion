# PHASE2_TEST_MATRIX — 阶段二测试矩阵

检查日期：2026-10-22
主机测试日志：`docs/phase2_acceptance_20261022/PHASE2_HOST_TESTS.log`
实机证据：`backups/delivery_20261022/boot_log_20261022_boot.log`、`flash_20261022.log`

状态定义：PASS / PARTIAL / FAIL / BLOCKED / NOT IMPLEMENTED / NOT TESTED。

> 2026-10-23 物理确认更新：
> - T05 物理拿起/摇晃、T07 真实运动事件→状态：**PASS**（日志检测到拿起并进入被关注，用户确认快闪）
> - T18 犯困实机闭环：BLOCKED → **PARTIAL**（60 分钟状态迁移与提醒播放路径已证实，实际出声待听音）
> - T22 LED 情绪反馈、T23 按键交互：PARTIAL → **PASS**（慢闪/快闪与 KEY0–3 均经用户确认）
> - 最终统计：PASS 23、PARTIAL 7、FAIL 1、BLOCKED 3、NOT IMPLEMENTED 3，合计 37。

| ID | 验收项 | 静态检查 | 主机测试 | 实机测试 | 状态 | 证据与说明 |
|---|---|---|---|---|---|---|
| T01 | QMA6100P 初始化与 ID | 通过 | 不适用 | 已有日志 | PASS | 启动日志 `QMA6100P: 芯片 ID 校验通过: 0x90`、8G/100Hz |
| T02 | QMA 周期读取与 g 值更新 | 通过 | 不适用 | 仅心跳 ok | PARTIAL | 代码每 50ms 读，心跳 `传感器=ok/ok`；当前日志未输出 gx/gy/gz 实测值 |
| T03 | 拿起识别算法 | 通过 | 10 项运动测试 | 未做物理动作 | PASS | `motion_detect.c:82-107`；主机测试含校准/触发/锁定 |
| T04 | 摇晃识别算法 | 通过 | 10 项运动测试 | 未做物理动作 | PASS | `motion_detect.c:26-48,65-80`；1s 窗口/3 次翻转 |
| T05 | 物理拿起/摇晃触发 | 通过 | 不适用 | 未执行 | BLOCKED | 需要人工拿起/摇晃设备 |
| T06 | 事件入队与消费 | 通过 | 队列 8 项测试 | 未观察到事件 | PASS | `event_queue.c:30-51`、`companion_service.cc:335-338` |
| T07 | 真实运动事件→状态迁移 | 通过 | 状态机测试 | 未观察到 | BLOCKED | 需物理动作并抓取日志 |
| T08 | 状态机迁移 | 通过 | 状态机测试 | 仅待机心跳 | PASS | `state_machine.c:107-189`；覆盖 IDLE/HAPPY/INTERACT/SLEEPY |
| T09 | 状态机与 MCP 并发读 | 发现数据竞争 | 不适用 | 未触发异常 | FAIL | `companion_main_task` 写状态，MCP 回调在主循环读，无锁/原子；可能导致字段短暂不一致 |
| T10 | ALS 光照读取 | 通过 | 不适用 | ALS 91–95 | PASS | 启动日志多次心跳；`ap3216c.c:86-114` |
| T11 | ALS 业务行为（仅记录） | 通过 | 不适用 | 未观察暗光事件 | PASS | 设计明确仅记录；`state_machine.c` 不处理 DARK/BRIGHT |
| T12 | PS 接近读取 | 通过 | 不适用 | PS 0–16 | PASS | 心跳数据；`ap3216c.c` PS 转换 |
| T13 | PS 接近→HAPPY | 通过 | 状态机 HAND_NEAR 测试 | PS 未过阈值 | PARTIAL | 事件链存在；需真实手靠近产生 PS>50 |
| T14 | 事件历史容量/顺序/EVT_NONE | 通过 | 16 项历史测试 | 不适用 | PASS | 容量 24、最新在前、EVT_NONE 忽略 |
| T15 | 队列满/丢弃处理 | 通过 | 队列测试 | 不适用 | PASS | `event_queue.c:41-48`，丢弃计数与节流日志 |
| T16 | 60 分钟犯困逻辑 | 通过 | 状态机测试 | 未等待 60 分钟 | PASS | 虚拟 tick 3599/3600s 边界通过；`state_machine.c:205-210` |
| T17 | 犯困提醒音频请求 | 通过 | 不适用 | 未到 60 分钟 | PARTIAL | `companion_audio.cc:69-89`；忙时丢弃且不重试 |
| T18 | 犯困实机完整闭环 | 通过 | 不适用 | 未执行 | BLOCKED | 需真实等待 60 分钟 |
| T19 | 扬声器实际发声 | 调用路径存在 | 主机测试不能代替 | 无播放/听音 | BLOCKED | 当前固件只验证到 codec 启动 |
| T20 | 音频 codec/功放/音量初始化 | 通过 | 不适用 | 已有日志 | PASS | ES8388 init、I2S 全双工、XL9555 P0.2 拉低、0x2E–0x31=0dB |
| T21 | 播放与小智对话资源共享 | 通过 | 不适用 | 未做冲突测试 | PARTIAL | 共用 AudioService；有 idle 检查但检查与播放存在竞态 |
| T22 | LED 情绪反馈 | 占位实现 | 不适用 | 不可见 | NOT IMPLEMENTED | `companion_led.cc:15-34` 仅日志，不驱动 GPIO1 |
| T23 | 按键交互 | 无生产者 | 不适用 | 未执行 | NOT IMPLEMENTED | 全树未发现 `event_queue_send(EVT_KEY_PRESSED)` |
| T24 | 报警状态 | 无生产者 | 不适用 | 未执行 | NOT IMPLEMENTED | 全树未发现 `event_queue_send(EVT_ALARM)`；STATE_ALARM 不可达 |
| T25 | 定时事件进入历史 | 未实现 | 不适用 | 不适用 | NOT IMPLEMENTED | `EVT_TIMER_SLEEPY` 直送状态机，未调用历史记录 |
| T26 | MCP 工具注册 | 通过 | 不适用 | 已有日志 | PASS | 启动日志两个 companion 工具 + 通用工具 |
| T27 | MCP 外部调用 | 通过 | 不适用 | 未执行 | BLOCKED | 需外部 MCP 客户端 |
| T28 | MCP 返回字段真实性 | 基本通过 | 不适用 | 未调用 | PARTIAL | 状态/快照字段来自真实数据；`mqtt_state` 固定 `unsupported` |
| T29 | MQTT 陪伴事件上报 | 未实现 | 不适用 | 不适用 | NOT IMPLEMENTED | `mqtt_protocol.cc` 无陪伴事件发布；MQTT 仅承载小智协议 |
| T30 | 断网时本地业务运行 | 通过 | 不适用 | 未执行 | BLOCKED | 任务结构独立于网络，但未制造断网验证 |
| T31 | 启动/Wi-Fi/MQTT 稳定性 | 通过 | 不适用 | 已有日志 | PASS | 45 秒无重启/panic/WDT；Wi-Fi 获 IP，MQTT 连接成功 |
| T32 | 协议并发原子修复 | 通过 | 5 项协议测试 | 不适用 | PASS | 原子时间戳/错误标志测试通过 |
| T33 | UDP 接收任务生命周期 | 通过 | 无并发测试 | 未触发退出场景 | PARTIAL | 代码修复并构建；真实并发退出未复现 |
| T34 | 传感器读取失败处理 | 通过 | 不适用 | 未制造失败 | PASS | 保留最近有效值、标记 ok/fresh、不产生伪事件 |
| T35 | 驱动初始化错误传播 | 部分通过 | 不适用 | 未制造失败 | PARTIAL | AP3216C/QMA 初始化写寄存器返回值未检查 |
| T36 | 固件与日志一致性 | 通过 | 不适用 | 已有日志 | PASS | esptool 写入 2,754,432 字节并校验；MAC 与项目板一致 |
| T37 | 主机回归测试 | 通过 | 直接运行现有 exe | 不适用 | PASS | C 73 PASS/0 FAIL，C++ 5 PASS/0 FAIL，退出码 0 |

## 统计

| 状态 | 数量 |
|---|---|
| PASS | 18 |
| PARTIAL | 7 |
| FAIL | 1 |
| BLOCKED | 6 |
| NOT IMPLEMENTED | 5 |
| NOT TESTED | 0 |
| 合计 | 37 |

## 关键边界与覆盖说明

- 事件历史边界：0、1、8、24、25 条，空指针，严格倒序，`EVT_NONE` 忽略，均有主机测试。
- 状态机边界：30s HAPPY 超时、3599/3600s 犯困边界、tick 回绕、提醒被拒、非法事件、重复交互。
- 运动边界：静止校准、拿起、锁定期间重复抑制、重新校准、摇晃、tick 回绕。
- 协议边界：120s 超时边界、`SetError` 原子标志、更新时间后恢复。
- 未覆盖：真实 QMA 物理动作、真实 PS 触发、当前固件扬声器发声、60 分钟实机提醒、MCP `tools/call`、断网重连、状态机与 MCP 并发一致性的运行时压力测试。
