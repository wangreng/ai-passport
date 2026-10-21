// main/wb_ui.c —— 见 wb_ui.h。
//
// 版式按 240x320 竖屏,四角有 30px 圆角遮罩,内容离边至少 12px。
#include "wb_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

LV_FONT_DECLARE(wb_font_cjk_14);
LV_FONT_DECLARE(wb_font_cjk_20);

#define SCR_W 240
#define SCR_H 320

#define C_PAPER   0xFBF6EC
#define C_INK     0x3A2E22
#define C_MUTED   0x8A7A64
#define C_CARD    0xFFFFFF
#define C_LINE    0xE6D9C2
#define C_BOX1    0xE85D4A
#define C_BOX2    0xF2A33A
#define C_BOX3    0x4CAF7A
#define C_DONE    0xC9A227
#define C_POOL    0x9A8F80
#define C_BOSS_BG 0x2B2340
#define C_BOSS_FG 0xF4EEFF
#define C_BOSS_HI 0xFF6B6B

// 带 Montserrat 兜底(箭头等 LV_SYMBOL 图标)的可写字体描述符。
static lv_font_t s_cjk14;
static lv_font_t s_cjk20;

static lv_obj_t *s_scr;
static lv_obj_t *s_battery;
static lv_obj_t *s_card_panel;
static lv_obj_t *s_portal_upload;
static lv_obj_t *s_portal_home;
static int s_last_soc = -1;

void wb_ui_init(void) {
    s_cjk14 = wb_font_cjk_14;
    s_cjk14.fallback = &lv_font_montserrat_14;
    s_cjk20 = wb_font_cjk_20;
    s_cjk20.fallback = &lv_font_montserrat_20;
}

static uint32_t box_color(uint8_t box) {
    switch (box) {
    case WB_BOX_1: return C_BOX1;
    case WB_BOX_2: return C_BOX2;
    case WB_BOX_3: return C_BOX3;
    case WB_BOX_DONE: return C_DONE;
    default: return C_POOL;
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// 换页:新建屏幕再删旧的,避免旧对象指针残留。
static lv_obj_t *new_screen(uint32_t bg) {
    lv_obj_t *old = s_scr;
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    s_card_panel = NULL;
    s_portal_upload = NULL;
    s_portal_home = NULL;

    s_battery = label(s_scr, &lv_font_montserrat_14, bg == C_BOSS_BG ? C_BOSS_FG : C_MUTED, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -34, 14);

    lv_screen_load(s_scr);
    if (old) lv_obj_delete(old);
    wb_ui_set_battery(s_last_soc);
    return s_scr;
}

void wb_ui_set_battery(int soc) {
    s_last_soc = soc;
    if (!s_battery) return;
    if (soc < 0) {
        lv_label_set_text(s_battery, "");
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
    }
}

// 底部按键提示条。
static void hint_bar(lv_obj_t *scr, const char *text, bool dark) {
    lv_obj_t *bar = panel(scr, 24, 272, 192, 30, dark ? 0x3D3358 : C_LINE, 15);
    lv_obj_t *l = label(bar, &s_cjk14, dark ? C_BOSS_FG : C_INK, text);
    lv_obj_center(l);
}

// ---------------------------------------------------------------------------
// 首页
// ---------------------------------------------------------------------------

static void box_tile(lv_obj_t *scr, int x, const char *head, uint16_t count,
                     const char *caption, uint32_t color) {
    lv_obj_t *tile = panel(scr, x, 64, 48, 78, C_CARD, 10);
    lv_obj_set_style_border_color(tile, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_t *top = panel(tile, 0, 0, 48, 24, color, 0);
    lv_obj_t *h = label(top, &lv_font_montserrat_14, 0xFFFFFF, head);
    lv_obj_center(h);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)count);
    lv_obj_t *n = label(tile, &lv_font_montserrat_20, C_INK, buf);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_t *c = label(tile, &s_cjk14, C_MUTED, caption);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void wb_ui_show_home(const wb_home_view_t *v) {
    lv_obj_t *scr = new_screen(C_PAPER);

    lv_obj_t *title = label(scr, &s_cjk20, C_INK, "单词盒子");
    lv_obj_set_pos(title, 24, 12);
    char buf[96];
    snprintf(buf, sizeof(buf), "第 %u 天", (unsigned)v->day);
    lv_obj_t *day = label(scr, &s_cjk14, C_MUTED, buf);
    lv_obj_set_pos(day, 24, 38);

    box_tile(scr, 12, "1", v->counts.box1, "每天", C_BOX1);
    box_tile(scr, 66, "2", v->counts.box2, "隔天", C_BOX2);
    box_tile(scr, 120, "3", v->counts.box3, "Boss", C_BOX3);
    box_tile(scr, 174, LV_SYMBOL_OK, v->counts.done, "毕业", C_DONE);

    lv_obj_t *today = panel(scr, 12, 152, 216, 66, C_CARD, 12);
    lv_obj_t *t1;
    lv_obj_t *t2 = NULL;
    if (v->total == 0) {
        t1 = label(today, &s_cjk20, C_INK, "盒子还是空的");
        t2 = label(today, &s_cjk14, C_MUTED, "长按 OK 进入换词模式");
    } else if (v->today_done) {
        t1 = label(today, &s_cjk20, C_BOX3, "今天完成啦 " LV_SYMBOL_OK);
        t2 = label(today, &s_cjk14, C_MUTED, "明天开机再来");
    } else if (v->review + v->fresh == 0) {
        t1 = label(today, &s_cjk20, C_INK, "今天没有要复习的");
        t2 = label(today, &s_cjk14, C_MUTED, v->counts.unsorted ? "先按 下 分拣新词" : "休息一天");
    } else {
        t1 = label(today, &s_cjk20, C_INK, "今天的任务");
        snprintf(buf, sizeof(buf), "复习 %u 张  新词 %u 张",
                 (unsigned)v->review, (unsigned)v->fresh);
        t2 = label(today, &s_cjk14, C_MUTED, buf);
    }
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 8);
    if (t2) lv_obj_align(t2, LV_ALIGN_BOTTOM_MID, 0, -8);

    snprintf(buf, sizeof(buf), "积分 %u   徽章 %u   收集 %u",
             (unsigned)v->points, (unsigned)v->badges, (unsigned)v->collected);
    lv_obj_t *stats = label(scr, &s_cjk14, C_INK, buf);
    lv_obj_align(stats, LV_ALIGN_TOP_MID, 0, 228);

    if (v->counts.unsorted) {
        snprintf(buf, sizeof(buf), "待分拣 %u 张", (unsigned)v->counts.unsorted);
        lv_obj_t *u = label(scr, &s_cjk14, C_BOX1, buf);
        lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 248);
    }

    hint_bar(scr, "OK 开始  " LV_SYMBOL_UP " Boss  " LV_SYMBOL_DOWN " 分拣", false);
}

// ---------------------------------------------------------------------------
// 卡片
// ---------------------------------------------------------------------------

static const lv_font_t *word_font(const char *en) {
    size_t n = strlen(en);
    if (n <= 8) return &lv_font_montserrat_36;
    if (n <= 12) return &lv_font_montserrat_28;
    return &lv_font_montserrat_20;
}

void wb_ui_show_card(const wb_card_view_t *v) {
    const bool boss = v->kind == WB_SESSION_BOSS;
    const uint32_t fg = boss ? C_BOSS_FG : C_INK;
    lv_obj_t *scr = new_screen(boss ? C_BOSS_BG : C_PAPER);

    char buf[40];
    snprintf(buf, sizeof(buf), "%u / %u", (unsigned)v->pos, (unsigned)v->total);
    lv_obj_t *prog = label(scr, &lv_font_montserrat_14, boss ? C_BOSS_FG : C_MUTED, buf);
    lv_obj_set_pos(prog, 34, 14);

    const char *tag;
    uint32_t tag_color;
    if (v->kind == WB_SESSION_SORT) {
        tag = "分拣";
        tag_color = C_POOL;
    } else if (boss) {
        tag = "BOSS";
        tag_color = C_BOSS_HI;
    } else if (v->retry) {
        tag = "再看一遍";
        tag_color = C_BOX1;
    } else {
        static const char *names[] = { "新词", "新词", "1号盒", "2号盒", "3号盒", "毕业" };
        tag = names[v->card->box <= WB_BOX_DONE ? v->card->box : 0];
        tag_color = box_color(v->card->box);
    }
    lv_obj_t *pill = panel(scr, 92, 10, 72, 24, tag_color, 12);
    lv_obj_t *pl = label(pill, &s_cjk14, 0xFFFFFF, tag);
    lv_obj_center(pl);

    const uint32_t border = boss ? C_BOSS_HI : (v->kind == WB_SESSION_SORT ? C_POOL
                                                : box_color(v->card->box));
    s_card_panel = panel(scr, 14, 44, 212, 218, C_CARD, 18);
    lv_obj_set_style_border_color(s_card_panel, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(s_card_panel, 3, 0);

    lv_obj_t *en = label(s_card_panel, word_font(v->card->en), C_INK, v->card->en);
    lv_obj_set_width(en, 196);
    lv_label_set_long_mode(en, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(en, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(en, LV_ALIGN_TOP_MID, 0, 26);

    if (v->card->chunk[0]) {
        lv_obj_t *ch = label(s_card_panel, &lv_font_montserrat_20, C_MUTED, v->card->chunk);
        lv_obj_set_width(ch, 196);
        lv_label_set_long_mode(ch, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(ch, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ch, LV_ALIGN_TOP_MID, 0, 86);
    }

    lv_obj_t *line = panel(s_card_panel, 24, 142, 164, 2, C_LINE, 1);
    (void)line;
    if (v->flipped) {
        lv_obj_t *zh = label(s_card_panel, &s_cjk20, C_INK,
                             v->card->zh[0] ? v->card->zh : "(没有填中文)");
        lv_obj_set_width(zh, 196);
        lv_label_set_long_mode(zh, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(zh, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(zh, LV_ALIGN_TOP_MID, 0, 158);
    } else {
        lv_obj_t *q = label(s_card_panel, &s_cjk14, C_MUTED, "先大声说出意思");
        lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 164);
    }
    (void)fg;

    const char *hint;
    if (v->kind == WB_SESSION_SORT) {
        hint = v->flipped ? LV_SYMBOL_UP " 认识   " LV_SYMBOL_DOWN " 不认识"
                          : LV_SYMBOL_UP " 认识  " LV_SYMBOL_DOWN " 不认识  OK 看";
    } else {
        hint = v->flipped ? LV_SYMBOL_UP " 会了     " LV_SYMBOL_DOWN " 不会"
                          : "说完按 OK 翻面";
    }
    hint_bar(scr, hint, boss);
}

static void anim_y(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void wb_ui_card_feedback(bool known) {
    if (!s_card_panel) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card_panel);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, 44, known ? -180 : 340);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, known ? lv_anim_path_ease_in : lv_anim_path_ease_in);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_card_panel);
    lv_anim_set_exec_cb(&b, anim_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&b, 220);
    lv_anim_start(&b);
}

// ---------------------------------------------------------------------------
// 结算
// ---------------------------------------------------------------------------

void wb_ui_show_result(const char *title, const char *line1, const char *line2,
                       uint16_t gained, bool celebrate) {
    lv_obj_t *scr = new_screen(C_PAPER);

    lv_obj_t *card = panel(scr, 14, 44, 212, 210, C_CARD, 18);
    lv_obj_set_style_border_color(card, lv_color_hex(celebrate ? C_DONE : C_LINE), 0);
    lv_obj_set_style_border_width(card, 3, 0);

    lv_obj_t *t = label(card, &s_cjk20, C_INK, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);

    if (gained) {
        char buf[16];
        snprintf(buf, sizeof(buf), "+%u", (unsigned)gained);
        lv_obj_t *g = label(card, &lv_font_montserrat_36, celebrate ? C_DONE : C_BOX3, buf);
        lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 56);
    } else if (celebrate) {
        lv_obj_t *g = label(card, &lv_font_montserrat_36, C_DONE, LV_SYMBOL_OK);
        lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 56);
    }
    if (line1) {
        lv_obj_t *l1 = label(card, &s_cjk14, C_INK, line1);
        lv_obj_set_width(l1, 190);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 116);
    }
    if (line2) {
        lv_obj_t *l2 = label(card, &s_cjk14, C_MUTED, line2);
        lv_obj_set_width(l2, 190);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 152);
    }
    hint_bar(scr, "OK 回首页", false);
}

// ---------------------------------------------------------------------------
// 换词模式
// ---------------------------------------------------------------------------

void wb_ui_show_portal(const char *ap_ssid, const char *ap_pass, const char *ip) {
    lv_obj_t *scr = new_screen(C_PAPER);

    lv_obj_t *title = label(scr, &s_cjk20, C_INK, "换词模式");
    lv_obj_set_pos(title, 24, 12);

    lv_obj_t *step1 = panel(scr, 12, 46, 216, 84, C_CARD, 12);
    lv_obj_t *s1 = label(step1, &s_cjk14, C_MUTED, "1  手机连这个 Wi-Fi");
    lv_obj_set_pos(s1, 12, 8);
    lv_obj_t *ssid = label(step1, &lv_font_montserrat_20, C_INK, ap_ssid);
    lv_obj_set_pos(ssid, 12, 30);
    char buf[48];
    snprintf(buf, sizeof(buf), "密码 %s", ap_pass);
    lv_obj_t *pw = label(step1, &s_cjk14, C_INK, buf);
    lv_obj_set_pos(pw, 12, 58);

    lv_obj_t *step2 = panel(scr, 12, 138, 216, 62, C_CARD, 12);
    lv_obj_t *s2 = label(step2, &s_cjk14, C_MUTED, "2  浏览器打开");
    lv_obj_set_pos(s2, 12, 8);
    lv_obj_t *addr = label(step2, &lv_font_montserrat_20, C_BOX1, ip);
    lv_obj_set_pos(addr, 12, 30);

    s_portal_upload = label(scr, &s_cjk14, C_INK, "等手机连上来...");
    lv_obj_set_width(s_portal_upload, 200);
    lv_label_set_long_mode(s_portal_upload, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_portal_upload, 20, 208);
    s_portal_home = label(scr, &s_cjk14, C_MUTED, "");
    lv_obj_set_width(s_portal_home, 200);
    lv_label_set_long_mode(s_portal_home, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_portal_home, 20, 248);

    hint_bar(scr, "弄完长按 OK 退出", false);
}

void wb_ui_portal_status(const char *upload_line, const char *home_line) {
    if (upload_line && s_portal_upload) lv_label_set_text(s_portal_upload, upload_line);
    if (home_line && s_portal_home) lv_label_set_text(s_portal_home, home_line);
}
