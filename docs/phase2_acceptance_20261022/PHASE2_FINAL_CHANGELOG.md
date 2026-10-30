# PHASE2_FINAL_CHANGELOG

日期：2026-10-23

## 本轮代码修改

| 文件 | 目的 | 摘要 |
|---|---|---|
| `main/companion/state_machine.h` | 一致性快照 API | 新增 `state_machine_snapshot_t` 与 `state_machine_get_snapshot()` |
| `main/companion/state_machine.c` | 修复数据竞争 | 状态字段读写加 `portMUX` 短临界区；外部 LED/音频动作移到锁外；所有 getter 加锁；新增单次快照 |
| `main/companion/companion_service.cc` | MCP 一致读取 | `get_status` 改用一次快照；LED 状态改为真实值（`led_available=true`、`led_state`、`actuator_state=led_only`） |
| `main/companion/companion_led.h/.cc` | 并发读安全 | 当前 LED 模式改为 `std::atomic<int>`，新增只读 getter 与模式名 |
| `tests/host/test_main.c` | 回归覆盖 | 新增 5 项快照一致性测试 |

## 根因

`state_machine` 静态字段由 companion 主任务写，MCP 回调在 Application 主循环读，缺少同步；多字段分次读取可能得到不同时刻的组合。

## 修复原则

- 短临界区只保护字段复制，不持锁执行 LED、音频、日志或网络。
- 读取方使用一次快照，保持字段一致。
- 未改变 MCP 工具名、参数、JSON 字段名。
- 未使用 `volatile` 替代锁。
- 未改变 60 分钟提醒周期、传感器周期、LED 效果策略。

## 测试结果

- C 主机测试：82 PASS / 0 FAIL。
- C++ 协议测试：5 PASS / 0 FAIL。
- 目标构建：成功，退出码 0。
- 烧录：成功，MAC `a4:cb:8f:d6:12:64`，校验通过。
- 启动：待机心跳、传感器 `ok/ok`、无 panic/WDT。

## 交付物

- 固件：`firmware/xiaozhi_concurrency_fix_20261023.bin`，2,748,256 字节，SHA-256 `237F6E753C34158A7488DBE38ACCC9CE82CA62DE52C381874AB32D4A224020CF`
- 本轮补丁：`companion_concurrency_fix_20261023.patch`
- 烧录日志：`flash_concurrency_fix_20261023.log`
- 启动日志：`boot_log_concurrency_fix_20261023.log`
- 测试日志：`phase2_final_host_tests_20261023.log`
- 构建日志：`phase2_final_build_20261023.log`

## 风险与回滚

- 风险：临界区使用不当可能引入优先级反转；当前临界区仅复制字段，未调用阻塞 API。
- 风险：主机测试为单线程，目标端并发压力未执行。
- 回滚：反向应用本轮补丁，或恢复上一版 `xiaozhi_gpio_led_20261023.bin`。
- 未执行全片擦除，未清除 NVS，未修改 Wi-Fi/MQTT 凭据或分区表。
