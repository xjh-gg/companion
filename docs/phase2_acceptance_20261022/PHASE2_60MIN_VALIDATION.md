# PHASE2_60MIN_VALIDATION — 犯困定时与提醒实机验证

执行时间：2026-10-22 22:24:10–22:25:30
设备：ESP32-S3，MAC `a4:cb:8f:d6:12:64`，COM11
固件：`xiaozhi_p1_led_key.bin`（P1，含 LED/按键/提醒重试）
日志：`backups/delivery_20261022/boot_log_60min_20261022.log`

## 结果

| 时间/uptime | 日志 | 结论 |
|---|---|---|
| 22:24:28 / 3,598,798ms | 心跳 `状态=待机 犯困定时=60 分钟` | 到点前仍为待机 |
| 22:24:31 / 3,601,568ms | `犯困定时到（60 分钟）`；`待机 -> 犯困` | 60 分钟计时准确触发 |
| 22:24:31 | `COMPANION_LED: 陪伴 LED 模式已应用: blink_slow` | 进入犯困后请求 LED 灯效 |
| 22:24:31 | `COMPANION_AUDIO: 播放犯困提醒（复用 XiaoZhi OGG_POPUP）` | 业务实际调用播放 |
| 22:24:31 | `Adev_Codec: Open codec device OK`、`Set output enable to true`、`OpusHead` | 音频编解码器打开，播放链路执行 |
| 22:24:40–22:25:20 | 心跳 `状态=犯困 ... 音频忙=0 传感器=ok/ok 队列丢弃=0` | 状态保持，传感器/队列正常 |
| 22:24:47 | `Set output enable to false` | 音频输出按电源超时正常关闭 |

- 无 panic、abort、WDT、Backtrace、重启。
- 犯困提醒只请求一次；本次首次请求即被受理，未触发重试分支。
- 实际扬声器是否发声仍需人工听音确认；本日志只能证明播放调用与 codec 打开成功。

## 后续 P2 固件

在 60 分钟日志采集完成后，已烧录 P2 固件 `xiaozhi_p2_timer_history.bin`：
- 增加进入 SLEEPY 时把 `EVT_TIMER_SLEEPY` 写入事件历史，MCP `get_recent_events` 可见。
- 烧录成功，MAC 一致，哈希校验通过。
- P2 启动 35 秒日志：LED `blink_slow` 应用、Wi-Fi/MQTT 连接、传感器 ok/ok、无 panic/WDT。
- P2 的 60 分钟行为未再次等待验证；其计时逻辑与 P1 相同，仅增加历史记录。

## 仍需人工

1. 确认犯困提醒是否实际出声。
2. 通过 MCP `get_recent_events` 确认 60 分钟事件出现在历史中（需外部客户端或下一轮 60 分钟实机）。
3. LED 慢闪、KEY0–3 按键的物理确认。
