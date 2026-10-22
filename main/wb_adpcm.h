// main/wb_adpcm.h —— 词包发音的 IMA ADPCM 解码(纯逻辑,host 测试覆盖)。
//
// 文件格式(tools/build_wordpack.py,wordpack 分支):
//   "WA"  版本 1  采样率 kHz(0 表示 16)  uint32 LE 采样数  然后 4 bit 码,先低半字节
// 单声道,预测值与步长下标都从 0 开始。词包目前用 12 kHz,播放时升到 16 kHz。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WB_ADPCM_HEADER   8
#define WB_ADPCM_OUT_RATE 16000   // 播放统一用的采样率

typedef struct {
    int32_t pred;
    int8_t index;
} wb_adpcm_state_t;

// 校验文件头,返回采样数;格式不对返回 0。len 为文件总字节数。rate_out 可为 NULL。
uint32_t wb_adpcm_parse_header(const uint8_t *data, size_t len, uint32_t *rate_out);

void wb_adpcm_init(wb_adpcm_state_t *st);

// 把 n_bytes 字节码流解成 2*n_bytes 个采样写入 out。可分段连续调用。
void wb_adpcm_decode(wb_adpcm_state_t *st, const uint8_t *codes, size_t n_bytes, int16_t *out);

// 线性插值把 in_rate 的采样流升到 WB_ADPCM_OUT_RATE。可分段连续调用。
typedef struct {
    uint32_t in_rate;
    uint32_t phase;      // 以 WB_ADPCM_OUT_RATE 为分母的小数位置
    int16_t prev;
} wb_resample_t;

void wb_resample_init(wb_resample_t *rs, uint32_t in_rate);

// 返回写入 out 的采样数;out 至少要 n_in * OUT_RATE / in_rate + 2 个位置。
size_t wb_resample(wb_resample_t *rs, const int16_t *in, size_t n_in, int16_t *out);
