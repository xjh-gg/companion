# DEVELOPMENT LOG — 2026-10-22

## 本轮目标

在保留 XiaoZhi 语音、MQTT AES 修复与官方音频链路的前提下，增加统一事件历史、光照事件、MCP 事件查询与状态字段。

## 修改文件

| 文件 | 修改 | 原因 |
|---|---|---|
| `main/companion/companion_event_log.h/.c` | 新增 | 24 条环形事件历史，供 MCP 查询 |
| `main/companion/event_queue.h` | 增加 `EVT_BRIGHT` | 光照恢复事件 |
| `main/companion/state_machine.c` | 增加事件名映射 | 日志/MCP 可读 |
| `main/companion/companion_service.cc` | 光照事件入队、历史记录、状态字段、`get_recent_events` | 形成感知→事件→状态→MCP 闭环 |
| `main/CMakeLists.txt` | 加入新源文件 | 构建 |
| `tests/host/stubs/freertos/FreeRTOS.h` | 增加 portMUX 桩 | 主机测试 |
| `tests/host/run_tests.ps1` | 编译新模块 | 主机测试 |
| `tests/host/test_main.c` | 增加 13 项事件历史测试 | 验证环形/limit/无效值 |

未修改：ES8388/I2S/DMA/PA/XL9555/I2C、`application.cc`、`audio_service.cc`、`mqtt_protocol.cc/.h`、WebSocket、分区/sdkconfig、NVS。

## 验证

1. 构建：exit 0，0 warning/error，2,753,616 B，SHA-256 `03E69216…4AA885`。
2. 主机测试：59 PASS / 0 FAIL。
3. 烧录：COM17，普通 flash，未擦除，Hash verified。
4. 启动：两个 MCP 工具注册、传感器 ok、WiFi/MQTT 连接、idle，无解密错误/崩溃。
5. 语音：上一版固件 1 轮证据；当前扩展版待人工。

## 备份与回滚

- 修改前备份：`backups/embodied_20261022_1440/`
  - `companion_service.cc`、`event_queue.h`、`state_machine.c`、`CMakeLists.txt`
  - `test_main.c`、`run_tests.ps1`、`FreeRTOS.h`
- 回滚：
  1. 将上述备份覆盖回原路径；
  2. 删除 `main/companion/companion_event_log.c/.h`（本轮新增）；
  3. 重新构建。
- 整机回滚：`backups/xiaozhi_194_flash_20261021_read1.bin`（需另行授权）。

## 未完成

- 3 轮语音与听感、运动/光照物理测试、MCP 端到端、断网重连、通道重建、30–60 分钟稳定性。
- LED/执行器未实现，未猜测 GPIO。
