# PHASE2_FINAL_TEST_MATRIX

日期：2026-10-23
主机日志：`backups/delivery_20261022/phase2_final_host_tests_20261023.log`
构建日志：`backups/delivery_20261022/phase2_final_build_20261023.log`
烧录/启动日志：`flash_concurrency_fix_20261023.log`、`boot_log_concurrency_fix_20261023.log`

| ID | 测试项 | 前置条件/操作 | 预期 | 实际 | 证据 | 状态 |
|---|---|---|---|---|---|---|
| F01 | 状态机与 MCP 数据竞争根因 | 静态追踪写/读任务 | 确认共享非原子字段 | 确认：main 任务写，MCP 主循环读 | `state_machine.c`、`mcp_server.cc:557` | PASS |
| F02 | 状态字段短临界区保护 | 构建 + 代码审查 | 读写在锁内，外部动作锁外 | 已实现 | `state_machine.c` | PASS |
| F03 | MCP 一致性快照 | 主机测试 | 一次读取字段一致 | 5 项快照测试通过 | 主机日志 | PASS |
| F04 | 并发运行时压力 | 目标端高频 MCP 调用 | 无撕裂/不一致 | 未执行 | — | PARTIAL |
| F05 | 主机 C 回归 | `run_tests.ps1` | 87 项通过 | 82 C + 5 C++，0 失败 | 主机日志 | PASS |
| F06 | 目标构建 | `idf.py -B build_companion build` | 成功 | 成功，退出码 0 | 构建日志 | PASS |
| F07 | 目标烧录 | COM11、0x20000、460800 | 成功且校验 | 成功，MAC 一致 | 烧录日志 | PASS |
| F08 | 烧录后启动 | 串口日志 | 无 panic/WDT | 待机心跳、传感器 ok | 启动日志 | PASS |
| F09 | 60 分钟定时 | 实机 60 分钟日志 | 3600s 进入犯困 | 3601.5s 进入犯困 | `boot_log_60min_20261022.log` | PASS |
| F10 | 提醒播放请求 | 进入犯困 | 调用 PlaySound | 日志显示播放请求 | 同上 | PASS |
| F11 | 播放接口执行 | codec/I2S | 打开成功 | 日志显示 codec open/output enable | 同上 | PASS |
| F12 | 扬声器实际出声 | 人耳听音 | 听到提示音 | 未确认 | — | BLOCKED |
| F13 | 提醒重试 | 主机测试 | 30s 重试最多 3 次 | 4 项测试通过 | 主机日志 | PASS |
| F14 | MCP 工具注册 | 启动日志 | 两个 companion 工具 | 已注册 | 启动日志 | PASS |
| F15 | get_status 数据源 | 静态检查 | 真实状态/传感器 | 真实快照；mqtt_state 仍 unsupported | `companion_service.cc` | PARTIAL |
| F16 | get_recent_events 数据源 | 静态 + 主机历史测试 | 24 条最新在前 | 测试通过 | 主机日志 | PASS |
| F17 | MCP 外部 tools/call | 外部客户端/授权 | 返回真实 JSON | 无客户端 | — | BLOCKED |
| F18 | Wi-Fi 断线重连代码 | 静态检查 | 自动重连/重扫 | 有重连计数与 10s 重扫 | `wifi_station.cc:244-259` | PASS |
| F19 | MQTT 断线重连代码 | 静态检查 | 定时重连 | 有重连定时器 | `mqtt_protocol.cc:17-30,78-90` | PASS |
| F20 | 实际断网/恢复 | 可控断网 | 本地业务继续、恢复无重复任务 | 未执行 | — | BLOCKED |
| F21 | LED 慢闪/快闪 | 实机 | 物理可见 | 用户确认 | `user_led_key_test_20261023.log` | PASS |
| F22 | KEY0–3 | 实机按键 | 事件与状态迁移 | 四键日志 + 用户确认 | 同上 | PASS |
| F23 | 运动识别 | 拿起/摇晃 | 进入被关注 | 日志检测拿起，30.8s 回退 | 同上 | PASS |
| F24 | 事件历史容量/顺序 | 主机测试 | 24 条、最新在前 | 通过 | 主机日志 | PASS |
| F25 | 报警状态可达性 | 静态检查 | 有生产者 | 无 `EVT_ALARM` 生产者 | 源码搜索 | NOT IMPLEMENTED |

## 汇总

PASS 19、PARTIAL 2、BLOCKED 3、FAIL 0、NOT IMPLEMENTED 1，合计 25。
