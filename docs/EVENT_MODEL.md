# EVENT MODEL — 事件与阈值

## 事件类型

| 事件 | 来源 | 条件 | 状态机影响 |
|---|---|---|---|
| `EVT_PICKED_UP` | QMA6100P + motion_detect | 稳定 5 拍校准后，`\|Δz\| > 0.45g`，锁定 400ms | 待机/犯困 → 被关注 |
| `EVT_SHAKE` | QMA6100P | 1s 窗口内方向翻转 ≥3 次（阈值 0.3g） | 待机/犯困 → 被关注 |
| `EVT_HAND_NEAR` | AP3216C PS | PS > 50 连续 3 次，边沿触发一次 | 待机/犯困 → 被关注 |
| `EVT_DARK` | AP3216C ALS | ALS < 50 连续 3 次 | 仅记录，不迁移 |
| `EVT_BRIGHT` | AP3216C ALS | ALS ≥ 60 连续 3 次 | 仅记录，不迁移 |
| `EVT_TIMER_SLEEPY` | state_machine tick | IDLE 满 60 分钟 | 待机 → 犯困 |
| `EVT_INTERACT_START/END` | `companion_main` 轮询语音状态 | Listening / 回到 Idle | 互动态切换 |

## 采样与去抖

- 采样周期 50ms；AP3216C 上电预热 10 拍（约 500ms）后才判定。
- ALS 双阈值 50/60，3 次去抖；PS 阈值 50，3 次去抖 + 边沿。
- 传感器读取失败：保留最近有效值，`ap3216c_ok/qma6100p_ok=false`，`*_data_fresh=false`；不产生伪事件。
- 运动阈值来自 `motion_detect.h`：`PICKUP_DZ_THRESHOLD=0.45f`、`PICKUP_HOLD_MS=400`、`SHAKE_FLIP_COUNT=3`、`SHAKE_WINDOW_MS=1000`。

## 事件历史

- 容量 24 条（`COMPANION_EVENT_LOG_CAP`），环形覆盖。
- 记录：单调 `seq`、开机毫秒、事件类型、ALS、PS、`valid`。
- 只在主任务写入；MCP 读取时复制，不长时间持锁。
- 队列满：丢弃并累计 `event_queue_dropped`，日志节流。

## 状态机

`IDLE / HAPPY / INTERACT / SLEEPY / ALARM`，详见 `main/companion/state_machine.c`。
- 30s 无活动：HAPPY → IDLE。
- IDLE 60 分钟：SLEEPY，并只请求一次提醒。
- 语音交互优先，交互期间不计犯困时间。
- 光照事件不参与状态迁移，仅用于状态展示与历史。

## 验证

- 主机测试：阈值/重复抑制/队列满/tick 回绕/事件历史容量（59 PASS）。
- 实机：遮挡/恢复、手靠近、拿起/摇晃待人工执行并记录日志。
