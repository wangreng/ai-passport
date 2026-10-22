// main/wb_slots.h —— 发音槽:flash 分区 "wbslots" 切成 WB_SLOT_COUNT 个 28 KB 槽,
// 每张有发音的卡占一个槽(卡片里记槽号),放单词和词块两段 ADPCM。
//
// 槽布局:
//   0   'W' 'B' 'S' '1'
//   4   uint32 LE 单词音频长度(字节,含 ADPCM 文件头)
//   8   uint32 LE 词块音频长度
//   12  保留到 32 字节(以后放像素卡图的位置)
//   32  单词音频,紧跟词块音频
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WB_SLOT_SIZE        (28 * 1024)
#define WB_SLOT_HEADER      32
#define WB_SLOT_PAYLOAD_MAX (WB_SLOT_SIZE - WB_SLOT_HEADER)

esp_err_t wb_slots_init(void);

// 擦掉槽并写入两段音频。word_len + chunk_len 不得超过 WB_SLOT_PAYLOAD_MAX。
// word 与 chunk 可以是同一块缓冲里相邻的两段。
esp_err_t wb_slots_write(uint8_t slot, const uint8_t *word, size_t word_len,
                         const uint8_t *chunk, size_t chunk_len);

// 找到槽里某段音频在分区内的位置。槽空或损坏返回 ESP_ERR_NOT_FOUND。
esp_err_t wb_slots_locate(uint8_t slot, bool chunk, uint32_t *offset, uint32_t *len);

// 从分区读原始字节。
esp_err_t wb_slots_read(uint32_t offset, void *buf, size_t len);
