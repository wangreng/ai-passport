// main/wb_sync.c —— 见 wb_sync.h。
#include "wb_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "wb_adpcm.h"
#include "wb_net.h"
#include "wb_parse.h"
#include "wb_slots.h"
#include "wb_store.h"

static const char *TAG = "wb_sync";

#define PACK_MAX       (20 * 1024)
#define CONNECT_MS     15000
#define TIME_MS        8000

static void report(wb_sync_progress_cb_t cb, const char *line, uint16_t done, uint16_t total) {
    if (cb) cb(line, done, total);
}

static esp_err_t fetch_pack(wb_deck_t *deck, uint16_t *added) {
    *added = 0;
    char *text = malloc(PACK_MAX);
    wb_parsed_word_t *parsed = malloc(sizeof(wb_parsed_word_t) * WB_MAX_CARDS);
    wb_word_t *list = malloc(sizeof(wb_word_t) * WB_MAX_CARDS);
    esp_err_t err = (text && parsed && list) ? ESP_OK : ESP_ERR_NO_MEM;

    size_t len = 0;
    if (err == ESP_OK) err = wb_net_get(WB_PACK_BASE_URL "pack.txt", (uint8_t *)text, PACK_MAX, &len);
    char version[WB_PACK_VERSION_MAX] = "";
    int n = 0;
    if (err == ESP_OK) {
        n = wb_parse_pack(text, len, parsed, WB_MAX_CARDS, version, sizeof(version));
        if (n < 0) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        char stored[WB_PACK_VERSION_MAX];
        wb_store_get_pack_version(stored, sizeof(stored));
        if (version[0] && strcmp(stored, version) == 0) {
            ESP_LOGI(TAG, "词包 %s 没变", version);
        } else {
            for (int i = 0; i < n; i++) {
                list[i] = (wb_word_t){ parsed[i].en, parsed[i].chunk, parsed[i].zh, parsed[i].slug };
            }
            *added = wb_deck_merge(deck, list, (size_t)n, NULL, NULL);
            err = wb_store_save_all(deck);
            if (err == ESP_OK) err = wb_store_set_pack_version(version);
            ESP_LOGI(TAG, "词包 %s: %d 个词,新加 %u", version, n, (unsigned)*added);
        }
    }
    free(text);
    free(parsed);
    free(list);
    return err;
}

static uint16_t count_missing(const wb_deck_t *deck) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < deck->meta.count; i++) {
        const wb_card_t *c = &deck->cards[i];
        if (c->slug[0] && c->slot == WB_SLOT_NONE) n++;
    }
    return n;
}

// 下载一段音频到 buf,并检查是不是合法的 ADPCM 文件。
static esp_err_t fetch_clip(const char *slug, const char *suffix, uint8_t *buf, size_t cap,
                            size_t *len) {
    char url[sizeof(WB_PACK_BASE_URL) + WB_SLUG_MAX + 12];
    snprintf(url, sizeof(url), WB_PACK_BASE_URL "a/%s%s", slug, suffix);
    esp_err_t err = wb_net_get(url, buf, cap, len);
    if (err == ESP_OK && wb_adpcm_parse_header(buf, *len, NULL) == 0) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}

static void fetch_audio(wb_deck_t *deck, wb_sync_progress_cb_t cb, volatile bool *cancel,
                        wb_sync_result_t *out) {
    const uint16_t total = count_missing(deck);
    if (total == 0 || wb_slots_init() != ESP_OK) return;
    uint8_t *buf = malloc(WB_SLOT_PAYLOAD_MAX);
    if (!buf) return;

    char line[48];
    uint16_t tried = 0;
    for (uint16_t i = 0; i < deck->meta.count; i++) {
        wb_card_t *c = &deck->cards[i];
        if (!c->slug[0] || c->slot != WB_SLOT_NONE) continue;
        if (cancel && *cancel) {
            out->cancelled = true;
            break;
        }
        snprintf(line, sizeof(line), "下载发音 %u / %u", (unsigned)(tried + 1), (unsigned)total);
        report(cb, line, tried, total);
        tried++;

        uint8_t slot = wb_deck_free_slot(deck);
        if (slot == WB_SLOT_NONE) break;
        size_t wlen = 0, clen = 0;
        esp_err_t err = fetch_clip(c->slug, ".w.adp", buf, WB_SLOT_PAYLOAD_MAX, &wlen);
        if (err == ESP_OK) {
            err = fetch_clip(c->slug, ".c.adp", buf + wlen, WB_SLOT_PAYLOAD_MAX - wlen, &clen);
        }
        if (err == ESP_OK) err = wb_slots_write(slot, buf, wlen, buf + wlen, clen);
        if (err == ESP_OK) {
            c->slot = slot;
            wb_store_save_card(deck, i);
            out->audio_done++;
        } else {
            ESP_LOGW(TAG, "%s 发音失败: %s", c->en, esp_err_to_name(err));
        }
    }
    free(buf);
}

void wb_sync_run(wb_deck_t *deck, wb_sync_progress_cb_t progress, volatile bool *cancel,
                 wb_sync_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->pack = ESP_ERR_INVALID_STATE;

    report(progress, "连家里 Wi-Fi...", 0, 0);
    out->wifi = wb_net_connect(CONNECT_MS);
    if (out->wifi != ESP_OK) return;

    report(progress, "对时间...", 0, 0);
    uint32_t date = 0;
    out->time_ok = wb_net_sync_time(TIME_MS, &date, NULL);
    if (out->time_ok) {
        uint16_t day = deck->meta.day;
        if (wb_deck_set_date(deck, date)) wb_store_save_meta(deck);
        out->new_day = deck->meta.day != day;
    }

    if (!(cancel && *cancel)) {
        report(progress, "下载词表...", 0, 0);
        out->pack = fetch_pack(deck, &out->added);
        if (out->pack != ESP_OK) ESP_LOGW(TAG, "词表失败: %s", esp_err_to_name(out->pack));
    }
    if (!(cancel && *cancel)) fetch_audio(deck, progress, cancel, out);
    if (cancel && *cancel) out->cancelled = true;
    out->audio_missing = count_missing(deck);

    wb_net_disconnect();
}
