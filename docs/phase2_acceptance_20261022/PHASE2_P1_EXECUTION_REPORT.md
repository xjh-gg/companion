# PHASE2_P1_EXECUTION_REPORT — LED、按键、提醒重试执行记录

执行时间：2026-10-22 21:20–21:25
执行范围：P1-1 陪伴 LED 物理反馈、P1-2 按键接入、P1-3 犯困提醒有限重试。

## 1. 代码修改

| 文件 | 修改 |
|---|---|
| `main/led/led.h` | 新增 `Led::SetCompanionEffect(r,g,b,interval_ms,times)` 虚接口，默认空实现 |
| `main/led/single_led.h/.cc` | SingleLed 实现陪伴灯效：常亮/持续闪烁/有限次数闪烁，复用现有 WS2812 GPIO1 |
| `main/companion/companion_led.cc/.h` | 由占位日志改为真实调用 `Board::GetInstance().GetLed()`；仅 XiaoZhi idle 时应用，回到 idle 自动恢复 |
| `main/boards/common/board.h` | 新增可选 `GetKeyBits(uint8_t&)`，默认不支持 |
| `main/boards/atk-dnesp32s3/atk_dnesp32s3.cc` | XL9555 增加非 abort 的 PORT1 读取；板级实现 KEY0–3 输入 |
| `main/companion/companion_service.cc` | 传感器任务 50ms 扫描 KEY0–3，3 拍去抖，下降沿产生 `EVT_KEY_PRESSED`；主任务周期调用 `companion_led_tick()` |
| `main/companion/state_machine.c` | 进入犯困态首次请求提醒，之后每 30s 重试，最多 3 次；受理后停止；待机初始化请求慢闪 |
| `tests/host/test_main.c` | 新增 4 项犯困提醒重试测试 |

## 2. 验证结果

- 主机测试：C **77 PASS / 0 FAIL**，C++ **5 PASS / 0 FAIL**，合计 **82 PASS / 0 FAIL**，退出码 0。
- 构建：`idf.py -B build_companion build` 成功，退出码 0。
- 新固件：`firmware/xiaozhi_p1_led_key.bin`，2,755,792 字节，SHA-256 `7FFA56D15177FAA4829A1D34B5AE6A7486B188757871D133C80009852FC6010C`。
- 增量补丁：`companion_p1_led_key_20261022.patch`，16,243 字节，SHA-256 `83A47943E25A154D3F54A93FCAF102AD7771E7149DB23D396F4F5D39F20A7F19`；正向应用 10/10 文件哈希一致。
- 烧录：COM11、460800、偏移 `0x20000`，写入 2,755,792 字节，哈希校验通过，MAC `a4:cb:8f:d6:12:64`，退出码 0。
- 启动日志：`boot_log_p1_led_key_20261022.log`。

## 3. 实机日志证据

- `COMPANION_LED: 陪伴 LED 模式请求: blink_slow（XiaoZhi 非 idle，暂缓）`
- `COMPANION_LED: XiaoZhi 回到 idle，恢复陪伴 LED 模式: blink_slow`
- 启动 40 秒无 panic、abort、WDT、Backtrace；Wi-Fi 获得 `<device_ip>`，MQTT 连接成功。
- AP3216C 心跳 ALS 117–119、PS 0–10，`传感器=ok/ok`，队列丢弃 0。
- 按键扫描未产生错误或崩溃；日志中没有按键按下记录（本次未实际按键）。

## 4. 验收状态

| 项目 | 状态 | 说明 |
|---|---|---|
| LED 代码路径 | PASS | 启动日志确认 idle 后应用 `blink_slow` |
| LED 物理闪烁 | 待人工确认 | Agent 无法目视确认灯珠 |
| 按键扫描接入 | PASS | 代码路径生效，无非 abort I2C 读取，无崩溃 |
| 按键物理触发 | 待人工确认 | 需实际按下 KEY0–3 并观察 `按键按下` 日志/状态变化 |
| 犯困提醒重试 | PASS（主机） | 首次拒绝、30s/60s 重试、受理后停止，4 项测试通过 |
| 60 分钟实机提醒 | 待人工/实机 | 仍需真实等待 60 分钟 |

## 5. 待人工验证

1. 观察 idle 时 GPIO1 是否按约 1s 周期闪烁；拿起/摇晃时是否变为快闪。
2. 按下 KEY0–3，检查串口是否出现 `COMPANION: 按键按下`，并观察状态迁移。
3. 等待 60 分钟，确认犯困提醒与重试行为；不得改短正式参数。

## 6. 物理确认（2026-10-23）

用户实机确认：

- 待机时 GPIO1 红灯约 1 秒慢闪：**已确认**。
- 拿起/晃动后约 200ms 快闪，30 秒后回慢闪：**已确认**。
- KEY0–3 按键触发“被关注”与快闪：**已确认**（日志含 `PORT1=0x7F/0xBF/0xDF/0xEF` 四次按键）。

对应固件：`xiaozhi_gpio_led_20261023.bin`，2,748,048 字节，SHA-256 `24B70F8343013A496C67FF6762FAE103D4C5640638BF1CA12E3B4EAF7A3AF01C`。
根因修正：GPIO1 为普通低电平点亮红灯，已将 XiaoZhi 板级从 WS2812 `SingleLed` 改为 `GpioLed(GPIO1, 1)`。
