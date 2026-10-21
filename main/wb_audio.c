// main/wb_audio.c —— 见 wb_audio.h。
//
// 每次从 flash 读 256 字节 ADPCM(512 个采样),解码、升到 16 kHz 后送 codec。
// 每块之间看一眼队列,有新请求就放弃当前这段。
#include "wb_audio.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wb_adpcm.h"
#include "wb_model.h"
#include "wb_slots.h"

static const char *TAG = "wb_audio";

#define CODE_BLOCK  256
#define VOLUME      75
#define REQ_STOP    0xFFFF

static QueueHandle_t s_queue;
static bool s_ready;

static uint8_t s_codes[CODE_BLOCK];
static int16_t s_pcm[CODE_BLOCK * 2];
static int16_t s_out[CODE_BLOCK * 2 * WB_ADPCM_OUT_RATE / 8000 + 4];

static void play_clip(uint8_t slot, bool chunk) {
    uint32_t off, len;
    if (wb_slots_locate(slot, chunk, &off, &len) != ESP_OK) return;
    uint8_t head[WB_ADPCM_HEADER];
    if (len < sizeof(head) || wb_slots_read(off, head, sizeof(head)) != ESP_OK) return;
    uint32_t rate = 0;
    uint32_t samples = wb_adpcm_parse_header(head, len, &rate);
    if (samples == 0) return;

    wb_adpcm_state_t st;
    wb_resample_t rs;
    wb_adpcm_init(&st);
    wb_resample_init(&rs, rate);
    uint32_t pos = off + WB_ADPCM_HEADER;
    uint32_t left = (samples + 1) / 2;
    while (left > 0) {
        if (uxQueueMessagesWaiting(s_queue) > 0) break;   // 被新请求打断
        size_t n = left < CODE_BLOCK ? left : CODE_BLOCK;
        if (wb_slots_read(pos, s_codes, n) != ESP_OK) break;
        wb_adpcm_decode(&st, s_codes, n, s_pcm);
        size_t out = wb_resample(&rs, s_pcm, n * 2, s_out);
        if (bsp_audio_write(s_out, out * sizeof(int16_t)) != ESP_OK) break;
        pos += n;
        left -= n;
    }
    // 补一小段静音,把 I2S DMA 里最后的采样推出去,免得尾音被截掉或下次开头带杂音。
    for (size_t i = 0; i < 256; i++) s_out[i] = 0;
    (void)bsp_audio_write(s_out, 256 * sizeof(int16_t));
}

static void audio_task(void *arg) {
    (void)arg;
    uint16_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        if (req == REQ_STOP) continue;
        play_clip((uint8_t)(req & 0xFF), (req >> 8) != 0);
    }
}

esp_err_t wb_audio_start(void) {
    if (s_ready) return ESP_OK;
    esp_err_t err = wb_slots_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "没有发音分区: %s", esp_err_to_name(err));
        return err;
    }
    err = bsp_audio_init();
    if (err == ESP_OK) err = bsp_audio_set_format(WB_ADPCM_OUT_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(VOLUME);
    s_queue = xQueueCreate(2, sizeof(uint16_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_task, "wb_audio", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    return ESP_OK;
}

void wb_audio_play(uint8_t slot, bool chunk) {
    if (!s_ready || slot == WB_SLOT_NONE) return;
    uint16_t req = (uint16_t)(slot | (chunk ? 0x100 : 0));
    // 只留最新的请求。
    xQueueReset(s_queue);
    (void)xQueueSend(s_queue, &req, 0);
}

void wb_audio_stop(void) {
    if (!s_ready) return;
    uint16_t req = REQ_STOP;
    xQueueReset(s_queue);
    (void)xQueueSend(s_queue, &req, 0);
}
