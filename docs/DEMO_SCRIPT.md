# DEMO SCRIPT — 3–5 分钟

## 准备

- 设备已烧录 `03E69216…` 固件，连接 2.4GHz 热点，串口 115200 监视。
- 说明：LED 当前为占位，执行器未安装；演示以语音、传感器、MCP 状态查询为主。

## 流程

### 0:00–0:30 开场与启动

- 上电/复位，串口显示：
  - `AP3216C: 初始化成功 (0x1E)`
  - `QMA6100P: 芯片 ID 校验通过: 0x90`
  - `MCP: Add tool: self.companion.get_status` / `get_recent_events`
  - `MQTT: Connected to endpoint`、`STATE: idle`

### 0:30–1:30 语音交互

- 说「你好小智」。
- 预期：`Wake word detected` → `STATE: listening` → `speaking`；询问「你现在状态怎么样？」。
- 讲点：XiaoZhi 原有 ASR/LLM/TTS 链路未改动；工具由模型按需调用。

### 1:30–2:30 运动感知

- 拿起设备并轻摇。
- 预期日志：`检测到拿起` / `检测到摇晃`；状态 `待机 -> 被关注`。
- 用 MCP 查询或问「刚刚发生了什么？」查看事件历史。

### 2:30–3:30 光照感知

- 遮住 AP3216C（U5）约 3 秒再移开。
- 预期：`环境进入暗光` → `环境光照恢复`；`EVT_DARK/EVT_BRIGHT` 进入历史。
- 说明阈值 50/60、3 次去抖，未做现场标定。

### 3:30–4:30 MCP 状态查询

- 通过服务端 MCP/语音调用 `self.companion.get_status`：
  - 展示 `state`、`speech_state`、`als/ps`、`light_dark`、`sensor_ok`、`event_total`。
- 调用 `self.companion.get_recent_events`（limit=8）。
- 强调只读、无执行器动作。

### 4:30–5:00 边界与限制

- 明确：LED 未实现、电机未接入、MQTT 自定义遥测未扩展、AES 修复保留。
- 断网/通道重建为人工验收项。
