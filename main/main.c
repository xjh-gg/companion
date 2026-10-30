#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>

#include "iic.h"
#include "xl9555.h"
#include "key.h"
#include "buzzer.h"
#include "led.h"
#include "qma6100p.h"
#include "ap3216c.h"
#include "at24c02.h"

#include "motion_detect.h"
#include "event_queue.h"
#include "state_machine.h"
#include "config.h"
#include "event_log.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "voice.h"
#include "voice_player.h"

static const char *TAG = "MAIN";

/* 开机验证音门控：
 * 本次 M5 调试置 1，用于实听确认喇叭出音 + 自动触发一次录音自测回采（能量打印）。
 * 交付时改回 0（不开机验证音，录音自测亦随之关闭）。 */
#define VOICE_BOOT_TONE 0

/* 中文人声 PCM 播放自测门控（独立于 VOICE_BOOT_TONE，二者互不影响）：
 * 置 1 时开机一次性播放 main/assets/welcome.pcm（官方小智 welcome.ogg 转出，
 * 16kHz/16bit/双声道/小端），用于验证 voice_play() 能播放真实人声。
 * 默认 0，不影响正常启动流程；验证完成后改回 0 即可恢复。 */
#define VOICE_SPEECH_TEST 0

/* 临时测试开关（默认 0）：置 1 时开机约 3 秒后播放一次犯困"叮咚"提醒音，
 * 用于快速验证 voice_player 音频闭环（请求→任务→voice_play→排空→回空闲）。
 * 独立于正式业务与 VOICE_SPEECH_TEST/VOICE_BOOT_TONE；测试后改回 0 即恢复。 */
#define VOICE_SLEEPY_TEST 0

/* 接近检测阈值（PS 值，需实测调整） */
#define PS_NEAR_THRESHOLD   50
/* 环境光过暗判定阈值（ALS 原始值） */
#define ALS_DARK_THRESHOLD  50
/* 环境光恢复阈值：与进入阈值形成迟滞带，避免临界值附近反复切换 */
#define ALS_RECOVER_THRESHOLD  60
/* 暗光/恢复判定所需的连续采样次数（sensor_task 每 50ms 采样一次） */
#define ALS_DEBOUNCE_COUNT     3

static TaskHandle_t s_sensor_task = NULL;
static TaskHandle_t s_key_task    = NULL;
static TaskHandle_t s_led_task    = NULL;
static TaskHandle_t s_buzzer_task = NULL;
static TaskHandle_t s_main_task   = NULL;
static TaskHandle_t s_net_task    = NULL;

/* ================= sensor_task：读传感器并投递事件 ================= */
static void sensor_task(void *arg)
{
    motion_detect_t *md = motion_detect_create();
    if (md == NULL) {
        ESP_LOGE(TAG, "motion_detect_create 失败，退出 sensor_task");
        vTaskDelete(NULL);
        return;
    }

    /* 暗光状态迟滞去抖（仅本任务访问）：dark_state 为当前判定，
     * dark_cnt/bright_cnt 分别为连续偏暗/连续达到恢复阈值的采样计数 */
    bool    dark_state = false;
    uint8_t dark_cnt   = 0;
    uint8_t bright_cnt = 0;

    while (1) {
        /* 读三轴加速度：失败时跳过本次，避免 motion_detect 误判 */
        float gx, gy, gz;
        if (qma6100p_read_g(&gx, &gy, &gz) == ESP_OK) {
            motion_event_t m = motion_detect_update(md, gx, gy, gz);
            if (m == MOTION_PICK_UP) {
                ESP_LOGI(TAG, "检测到拿起 (x=%.2f y=%.2f z=%.2f)", gx, gy, gz);
                event_queue_send(EVT_PICKED_UP, 0);
            } else if (m == MOTION_SHAKE) {
                ESP_LOGI(TAG, "检测到摇晃 (x=%.2f y=%.2f z=%.2f)", gx, gy, gz);
                event_queue_send(EVT_SHAKE, 0);
            }
        } else {
            ESP_LOGW(TAG, "QMA6100P 读取失败，跳过本次");
        }

        /* 读环境光 / 接近（两者独立判定，PS 过阈值不影响 ALS 采样） */
        ap3216c_data_t d;
        if (ap3216c_read(&d) == ESP_OK) {
            /* 暗光状态仅记录日志：不投递事件、不改变状态/LED/蜂鸣器、不写 EEPROM。
             * 迟滞去抖：连续 3 次低于进入阈值才判暗；连续 3 次达到恢复阈值才判亮；
             * 落在两阈值之间时维持当前判定，避免临界值附近反复切换。 */
            if (d.als < ALS_DARK_THRESHOLD) {
                bright_cnt = 0;
                if (!dark_state && ++dark_cnt >= ALS_DEBOUNCE_COUNT) {
                    dark_cnt = 0;
                    dark_state = true;
                    ESP_LOGI(TAG, "环境进入暗光 (ALS=%d)", d.als);
                }
            } else if (d.als >= ALS_RECOVER_THRESHOLD) {
                dark_cnt = 0;
                if (dark_state && ++bright_cnt >= ALS_DEBOUNCE_COUNT) {
                    bright_cnt = 0;
                    dark_state = false;
                    ESP_LOGI(TAG, "环境光照恢复 (ALS=%d)", d.als);
                }
            } else {
                /* 迟滞带内：保持当前判定，连续计数作废 */
                dark_cnt   = 0;
                bright_cnt = 0;
            }

            if (d.ps > PS_NEAR_THRESHOLD) {
                ESP_LOGI(TAG, "手靠近! PS=%d", d.ps);
                event_queue_send(EVT_HAND_NEAR, 0);
            }
        } else {
            ESP_LOGW(TAG, "AP3216C 读取失败，跳过本次");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ================= key_task：扫描按键并投递事件 ================= */
static void key_task(void *arg)
{
    while (1) {
        uint8_t boot = key_scan();
        uint8_t k = xl9555_key_scan();
        if (boot || k) {
            ESP_LOGI(TAG, "按键: BOOT=%d KEY=%d", boot, k);
            event_queue_send(EVT_KEY_PRESSED, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ================= led_task：LED 展示 =================
 * LED 联动由状态机完成（led 模块内部已有定时器）。
 * 本任务保留框架，仅作心跳，便于后续扩展。 */
static void led_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ================= buzzer_task：蜂鸣器播放 =================
 * 蜂鸣器由状态机直接触发（buzzer 阻塞式，vTaskDelay）。
 * 本任务保留框架，便于后续改为独立节拍控制。 */
static void buzzer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ================= voice_test_task：M5 播放最小闭环自测 =================
 * 开机一次性向喇叭播放 ~1s 的 1kHz 正弦波（16bit 双声道、16kHz），
 * 用于验证 ES8388 + I2S 播放链路。听到明显"嘟"声即代表链路打通。
 * 验证结束后自删除。 */
static void voice_test_task(void *arg)
{
    const uint32_t fs = 16000, freq = 1000;
    const uint32_t ms = 1000, ch = 2, bytes = fs * ch * 2 * ms / 1000;
    uint8_t *buf = malloc(bytes);
    if (!buf) {
        ESP_LOGE(TAG, "voice_test: 分配音频缓冲失败(%u)", (unsigned)bytes);
        vTaskDelete(NULL);
        return;
    }
    /* 生成 1kHz 正弦（16bit 小端，左右声道相同） */
    for (uint32_t i = 0; i < fs * ms / 1000; i++) {
        int16_t s = (int16_t)(10000 * sinf(2.0f * 3.1415926f * freq * i / fs));
        for (uint32_t c = 0; c < ch; c++) {
            uint32_t o = (i * ch + c) * 2;
            buf[o]     = s & 0xff;
            buf[o + 1] = (s >> 8) & 0xff;
        }
    }
    ESP_LOGI(TAG, "voice_test: 播放 %uHz 正弦约 %ums...", (unsigned)freq, (unsigned)ms);
    esp_err_t ret = voice_play(buf, bytes);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "voice_test: 播放完成（应能听到 %uHz 提示音）", (unsigned)freq);
    } else {
        ESP_LOGE(TAG, "voice_test: 播放失败: %s", esp_err_to_name(ret));
    }
    free(buf);

    /* M5 调试自测：放音后自动触发 ~2.6s 录音回采，打印能量统计以验证 ADC 有效 */
    if (voice_record_start() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(3200));
        voice_record_stop();
        ESP_LOGI(TAG, "voice_test: 录音自测结束");
    } else {
        ESP_LOGW(TAG, "voice_test: 录音自测未启动，忽略");
    }

    vTaskDelete(NULL);
}

/* ================= voice_speech_test_task：中文人声 PCM 一次性播放自测 =================
 * 只读引用嵌入式 PCM 资源（不复制到 malloc），分块调用 voice_play() 播放一次。
 * 仅由 VOICE_SPEECH_TEST=1 时创建，播完自删除；不修改 voice_test_task 与任何驱动。 */
extern const uint8_t welcome_pcm_start[] asm("_binary_welcome_pcm_start");
extern const uint8_t welcome_pcm_end[]   asm("_binary_welcome_pcm_end");

static void voice_speech_test_task(void *arg)
{
    size_t len = (size_t)(welcome_pcm_end - welcome_pcm_start);
    if (len == 0) {
        ESP_LOGE(TAG, "voice_speech: PCM 资源为空");
        vTaskDelete(NULL);
        return;
    }
    /* 每帧 16bit×2ch = 4 字节，长度须为 4 的整数倍，保证采样对齐 */
    if (len % 4 != 0) {
        ESP_LOGE(TAG, "voice_speech: PCM 长度 %u 非 4 的倍数，未对齐", (unsigned)len);
        vTaskDelete(NULL);
        return;
    }

    /* 分块 ≤32000 字节；32000 为 4 的倍数，保持 16-bit 双声道采样对齐 */
    const size_t chunk = 32000;
    const uint32_t fs = 16000, ch = 2, bytes = 2;  /* 采样率/声道/每样本字节 */
    const uint32_t bytes_per_sec = fs * ch * bytes;  /* 64000 字节/秒 */

    const uint32_t play_ms = (uint32_t)(len * 1000 / bytes_per_sec) + 300;

    for (int round = 1; round <= 3; round++) {
        ESP_LOGI(TAG, "voice_speech: 第 %d/3 次播放开始（%u 字节，约 %u ms）",
                 round, (unsigned)len, (unsigned)(len * 1000 / bytes_per_sec));

        size_t off = 0;
        while (off < len) {
            size_t n = len - off;
            if (n > chunk) {
                n = chunk;
            }
            esp_err_t ret = voice_play(welcome_pcm_start + off, n);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "voice_speech: 第 %d 次播放失败(偏移 %u): %s",
                         round, (unsigned)off, esp_err_to_name(ret));
                vTaskDelete(NULL);
                return;
            }
            off += n;
        }

        /* voice_play 返回仅代表数据已送入 DMA，不代表播完；按总时长估算并加余量 */
        ESP_LOGI(TAG, "voice_speech: 第 %d/3 次播放数据已写完，等待约 %u ms",
                 round, (unsigned)play_ms);
        vTaskDelay(pdMS_TO_TICKS(play_ms));

        if (round < 3) {
            ESP_LOGI(TAG, "voice_speech: 第 %d/3 次完成，间隔 2 秒", round);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    ESP_LOGI(TAG, "voice_speech: 3 次播放全部完成");
    vTaskDelete(NULL);
}

/* ================= voice_sleepy_test_task：犯困提醒音频闭环一次性自测（临时） =================
 * 仅 VOICE_SLEEPY_TEST=1 时创建：开机延时后向 voice_player 提交一次 VOICE_SRC_SLEEPY，
 * 验证「请求→任务→voice_play→排空→回空闲」整条链路；不循环、不修改任何驱动。 */
extern const uint8_t sleepy_pcm_start[] asm("_binary_sleepy_pcm_start");
extern const uint8_t sleepy_pcm_end[]   asm("_binary_sleepy_pcm_end");

static void voice_sleepy_test_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));   /* 等 boot 稳定、voice_player 已就绪 */
    size_t len = (size_t)(sleepy_pcm_end - sleepy_pcm_start);
    esp_err_t ret = voice_player_play(sleepy_pcm_start, len, VOICE_SRC_SLEEPY);
    ESP_LOGI(TAG, "voice_sleepy_test: 提交犯困提醒 ret=%s",
             ret == ESP_OK ? "OK" : esp_err_to_name(ret));
    vTaskDelete(NULL);
}

/* ================= main_task：状态机超时检测 + 心跳 ================= */
static void main_task(void *arg)
{
    uint32_t tick_div = 0;
    while (1) {
        /* 消费事件队列（单消费者），非阻塞取 */
        event_t evt;
        while (event_queue_recv(&evt, 0)) {
            state_machine_on_event(evt);
        }

        /* 每 10 个 100ms 周期（约 1s）调用一次状态机 tick */
        tick_div++;
        if (tick_div >= 10) {
            tick_div = 0;
            state_machine_tick();
            ESP_LOGI(TAG, "心跳: 状态=%s 犯困定时=%u分钟",
                     (state_machine_get_state() == STATE_IDLE) ? "待机" :
                     (state_machine_get_state() == STATE_HAPPY) ? "被关注" :
                     (state_machine_get_state() == STATE_INTERACT) ? "互动中" :
                     (state_machine_get_state() == STATE_SLEEPY) ? "犯困" : "报警",
                     (unsigned)state_machine_get_sleepy_timeout_min());
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ================= network_task：WiFi 连接等待 + MQTT 周期上报 =================
 * M4：独立网络任务（优先级 2，低于本地各任务）。仅负责等待 WiFi 连上后启动
 * MQTT 并周期上报状态。WiFi 连不上 / 断线时本地传感器与状态机照常运行，
 * 网络失败一律降级处理，绝不阻塞本地功能。 */
static void network_task(void *arg)
{
    const TickType_t conn_timeout = pdMS_TO_TICKS(CONFIG_COMPANION_WIFI_CONNECT_TIMEOUT_S * 1000);
    const TickType_t report_period = pdMS_TO_TICKS(CONFIG_COMPANION_MQTT_REPORT_PERIOD_S * 1000);

    while (1) {
        /* 第一段：等 WiFi 连上（带超时）。超时则等待一段时间后重新尝试 */
        TickType_t start = xTaskGetTickCount();
        while (!wifi_manager_is_connected()) {
            if (xTaskGetTickCount() - start >= conn_timeout) {
                ESP_LOGW(TAG, "等待 WiFi 连接超时(%ds)，本地功能不受影响，稍后重试",
                         CONFIG_COMPANION_WIFI_CONNECT_TIMEOUT_S);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));   /* 5s 后回到外层重新等待连接 */
            continue;
        }

        /* 第二段：WiFi 已连上，启动 MQTT 并周期上报 */
        mqtt_manager_start();
        while (wifi_manager_is_connected()) {
            mqtt_manager_publish_status();
            vTaskDelay(report_period);
        }

        /* WiFi 断开：esp-mqtt 会在网络恢复后自动重连，这里回到外层重新等待 */
        ESP_LOGW(TAG, "WiFi 连接丢失，network_task 回到等待重连状态");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Companion 启动 ===");

    /* 初始化各模块（顺序有依赖） */
    iic_init();
    iic_scan();                     /* 扫描总线，确认设备在线 */
    xl9555_init();
    key_init();
    buzzer_init();
    led_init();

    /* M5：语音子系统（ES8388 + I2S）。需在 I2C 总线与 XL9555 初始化之后。
     * 失败只降级为"无语音"，不影响本地功能继续运行。 */
    if (voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音子系统初始化失败，本次不开提示音，其余功能正常");
    } else {
        /* 独立播放模块：业务侧经 voice_player 串行调用 voice_play */
        if (voice_player_init() != ESP_OK) {
            ESP_LOGW(TAG, "语音播放模块初始化失败，业务语音不可用");
        }
    }

    if (qma6100p_init() != ESP_OK) {
        ESP_LOGW(TAG, "QMA6100P 初始化失败，继续运行");
    }
    if (ap3216c_init() != ESP_OK) {
        ESP_LOGW(TAG, "AP3216C 初始化失败，继续运行");
    }

    event_queue_init();
    state_machine_init();

    /* M3：配置管理与环形事件日志（需在 iic 初始化后，状态机可用后） */
    if (at24c02_init() != ESP_OK) {
        ESP_LOGW(TAG, "AT24C02 初始化失败，配置将使用默认值、日志不可用");
    }
    config_init();
    event_log_init();

    /* 开机提示音 + 呼吸灯 */
    buzzer_beep(100);
    led_set_mode(LED_BREATH);

    /* M4：启动 WiFi（非阻塞，失败不影响本地功能） */
    if (wifi_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 初始化失败，网络功能不可用，本地功能正常");
    }

    /* 创建 5 个任务：传感器优先，系统任务最低优先级 */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, &s_sensor_task);
    xTaskCreate(key_task,     "key_task",     2048, NULL, 4, &s_key_task);
    xTaskCreate(led_task,     "led_task",     2048, NULL, 3, &s_led_task);
    xTaskCreate(buzzer_task,  "buzzer_task",  2048, NULL, 3, &s_buzzer_task);
    xTaskCreate(main_task,    "main_task",    4096, NULL, 1, &s_main_task);

    /* M4：网络任务优先级 2（介于 buzzer 与 main 之间），独立等待 WiFi + 周期上报 */
    xTaskCreate(network_task, "network_task", 4096, NULL, 2, &s_net_task);

    /* M5：语音播放最小闭环自测任务（仅开机验证一次，播完自删除）。
     * VOICE_BOOT_TONE=0 时不开机验证音，避免每次开机都发声。 */
#if VOICE_BOOT_TONE
    xTaskCreate(voice_test_task, "voice_test", 4096, NULL, 3, NULL);
#endif

    /* M5：中文人声 PCM 播放自测任务（独立开关，默认关闭；播完自删除）。
     * VOICE_SPEECH_TEST=1 时开机播放一次 main/assets/welcome.pcm 验证真实人声。 */
#if VOICE_SPEECH_TEST
    xTaskCreate(voice_speech_test_task, "voice_speech", 4096, NULL, 3, NULL);
#endif

    /* 临时：犯困提醒音频闭环一次性自测（VOICE_SLEEPY_TEST=1 时开启，默认 0） */
#if VOICE_SLEEPY_TEST
    xTaskCreate(voice_sleepy_test_task, "voice_sleepy_test", 4096, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "初始化完成，任务已创建");
}