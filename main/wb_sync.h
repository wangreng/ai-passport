// main/wb_sync.h —— 联网同步:连家里 Wi-Fi → 对时 → 下载词包清单并合并 → 给缺发音的卡下载音频。
//
// 词包由仓库的 wordpack 分支生成(tools/build_wordpack.py + GitHub Actions),
// 设备从 raw.githubusercontent.com 读。阻塞执行,只能在应用任务里调。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wb_model.h"

#define WB_PACK_BASE_URL \
    "https://raw.githubusercontent.com/wangreng/ai-passport/wordpack/packs/out/"

// 进度回调:line 给孩子 / 家长看的一行字;total 为 0 时不显示进度条。
typedef void (*wb_sync_progress_cb_t)(const char *line, uint16_t done, uint16_t total);

typedef struct {
    esp_err_t wifi;          // ESP_ERR_NOT_FOUND 表示没设置家里 Wi-Fi
    esp_err_t pack;          // 词表下载 / 解析结果
    bool time_ok;
    bool new_day;            // 对时后日期变了,翻到新的一天
    uint16_t added;          // 新加入的卡
    uint16_t audio_done;     // 本次下好的发音
    uint16_t audio_missing;  // 结束时仍没有发音的卡
    bool cancelled;
} wb_sync_result_t;

// deck 由调用方持有;函数内部按需保存卡片和进度。cancel 可在别的任务里置 true。
void wb_sync_run(wb_deck_t *deck, wb_sync_progress_cb_t progress, volatile bool *cancel,
                 wb_sync_result_t *out);
