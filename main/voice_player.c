#include "voice_player.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "voice.h"

static const char *TAG = "VOICE_PLAYER";

/* 播放格式：16kHz/16bit/双声道 = 64000 字节/秒 */
#define BYTES_PER_SEC   (16000 * 2 * 2)
/* 分块 ≤32000 字节，且为 4 的倍数保持 16-bit 双声道采样对齐 */
#define CHUNK           (32000)

static TaskHandle_t    s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;  /* 保护忙标志 + 请求槽 */

/* 当前请求槽 + 忙标志（忙时丢弃策略，无需队列） */
static bool            s_busy = false;
static const uint8_t  *s_data = NULL;
static size_t          s_len  = 0;
static voice_src_t     s_src  = VOICE_SRC_OTHER;

static const char *src_name(voice_src_t src)
{
    switch (src) {
        case VOICE_SRC_ALARM:  return "报警";
        case VOICE_SRC_SLEEPY: return "犯困";
        default:               return "其他";
    }
}

/* 后台播放任务：独占 voice_play，串行播放 */
static void voice_player_task(void *arg)
{
    while (1) {
        /* 阻塞等待播放请求（每次通知后计数清零） */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const uint8_t *data = s_data;
        size_t       len  = s_len;
        voice_src_t  src  = s_src;

        ESP_LOGI(TAG, "开始播放(%s, %u 字节, 约 %u ms)",
                 src_name(src), (unsigned)len, (unsigned)(len * 1000 / BYTES_PER_SEC));

        size_t off = 0;
        bool   ok = true;
        while (off < len) {
            size_t n = len - off;
            if (n > CHUNK) {
                n = CHUNK;
            }
            esp_err_t ret = voice_play(data + off, n);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "播放失败(%s, 偏移 %u): %s",
                         src_name(src), (unsigned)off, esp_err_to_name(ret));
                ok = false;
                break;
            }
            off += n;
        }

        if (ok) {
            /* voice_play 返回仅代表数据已入 DMA，不代表播完；估算排空并加余量 */
            uint32_t play_ms = (uint32_t)(len * 1000 / BYTES_PER_SEC) + 300;
            ESP_LOGI(TAG, "数据已写完(%s)，估算排空等待约 %u ms",
                     src_name(src), (unsigned)play_ms);
            vTaskDelay(pdMS_TO_TICKS(play_ms));
        }

        /* 回空闲 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t voice_player_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;   /* 已初始化 */
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(voice_player_task, "voice_player", 4096, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_player_play(const uint8_t *data, size_t len, voice_src_t src)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len % 4 != 0) {
        ESP_LOGE(TAG, "PCM 长度 %u 非 4 的倍数，采样未对齐", (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_busy) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "播放忙，丢弃请求(%s)", src_name(src));
        return ESP_ERR_INVALID_STATE;
    }
    s_data = data;
    s_len  = len;
    s_src  = src;
    s_busy = true;
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool voice_player_is_busy(void)
{
    bool busy = false;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        busy = s_busy;
        xSemaphoreGive(s_lock);
    }
    return busy;
}