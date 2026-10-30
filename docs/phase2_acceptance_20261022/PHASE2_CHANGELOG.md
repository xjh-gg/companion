# PHASE2_CHANGELOG — 阶段二变更记录

## 结论

阶段二初始只读核查未修改业务代码。2026-10-22 21:20–21:25 已按用户授权执行 P1-1（LED）、P1-2（按键）、P1-3（提醒重试），修改了 LED 接口、SingleLed、板级按键读取、陪伴服务、状态机与测试；详见 `PHASE2_P1_EXECUTION_REPORT.md`。

未执行：全片擦除、NVS 清除、Wi-Fi/MQTT 凭据修改、分区表修改。

## 实际新增文件

| 文件 | 用途 |
|---|---|
| `docs/phase2_acceptance_20261022/PHASE2_ACCEPTANCE_REPORT.md` | 阶段二验收报告 |
| `docs/phase2_acceptance_20261022/PHASE2_CALL_GRAPH.md` | 真实调用链与状态迁移 |
| `docs/phase2_acceptance_20261022/PHASE2_TEST_MATRIX.md` | 37 项测试矩阵 |
| `docs/phase2_acceptance_20261022/PHASE2_CHANGELOG.md` | 本文件 |
| `docs/phase2_acceptance_20261022/PHASE2_NEXT_ACTIONS.md` | 后续行动清单 |
| `docs/phase2_acceptance_20261022/PHASE2_HOST_TESTS.log` | 直接运行现有测试二进制的输出 |
| `docs/phase2_acceptance_20261022/PHASE2_P1_EXECUTION_REPORT.md` | P1-1/P1-2/P1-3 执行记录 |

## P1 业务代码修改（2026-10-22 21:25）

| 文件 | 修改目的 |
|---|---|
| `main/led/led.h` | 新增陪伴灯效虚接口 |
| `main/led/single_led.h/.cc` | 实现常亮/持续闪烁/有限闪烁 |
| `main/companion/companion_led.cc/.h` | 接入真实 LED，仅 idle 应用 |
| `main/boards/common/board.h` | 可选按键接口 |
| `main/boards/atk-dnesp32s3/atk_dnesp32s3.cc` | XL9555 PORT1 按键读取 |
| `main/companion/companion_service.cc` | 按键扫描与 LED tick |
| `main/companion/state_machine.c` | 犯困提醒 30s×3 重试 |
| `tests/host/test_main.c` | 新增 4 项重试测试 |

构建与烧录证据、补丁和固件哈希见 `PHASE2_P1_EXECUTION_REPORT.md`。回滚方式：使用 `companion_p1_led_key_20261022.patch` 反向应用，或恢复交付目录中上一版 `firmware/xiaozhi.bin`。

## 只读验证

- 直接运行现有 `tests/host/host_tests.exe` 与 `host_protocol_tests.exe`，未重新编译。
- 结果：C 73 PASS/0 FAIL，C++ 5 PASS/0 FAIL，退出码均为 0。
- 使用现有实机证据：`backups/delivery_20261022/flash_20261022.log`、`boot_log_20261022_boot.log`。
- 未打开串口、未复位设备、未烧录。

## 回滚方式

本阶段仅新增文档目录，无代码回滚需求。如需撤销，删除 `docs/phase2_acceptance_20261022/` 即可；不影响固件、补丁、交付目录或设备状态。
