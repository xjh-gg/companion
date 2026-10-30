#include "voice.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "es8388_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio_codec_if.h"
#include "audio_codec_ctrl_if.h"

#include "iic.h"       /* g_i2c_bus（项目 I2C 总线句柄） */
#include "xl9555.h"    /* SPK_EN_IO 喇叭使能 */

/* =====================================================================
 * 语音子系统实装（M5 第一步：ES8388 + I2S 播放最小闭环）
 * ---------------------------------------------------------------------
 * 打通板载音频链路：ES8388(0x10) 经 I2C 控制，ESP32-S3 I2S0 以
 * 主模式输出 MCLK(IO3)/BCK(IO46)/WS(IO9)/DO(IO10)，并回采 DI(IO14)。
 * 开机即启用喇叭功放（XL9555 PORT0 bit2 SPK_EN_IO 置低，MD8002A SD 脚高有效关断）。
 *
 * 本轮先完成【播放】闭环：voice_play() 把 PCM 写入 I2S，可从喇叭听到声音。
 * 【录音】接口保留、暂未打通（返回 NOT_SUPPORTED），会话状态亦暂不维护。
 * ===================================================================== */

static const char *TAG = "VOICE";

/* --- 板载 I2S 引脚（正点原子 DNESP32-S3 官方资料） --- */
#define I2S_PORT       I2S_NUM_0
#define I2S_MCLK_IO    3
#define I2S_BCK_IO     46
#define I2S_WS_IO      9
#define I2S_DOUT_IO    10
#define I2S_DIN_IO     14

/* 播放采样格式：16kHz / 16bit / 双声道（左右声道均喂同一声，确保出声） */
#define PLAY_BITS      16
#define PLAY_CHANNELS  2
#define PLAY_FS        16000

/* ES8388 I2C 地址(8bit 形态，0x10 << 1)；底层接口会用 (addr>>1) 还原 7bit 地址 */
#define ES8388_I2C_ADDR ES8388_CODEC_DEFAULT_ADDR   /* 0x20 */

static esp_codec_dev_handle_t s_play_dev = NULL;   /* 播放句柄（含 IN_OUT 能力，录音后复用） */
static bool                  s_opened    = false;  /* 播放采样格式是否已 open */

/* 播放用采样信息 */
static const esp_codec_dev_sample_info_t s_play_fs = {
    .sample_rate     = PLAY_FS,
    .channel         = PLAY_CHANNELS,
    .bits_per_sample = PLAY_BITS,
    .channel_mask    = 0,     /* 0 = 不过滤，全部通道 */
    .mclk_multiple   = 0,     /* 0 = codec 默认 MCLK=fs*256 */
};

/* =====================================================================
 * ES8388 寄存器兜底与无声排查取证
 * ---------------------------------------------------------------------
 * esp_codec_dev 的 ES8388 驱动的 open/enable 流程在寄存器层面已经会
 * 解除 DAC 静音、使能 DAC 与 LOUT/ROUT 输出（DACPOWER=0x3C、DACCONTROL3
 * unmute 等）。日志里 "Start on mode:3" 证明 enable 已走通。为排查"寄存器/
 * DMA 全绿但喇叭无声"，这里：① 回读关键寄存器并打到串口作证据；
 * ② 在首次播放后强制再写一遍"已知可出声"的 DAC 输出配置兜底，
 *    杜绝驱动默认任何残留静音/未使能导致无声。
 * ===================================================================== */
#define ES_DACPOWER      0x04   /* DAC 功率/输出使能（0x3C=LOUT1/2+ROUT1/2） */
#define ES_MASTERMODE    0x08   /* I2S 主从 */
#define ES_ADCCONTROL2   0x0A   /* ADC 输入选择（默认 LINPUT1/RINPUT1） */
#define ES_ADCCONTROL4   0x0C   /* ADC 位宽/格式 */
#define ES_DACCONTROL1   0x17   /* DAC 位宽/格式 */
#define ES_DACCONTROL2   0x18   /* DAC Fs/MCLK ratio */
#define ES_DACCONTROL3   0x19   /* DAC 静音/去重（bit2=1 静音） */
#define ES_DACCONTROL4   0x1A   /* 左声道 DAC 音量 */
#define ES_DACCONTROL5   0x1B   /* 右声道 DAC 音量 */
#define ES_DACCONTROL16  0x26   /* LOUT1/RIN 输入选择（0x00=LIN1&RIN1） */
#define ES_DACCONTROL17  0x27   /* 左 DAC->mixer 使能 0dB（0x90） */
#define ES_DACCONTROL20  0x2A   /* 右 DAC->mixer 使能 0dB（0x90） */
#define ES_DACCONTROL21  0x2B   /* DAC L/R 使能（0x80=DAC on，0xC0=ADC+DAC） */
#define ES_DACCONTROL24  0x2E   /* L1 R1 L2 R2 音量（0x1E=0dB） */
#define ES_DACCONTROL25  0x2F
#define ES_DACCONTROL26  0x30   /* LOUT2 音量（喇叭），0x1E=0dB */
#define ES_DACCONTROL27  0x31   /* ROUT2 音量（喇叭），0x1E=0dB */

static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;  /* 复用项目 I2C 总线的 codec 控制接口 */

/* 回读 ES8388 关键寄存器打印（无声排查证据，直接经控制接口读） */
static void es8388_reg_dump(void)
{
    if (!s_ctrl_if) {
        return;
    }
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, ES_DACPOWER, 0x05, ES_MASTERMODE, 0x09,
        0x0A, 0x0B, 0x0C, 0x10, 0x11,
        ES_DACCONTROL1, ES_DACCONTROL2, ES_DACCONTROL3, ES_DACCONTROL4,
        ES_DACCONTROL5, ES_DACCONTROL16, ES_DACCONTROL17, ES_DACCONTROL20,
        ES_DACCONTROL21, ES_DACCONTROL24, ES_DACCONTROL25,
    };
    ESP_LOGI(TAG, "=== ES8388 关键寄存器回读 ===");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        int v = 0;
        int ret = s_ctrl_if->read_reg(s_ctrl_if, regs[i], 1, &v, 1);
        if (ret == 0) {
            ESP_LOGI(TAG, "REG[0x%02X] = 0x%02X", regs[i], v & 0xFF);
        } else {
            ESP_LOGE(TAG, "REG[0x%02X] 读回失败 ret=%d", regs[i], ret);
        }
    }
    ESP_LOGI(TAG, "=== dump 完成：0504=DAC功率 1919=mute 1A/1B=音量 26/27/2A=输出路由 ===");
}

/* 强制写入"已知可出声"的 DAC 输出配置兜底（解除任何残留静音/未使能） */
static void es8388_ensure_dac_output(void)
{
    if (!s_ctrl_if) {
        return;
    }
    uint8_t v;
    v = 0x00; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL3, 1, &v, 1);  /* 解除静音+去重 */
    v = 0x3C; s_ctrl_if->write_reg(s_ctrl_if, ES_DACPOWER, 1, &v, 1);     /* DAC + LOUT1/2 ROUT1/2 上电 */
    v = 0x00; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL16, 1, &v, 1); /* RIN 输入选 LIN1&RIN1 */
    v = 0x90; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL17, 1, &v, 1); /* 左 DAC->mixer 0dB */
    v = 0x90; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL20, 1, &v, 1); /* 右 DAC->mixer 0dB */
    v = 0x80; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL21, 1, &v, 1); /* DAC L/R 使能 */
    v = 0x1E; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL24, 1, &v, 1); /* 输出 0dB */
    v = 0x1E; s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL25, 1, &v, 1);
    v = 0x1E;
    if (s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL26, 1, &v, 1) != 0) {
        ESP_LOGE(TAG, "ES8388 LOUT2 音量(0x30) 写入失败");
    }
    v = 0x1E;
    if (s_ctrl_if->write_reg(s_ctrl_if, ES_DACCONTROL27, 1, &v, 1) != 0) {
        ESP_LOGE(TAG, "ES8388 ROUT2 音量(0x31) 写入失败");
    }
    ESP_LOGI(TAG, "ES8388 DAC 输出已强制使能：UNMUTE + DAC PWR + LOUT/ROUT 0dB（含喇叭 LOUT2/ROUT2）");
}

/* 录音参数：与播放同格式 16kHz/16bit/双声道，块长 100ms，自动限时约 2.5s 便于验证 */
#define REC_FS          16000
#define REC_CHANNELS    2
#define REC_BITS        16
#define REC_CHUNK_MS    100
#define REC_ITER_CNT    25

static TaskHandle_t  s_record_task = NULL;   /* 录音后台任务 */
static volatile bool s_record_stop  = false; /* 停止信号 */

static esp_err_t voice_ensure_opened(void);  /* 前向声明 */

static void voice_record_task(void *arg)
{
    const int chunk = REC_FS * REC_CHANNELS * 2 * REC_CHUNK_MS / 1000; /* 每帧字节数 */
    int16_t *buf = malloc(chunk);
    if (!buf) {
        ESP_LOGE(TAG, "record: 分配回采缓冲失败(%d)", chunk);
        s_record_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "record: 开始回采 %dHz/%dbit/%dch, 帧=%dB, 自动限时%ds",
             REC_FS, REC_BITS, REC_CHANNELS, chunk, REC_ITER_CNT * REC_CHUNK_MS / 1000);

    int iter = 0, n_samples = 0, nonzero = 0;
    int32_t sum = 0, max_abs = 0;
    while (!s_record_stop && iter < REC_ITER_CNT) {
        /* out_reconfig 期间 read 会返回成功但清零，正常计数即可 */
        if (esp_codec_dev_read(s_play_dev, buf, chunk) != 0) {
            ESP_LOGE(TAG, "record: esp_codec_dev_read 失败");
            break;
        }
        int32_t ssum = 0, smax = 0, snz = 0;
        for (int i = 0; i < chunk / 2; i++) {
            int32_t s = buf[i];
            if (s != 0) {
                snz++;
            }
            int32_t a = s < 0 ? -s : s;
            ssum += a;
            if (a > smax) {
                smax = a;
            }
        }
        double rms = (chunk / 2) ? (double)ssum / (chunk / 2) : 0.0;
        ESP_LOGI(TAG, "record[%2d] 帧能量 avg=%.0f peak=%d 非零=%d/%d",
                 iter, rms, (int)smax, (int)snz, chunk / 2);
        sum += ssum;
        if (smax > max_abs) {
            max_abs = smax;
        }
        nonzero += (int)snz;
        n_samples += chunk / 2;
        iter++;
    }
    ESP_LOGI(TAG, "record: 停止。共 %d 样本 平均幅=%.0f 峰值=%d 非零=%d（非零>0 即 ADC 有效）",
             n_samples, n_samples ? (double)sum / n_samples : 0.0, (int)max_abs, nonzero);
    free(buf);
    s_record_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_init(void)
{
    if (s_play_dev) {
        return ESP_OK;   /* 已初始化 */
    }

    /* 1. 使能喇叭功放：MD8002A 的 SHUTDOWN 脚高有效关断，故 XL9555 PORT0 bit2 置低才是开启 */
    esp_err_t ret = xl9555_pin_write(SPK_EN_IO, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "使能喇叭功放失败(SPK_EN): %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "喇叭功放已开启 (SPK_EN=0)");

    /* 2. 建立 I2S0 全双工标准模式通道（MCLK+BCK+WS+DO+DI） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    i2s_chan_handle_t tx_chan = NULL, rx_chan = NULL;
    ret = i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 建通道失败: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg   = I2S_STD_CLK_DEFAULT_CONFIG(PLAY_FS),
        .slot_cfg  = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg  = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DOUT_IO,
            .din  = I2S_DIN_IO,
        },
    };
    if ((ret = i2s_channel_init_std_mode(tx_chan, &std_cfg)) != ESP_OK ||
        (ret = i2s_channel_init_std_mode(rx_chan, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "I2S0 就绪  MCLK=%d BCK=%d WS=%d DO=%d DI=%d",
             I2S_MCLK_IO, I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO, I2S_DIN_IO);

    /* 3. 数据接口：I2S（复用 RX/TX 句柄） */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = rx_chan,
        .tx_handle = tx_chan,
        .clk_src   = 0,            /* 0 = 驱动默认时钟源 */
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "创建 I2S 数据接口失败");
        return ESP_FAIL;
    }

    /* 4. 控制接口：复用项目 I2C 总线（g_i2c_bus，SDA=41/SCL=42） */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr           = ES8388_I2C_ADDR,
        .bus_handle     = g_i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "创建 I2C 控制接口失败");
        return ESP_FAIL;
    }
    s_ctrl_if = ctrl_if;   /* 保留备用：寄存器回读/兜底写入复用同一总线 */

    /* 5. Codec 接口：ES8388，DAC+ADC 均启用（本轮先走 DAC 播放） */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8388_codec_cfg_t es_cfg = {
        .ctrl_if      = ctrl_if,
        .gpio_if      = gpio_if,
        .codec_mode   = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode  = false,     /* ESP 做 I2S 主，codec 从 */
        .pa_pin       = -1,        /* 喇叭功放已由 SPK_EN_IO 使能，不走 codec PA 脚 */
        .hw_gain      = { 0 },     /* 硬件增益由外围电路决定，不额外校准 */
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "初始化 ES8388 失败（校验 I2C/引脚）");
        return ESP_FAIL;
    }

    /* 6. 组装成 esp_codec_dev 设备 */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_play_dev) {
        ESP_LOGE(TAG, "创建 codec 设备失败");
        return ESP_FAIL;
    }

    /* 参考 dump：open 建立后（此时 enable 尚未执行，DAC 可能仍处于驱动默认静音态） */
    es8388_reg_dump();

    ESP_LOGI(TAG, "语音子系统就绪（ES8388 播放最小闭环）");
    return ESP_OK;
}

esp_err_t voice_session_start(void (*on_end)(voice_end_reason_t reason))
{
    /* M5 播放闭环阶段暂不启用会话。on_end 仅占位，不回调。 */
    (void)on_end;
    ESP_LOGW(TAG, "voice_session_start 未实现（会话驱动后置）");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_session_stop(voice_end_reason_t reason)
{
    /* M5 播放闭环阶段无会话可结束。reason 仅记录。 */
    (void)reason;
    ESP_LOGW(TAG, "voice_session_stop 未实现（会话驱动后置）");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_play(const uint8_t *data, size_t len)
{
    if (!s_play_dev || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 首次播放按固定采样格式打开 codec（录音复用同一句柄/格式） */
    esp_err_t ret = voice_ensure_opened();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 写入 PCM：16bit 双声道，小端；esp_codec_dev_write 会内部切分写入 DMA */
    int wr = esp_codec_dev_write(s_play_dev, (void *)data, (int)len);
    if (wr != 0) {
        ESP_LOGE(TAG, "播放写入失败: %d", wr);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 确保 codec 设备已按播放格式打开（播放/录音共用），并执行 DAC 输出兜底 + 寄存器取证 */
static esp_err_t voice_ensure_opened(void)
{
    if (s_opened) {
        return ESP_OK;
    }
    if (esp_codec_dev_open(s_play_dev, (esp_codec_dev_sample_info_t *)&s_play_fs) != 0) {
        ESP_LOGE(TAG, "codec open 失败");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_play_dev, 60);   /* 播放音量 */
    /* 无视驱动默认，强制 DAC 输出配置兜底（解除静音 + 使能 LOUT/ROUT），再取证回读 */
    es8388_ensure_dac_output();
    es8388_reg_dump();
    s_opened = true;
    ESP_LOGI(TAG, "codec 已打开: %dHz/%dbit/%dch（已强制 DAC 输出）", PLAY_FS, PLAY_BITS, PLAY_CHANNELS);
    return ESP_OK;
}

esp_err_t voice_record_start(void)
{
    if (!s_play_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_record_task) {
        ESP_LOGW(TAG, "record: 已在录制中，忽略重复 start");
        return ESP_OK;
    }
    /* 复用播放格式（16kHz/16bit/双声道）打开 codec，保证 IN 通道已启用 */
    if (voice_ensure_opened() != ESP_OK) {
        ESP_LOGE(TAG, "record: codec 打开失败");
        return ESP_FAIL;
    }
    s_record_stop = false;
    if (xTaskCreate(voice_record_task, "voice_record", 4096, NULL, 3, &s_record_task) != pdPASS) {
        ESP_LOGE(TAG, "record: 创建后台任务失败");
        s_record_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_record_stop(void)
{
    if (!s_record_task) {
        /* 未在录制：直接返回成功（幂等） */
        return ESP_OK;
    }
    s_record_stop = true;
    for (int i = 0; i < 100 && s_record_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));   /* 等待后台任务自然退出（最长 2s） */
    }
    if (s_record_task) {
        ESP_LOGW(TAG, "record: 后台任务未及时退出");
    }
    s_record_stop = false;
    return ESP_OK;
}

voice_session_state_t voice_get_state(void)
{
    /* M5 阶段不维护会话生命周期，恒为空闲 */
    return VOICE_IDLE;
}