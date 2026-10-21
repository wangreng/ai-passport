// main/wb_ui.h —— 单词盒子的界面。每次状态变化整页重建,简单可靠。
//
// 所有函数都必须在 LVGL 任务里调用,或在调用前持有 bsp_lvgl_lock()。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wb_model.h"

typedef struct {
    uint16_t day;
    bool today_done;
    uint16_t review;
    uint16_t fresh;
    wb_counts_t counts;
    uint16_t total;
    uint16_t points;
    uint16_t badges;
    uint16_t collected;
    char date[24];       // 对过时才有,比如 "10月21日 周三";空串表示没联网
    bool boss_day;       // 周六且 3 号盒有卡
} wb_home_view_t;

typedef struct {
    wb_session_kind_t kind;
    uint16_t pos;        // 从 1 开始
    uint16_t total;
    bool retry;          // 当轮末尾的重练
    bool flipped;
    bool copy_hint;      // 1 号盒的卡翻面后提示抄到白板上
    const wb_card_t *card;
} wb_card_view_t;

void wb_ui_init(void);

void wb_ui_show_home(const wb_home_view_t *v);
void wb_ui_show_card(const wb_card_view_t *v);
// 答题后的小动画:会了往上飞,不会往下掉。约 220 ms,调用方等它播完再换下一张。
void wb_ui_card_feedback(bool known);
#define WB_UI_FEEDBACK_MS 240

// 一轮结束。gained 为 0 时不显示加分。
void wb_ui_show_result(const char *title, const char *line1, const char *line2,
                       uint16_t gained, bool celebrate);

void wb_ui_show_portal(const char *ap_ssid, const char *ap_pass, const char *ip);
// 换词页的两行状态:手机/上传结果、家里 Wi-Fi。传 NULL 表示不变。
void wb_ui_portal_status(const char *upload_line, const char *home_line);

// 答错 2 号 / 3 号盒的卡:大字显示单词和词块,提示去白板写 3 遍,OK 继续。
void wb_ui_show_write(const wb_card_t *card, bool boss);

// 联网同步页。status 更新一行字和进度条(total 为 0 时隐藏进度条)。
void wb_ui_show_sync(void);
void wb_ui_sync_status(const char *line, uint16_t done, uint16_t total);

// 右上角电量;soc < 0 时隐藏。
void wb_ui_set_battery(int soc);
