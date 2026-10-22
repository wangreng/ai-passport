// main/wb_adpcm.c —— 见 wb_adpcm.h。
#include "wb_adpcm.h"

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t INDEX_TABLE[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

uint32_t wb_adpcm_parse_header(const uint8_t *data, size_t len, uint32_t *rate_out) {
    if (!data || len < WB_ADPCM_HEADER) return 0;
    if (data[0] != 'W' || data[1] != 'A' || data[2] != 1) return 0;
    uint32_t rate = data[3] ? (uint32_t)data[3] * 1000u : 16000u;
    if (rate < 8000 || rate > WB_ADPCM_OUT_RATE) return 0;
    if (rate_out) *rate_out = rate;
    uint32_t n = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                 ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    if ((size_t)(n + 1) / 2 > len - WB_ADPCM_HEADER) return 0;
    return n;
}

void wb_adpcm_init(wb_adpcm_state_t *st) {
    st->pred = 0;
    st->index = 0;
}

static int16_t step_one(wb_adpcm_state_t *st, uint8_t code) {
    int32_t step = STEP_TABLE[st->index];
    int32_t delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    st->pred += (code & 8) ? -delta : delta;
    if (st->pred > 32767) st->pred = 32767;
    if (st->pred < -32768) st->pred = -32768;
    int idx = st->index + INDEX_TABLE[code & 7];
    st->index = (int8_t)(idx < 0 ? 0 : (idx > 88 ? 88 : idx));
    return (int16_t)st->pred;
}

void wb_adpcm_decode(wb_adpcm_state_t *st, const uint8_t *codes, size_t n_bytes, int16_t *out) {
    for (size_t i = 0; i < n_bytes; i++) {
        *out++ = step_one(st, codes[i] & 0x0F);
        *out++ = step_one(st, codes[i] >> 4);
    }
}

void wb_resample_init(wb_resample_t *rs, uint32_t in_rate) {
    rs->in_rate = in_rate;
    rs->phase = 0;
    rs->prev = 0;
}

size_t wb_resample(wb_resample_t *rs, const int16_t *in, size_t n_in, int16_t *out) {
    size_t n_out = 0;
    if (rs->in_rate == WB_ADPCM_OUT_RATE) {
        for (size_t i = 0; i < n_in; i++) out[i] = in[i];
        return n_in;
    }
    // 输出点落在 prev 与 in[i] 之间:phase/OUT_RATE 是离 prev 的距离(以输入采样为单位)。
    for (size_t i = 0; i < n_in; i++) {
        int32_t a = rs->prev, b = in[i];
        while (rs->phase < WB_ADPCM_OUT_RATE) {
            out[n_out++] = (int16_t)(a + (b - a) * (int32_t)rs->phase / (int32_t)WB_ADPCM_OUT_RATE);
            rs->phase += rs->in_rate;
        }
        rs->phase -= WB_ADPCM_OUT_RATE;
        rs->prev = in[i];
    }
    return n_out;
}
