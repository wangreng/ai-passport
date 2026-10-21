// main/wb_store.c —— 见 wb_store.h。
#include "wb_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wb_store";

#define PART "wbdata"
#define NS   "wordbox"
#define FORMAT_VERSION 1u

static esp_err_t open_rw(nvs_handle_t *h) {
    return nvs_open_from_partition(PART, NS, NVS_READWRITE, h);
}

static void card_key(char key[8], uint16_t idx) {
    snprintf(key, 8, "c%03u", (unsigned)idx);
}

esp_err_t wb_store_init(void) {
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "wbdata 分区需要重建: %s", esp_err_to_name(err));
        err = nvs_flash_erase_partition(PART);
        if (err == ESP_OK) err = nvs_flash_init_partition(PART);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wbdata 初始化失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wb_store_load(wb_deck_t *deck) {
    wb_deck_init(deck);
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   // 全新设备
    if (err != ESP_OK) return err;

    uint32_t ver = 0;
    if (nvs_get_u32(h, "ver", &ver) != ESP_OK || ver != FORMAT_VERSION) {
        nvs_close(h);
        ESP_LOGW(TAG, "存档版本 %u 不认识,当作新设备", (unsigned)ver);
        return ESP_OK;
    }

    wb_meta_t meta;
    size_t len = sizeof(meta);
    err = nvs_get_blob(h, "meta", &meta, &len);
    if (err != ESP_OK || len != sizeof(meta) || meta.count > WB_MAX_CARDS) {
        nvs_close(h);
        ESP_LOGW(TAG, "进度读取失败(%s),当作新设备", esp_err_to_name(err));
        return ESP_OK;
    }
    deck->meta = meta;

    uint16_t ok = 0;
    for (uint16_t i = 0; i < meta.count; i++) {
        char key[8];
        card_key(key, i);
        len = sizeof(wb_card_t);
        wb_card_t card;
        if (nvs_get_blob(h, key, &card, &len) == ESP_OK && len == sizeof(card)) {
            // 防御:字符串必须以 NUL 结尾。
            card.en[WB_EN_MAX - 1] = '\0';
            card.chunk[WB_CHUNK_MAX - 1] = '\0';
            card.zh[WB_ZH_MAX - 1] = '\0';
            if (card.box > WB_BOX_DONE) card.box = WB_BOX_1;
            deck->cards[ok++] = card;
        }
    }
    if (ok != meta.count) {
        ESP_LOGW(TAG, "卡片缺失 %u 张", (unsigned)(meta.count - ok));
        deck->meta.count = ok;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "读入 %u 张卡,第 %u 天", (unsigned)deck->meta.count, (unsigned)deck->meta.day);
    return ESP_OK;
}

static esp_err_t write_meta(nvs_handle_t h, const wb_deck_t *deck) {
    esp_err_t err = nvs_set_u32(h, "ver", FORMAT_VERSION);
    if (err == ESP_OK) err = nvs_set_blob(h, "meta", &deck->meta, sizeof(deck->meta));
    return err;
}

esp_err_t wb_store_save_meta(const wb_deck_t *deck) {
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) return err;
    err = write_meta(h, deck);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存进度失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wb_store_save_card(const wb_deck_t *deck, uint16_t idx) {
    if (idx >= deck->meta.count) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) return err;
    char key[8];
    card_key(key, idx);
    err = nvs_set_blob(h, key, &deck->cards[idx], sizeof(wb_card_t));
    if (err == ESP_OK) err = write_meta(h, deck);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存卡片 %u 失败: %s", (unsigned)idx, esp_err_to_name(err));
    return err;
}

esp_err_t wb_store_save_all(const wb_deck_t *deck) {
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) return err;
    char key[8];
    for (uint16_t i = 0; i < deck->meta.count && err == ESP_OK; i++) {
        card_key(key, i);
        err = nvs_set_blob(h, key, &deck->cards[i], sizeof(wb_card_t));
    }
    for (uint16_t i = deck->meta.count; i < WB_MAX_CARDS && err == ESP_OK; i++) {
        card_key(key, i);
        esp_err_t e = nvs_erase_key(h, key);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = write_meta(h, deck);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "整副保存失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wb_store_get_wifi(char ssid[WB_SSID_MAX], char pass[WB_PASS_MAX]) {
    ssid[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = WB_SSID_MAX;
    err = nvs_get_str(h, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = WB_PASS_MAX;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) pass[0] = '\0';
    } else {
        ssid[0] = '\0';
    }
    nvs_close(h);
    return err;
}

esp_err_t wb_store_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
