# BSP 模块 API 接口

## IIC 模块 (iic.h)

```c
/* 初始化 I2C 总线 */
esp_err_t iic_init(void);

/* 扫描总线上的设备，打印在线地址 */
void iic_scan(void);

/* 全局总线句柄，其他模块用它 add_device */
extern i2c_master_bus_handle_t g_i2c_bus;
```

## LED 模块 (led.h)

```c
/* 初始化红灯引脚 */
void led_init(void);

/* 设置 LED 闪烁模式（命名与 led.h 一致：缺失 BLINK_SLOW/BLINK_FAST/BLINK_ALARM） */
typedef enum {
    LED_OFF,          // 熄灭
    LED_ON,           // 常亮
    LED_BLINK_SLOW,   // 慢闪：待机平静，2s 一周期
    LED_BLINK_FAST,   // 快闪：被关注开心，0.2s 一周期
    LED_BLINK_ALARM,  // 急促闪：报警，0.1s 一周期
    LED_BREATH,       // 呼吸：柔和渐变（PWM 方式）
} led_mode_t;

void led_set_mode(led_mode_t mode);
```

## XL9555 模块 (xl9555.h)

```c
/* 初始化 IO 扩展芯片 */
esp_err_t xl9555_init(void);

/* 读某个 IO 的电平（pin: 0x0001~0x8000） */
int xl9555_pin_read(uint16_t pin);

/* 写某个 IO 的电平 */
uint16_t xl9555_pin_write(uint16_t pin, int val);

/* 按键扫描，返回按键值（0=无, 1=KEY0, 2=KEY1...） */
uint8_t xl9555_key_scan(void);
```

## QMA6100P 模块 (qma6100p.h)

```c
/* 初始化加速度计 */
esp_err_t qma6100p_init(void);

/* 读三轴原始数据 */
typedef struct {
    float acc_x;
    float acc_y;
    float acc_z;
} qma_data_t;

void qma6100p_read(qma_data_t *data);

/* 事件检测：返回是否发生了拿起/摇晃事件 */
typedef enum {
    EVENT_NONE = 0,
    EVENT_PICK_UP,    // 拿起
    EVENT_SHAKE,      // 摇晃
} qma_event_t;

qma_event_t qma6100p_detect_event(void);
```

## AP3216C 模块 (ap3216c.h)

```c
/* 初始化光照/接近传感器 */
esp_err_t ap3216c_init(void);

/* 读光照值 (0~65535) */
uint16_t ap3216c_read_als(void);

/* 读接近值 (0~255，越大越近) */
uint8_t ap3216c_read_ps(void);

/* 检测手是否靠近 (< 5cm) */
bool ap3216c_hand_near(void);
```

## BUZZER 模块 (buzzer.h)

```c
/* 初始化蜂鸣器引脚 */
void buzzer_init(void);

/* 播放提示音 */
typedef enum {
    BEEP_SHORT,    // 短嘀一声
    BEEP_DOUBLE,   // 双嘀
    BEEP_ALARM,    // 连续急促
} buzzer_tone_t;

void buzzer_play(buzzer_tone_t tone);
```

## AT24C02 模块 (at24c02.h)

```c
/* 初始化 EEPROM */
esp_err_t at24c02_init(void);

/* 读/写一个字节 */
esp_err_t at24c02_write(uint8_t addr, uint8_t data);
esp_err_t at24c02_read(uint8_t addr, uint8_t *data);
```

# main 层应用接口

## 事件队列 (event_queue.h)

```c
/* 统一事件枚举（覆盖所有事件源，含 M2 预留的语音交互事件） */
event_t { EVT_NONE, EVT_KEY_PRESSED, EVT_PICKED_UP, EVT_SHAKE,
          EVT_HAND_NEAR, EVT_DARK, EVT_TIMER_SLEEPY, EVT_ALARM,
          EVT_INTERACT_START, EVT_INTERACT_END };

esp_err_t event_queue_init(void);                 /* 队列深度 8 */
esp_err_t event_queue_send(event_t evt, uint32_t timeout_ms);   /* 生产者，非阻塞 */
bool      event_queue_recv(event_t *evt, uint32_t timeout_ms);  /* 状态机消费 */
```

> 说明：`EVT_DARK` 枚举暂时保留，但当前 `sensor_task` **不再投递**该事件；状态机收到 `EVT_DARK` 也**不会发生任何状态迁移**。暗光的进入/恢复仅由 `sensor_task` 去抖、迟滞后打印串口日志（进入阈值 50、恢复阈值 60、连续 3 次采样）。

## 拿起/摇晃检测 (motion_detect.h)

```c
motion_event_t { MOTION_NONE, MOTION_PICK_UP, MOTION_SHAKE };

motion_detect_t *motion_detect_create(void);      /* 失败返回 NULL */
motion_event_t   motion_detect_update(motion_detect_t *md, float gx, float gy, float gz);
void             motion_detect_reset(motion_detect_t *md);
void             motion_detect_destroy(motion_detect_t *md);
```

## 状态机 (state_machine.h)

```c
companion_state_t { STATE_IDLE, STATE_HAPPY, STATE_INTERACT, STATE_SLEEPY, STATE_ALARM };

void              state_machine_init(void);
void              state_machine_on_event(event_t evt);              /* 迁移并联动 LED/蜂鸣器 */
void              state_machine_tick(void);                         /* 1s 周期：超时/犯困/报警 */
companion_state_t state_machine_get_state(void);
void              state_machine_set_sleepy_timeout_min(uint32_t min);  /* 默认 60 分钟 */
```

## 配置管理 (config.h)

```c
typedef struct {
    uint32_t sleepy_timeout_min;   /* 犯困提醒间隔（分钟），默认 60 */
    uint8_t  volume;               /* 音量 0~100，默认 80（预留） */
    uint8_t  mode_enabled;         /* 模式开关（预留） */
    uint8_t  check;                /* 校验字节 */
} companion_config_t;

void               config_init(void);              /* 读 AT24C02，失败回默认值并写回 */
companion_config_t config_get(void);
esp_err_t          config_save(const companion_config_t *cfg);
```

## 环形事件日志 (event_log.h)

```c
esp_err_t event_log_init(void);                   /* AT24C02 偏移 16 起 240B */
esp_err_t event_log_record(event_t evt);          /* 最新覆盖最旧 */
int       event_log_read_recent(event_t *out, int max);   /* 从新到旧返回条数 */
```

## 语音抽象接口层 (voice.h)

```c
voice_session_state_t { VOICE_IDLE, VOICE_LISTENING, VOICE_THINKING, VOICE_SPEAKING };
voice_end_reason_t    { VOICE_END_NORMAL, VOICE_END_ERROR, VOICE_END_TIMEOUT, VOICE_END_ABORT };

esp_err_t voice_init(void);                        /* M5 空实现返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t voice_session_start(void (*on_end)(voice_end_reason_t reason));
esp_err_t voice_session_stop(voice_end_reason_t reason);
esp_err_t voice_play(const uint8_t *data, size_t len);
esp_err_t voice_record_start(void);
esp_err_t voice_record_stop(void);
voice_session_state_t voice_get_state(void);
```

## WiFi 连接管理 (wifi_manager.h)

```c
esp_err_t wifi_manager_init(void);                 /* 非阻塞连接 + 断线自动重连 */
bool      wifi_manager_is_connected(void);
int32_t   wifi_manager_get_rssi(void);             /* dBm，-127 表示不可用 */
```

## MQTT 上报 (mqtt_manager.h)

```c
esp_err_t mqtt_manager_start(void);                /* 内部读 COMPANION_MQTT_BROKER_URI */
void      mqtt_manager_publish_status(void);       /* 上报 JSON 到 companion/status */
void      mqtt_manager_publish(const char *topic, const char *payload, int qos);
```
