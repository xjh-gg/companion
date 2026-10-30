# TEST REPORT — 2026-10-22

> **2026-10-22 17:43 复核**：COM17 当前不存在（仅蓝牙串口与 CH340 COM13），设备端项目前不可验收；本次仅重新运行主机测试（59/0）并增量构建（哈希仍为 03E6…）。下方实机记录为 15:48 历史证据，不代表当前设备状态。详见 `backups/final_acceptance_20261022_1605/`。

## 1. 构建

| 项 | 值 |
|---|---|
| 源码 | `<xiaozhi_source>` |
| 命令 | `idf.py -B build_companion -D IDF_TARGET=esp32s3 build` |
| 退出码 | 0 |
| warning / error | 0 / 0 |
| 固件 | `build_companion\xiaozhi.bin` |
| 大小 | 2,753,616 B |
| SHA-256 | `03E69216E083A948208F2CA170392A4B1BA43B179AF31A7C2D8FC3E2CE4AA885` |
| 分区余量 | 1,375,152 B（约 33.3%） |
| 日志 | `backups/embodied_build_20261022.log` |

## 2. 主机测试

- 命令：`powershell -ExecutionPolicy Bypass -File tests\host\run_tests.ps1`
- 结果：**PASS=59 / FAIL=0，退出码 0**
- 新增覆盖：事件历史环形容量、最新在前、limit、EVT_NONE 忽略、无效数据标志、事件类型。
- 原 46 项（状态机/队列）全部保留通过。
- 日志：`backups/host_tests_embodied_20261022.log`

## 3. 烧录

| 项 | 值 |
|---|---|
| 命令 | `idf.py -B build_companion -p COM17 -b 460800 flash` |
| 结果 | 退出码 0，`Hash of data verified`，`Hard resetting` |
| 擦除 | 未执行（`ERASE_COUNT=0`） |
| 日志 | `backups/embodied_flash_20261022.log` |

## 4. 实机启动

启动日志 `backups/embodied_boot_20261022.log`：

- `AP3216C: 初始化成功 (0x1E)`、`QMA6100P: 芯片 ID 校验通过: 0x90`
- `MCP: Add tool: self.companion.get_status`
- `MCP: Add tool: self.companion.get_recent_events`
- WiFi 连接成功，`MQTT: Connected to endpoint`，`STATE: idle`
- 心跳：`ALS≈108–111 PS=0–18 传感器=ok/ok 队列丢弃=0`
- 无 `Failed to decrypt`、`Guru Meditation`、watchdog、panic

## 5. 语音回归

- **上一版 AES 修复固件（F80C）**：捕获 1 轮真实对话，`Wake word detected: 你好小智` → `>> 你好小智` → `<< 你好呀～/很高兴见到你！/有什么我可以帮你的吗？` → `STATE: speaking/listening`，无解密错误。
- **当前事件/MCP 扩展固件（03E6）**：仅完成启动与空闲验证；未捕获到新的完整语音轮次。
- 实际扬声器听感：**未由人耳确认**。

## 6. 手工/未完成项

| 项目 | 状态 | 原因 |
|---|---|---|
| 3 轮语音（03E6） | 未完成 | 需人说话/听音 |
| 拿起、摇晃、遮挡光照 | 未执行 | 需物理操作 |
| MCP `tools/call` 端到端 | 未执行 | 需服务端/客户端 |
| 断网重连 5–10 次 | 未执行 | 需控制热点，未擅自切换 |
| 通道关闭/重建 | 未执行 | 需语音触发 |
| LED 控制 | 未实现 | 硬件归属未确认，禁止猜 GPIO |
| 执行器 | 未实现 | 无硬件/驱动 |
| 30–60 分钟稳定性 | 未完成 | 实际空闲观察约 2 分钟（03E6）/约 7.5 分钟（F80C） |

## 7. 结论

源码、构建、主机测试、烧录、启动、传感器、MCP 注册均通过；语音链路在上一版固件有 1 轮证据，当前扩展版仍待人工 3 轮对话与听音确认。不得把上述结果表述为“完整实机验收通过”。
