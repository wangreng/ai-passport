// main/wb_slots.c —— 见 wb_slots.h。
#include "wb_slots.h"

#include <string.h>

#include "esp_partition.h"
#include "wb_model.h"

#define SLOTS_SUBTYPE 0x40

static const esp_partition_t *s_part;

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t wb_slots_init(void) {
    if (s_part) return ESP_OK;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, SLOTS_SUBTYPE, "wbslots");
    if (!s_part) return ESP_ERR_NOT_FOUND;
    if (s_part->size < (size_t)WB_SLOT_SIZE * WB_SLOT_COUNT) {
        s_part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t wb_slots_write(uint8_t slot, const uint8_t *word, size_t word_len,
                         const uint8_t *chunk, size_t chunk_len) {
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (slot >= WB_SLOT_COUNT || word_len + chunk_len > WB_SLOT_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t base = (uint32_t)slot * WB_SLOT_SIZE;
    esp_err_t err = esp_partition_erase_range(s_part, base, WB_SLOT_SIZE);
    // 先写音频,最后写文件头:写到一半断电,槽仍是"空"的。
    if (err == ESP_OK && word_len) {
        err = esp_partition_write(s_part, base + WB_SLOT_HEADER, word, word_len);
    }
    if (err == ESP_OK && chunk_len) {
        err = esp_partition_write(s_part, base + WB_SLOT_HEADER + word_len, chunk, chunk_len);
    }
    if (err == ESP_OK) {
        uint8_t head[WB_SLOT_HEADER];
        memset(head, 0xFF, sizeof(head));
        memcpy(head, "WBS1", 4);
        put_u32(head + 4, (uint32_t)word_len);
        put_u32(head + 8, (uint32_t)chunk_len);
        err = esp_partition_write(s_part, base, head, sizeof(head));
    }
    return err;
}

esp_err_t wb_slots_locate(uint8_t slot, bool chunk, uint32_t *offset, uint32_t *len) {
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (slot >= WB_SLOT_COUNT) return ESP_ERR_NOT_FOUND;
    const uint32_t base = (uint32_t)slot * WB_SLOT_SIZE;
    uint8_t head[12];
    esp_err_t err = esp_partition_read(s_part, base, head, sizeof(head));
    if (err != ESP_OK) return err;
    if (memcmp(head, "WBS1", 4) != 0) return ESP_ERR_NOT_FOUND;
    uint32_t wl = get_u32(head + 4), cl = get_u32(head + 8);
    if ((uint64_t)wl + cl > WB_SLOT_PAYLOAD_MAX) return ESP_ERR_NOT_FOUND;
    *offset = base + WB_SLOT_HEADER + (chunk ? wl : 0);
    *len = chunk ? cl : wl;
    return *len ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t wb_slots_read(uint32_t offset, void *buf, size_t len) {
    if (!s_part) return ESP_ERR_INVALID_STATE;
    return esp_partition_read(s_part, offset, buf, len);
}
