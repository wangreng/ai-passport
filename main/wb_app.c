// main/wb_app.c —— 见 wb_app.h。
//
// 一个工作任务串行处理按键、换词模式事件和定时刷新;存储、Wi-Fi 同步都在这里做,
// 不进按键回调和 LVGL 任务。卡组只有在换词网页提交时会被 httpd 任务改写,
// 两边用 s_deck_lock 互斥。发音在 wb_audio 自己的任务里播。
#include "wb_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "wb_audio.h"
#include "wb_model.h"
#include "wb_net.h"
#include "wb_parse.h"
#include "wb_portal.h"
#include "wb_store.h"
#include "wb_sync.h"
#include "wb_ui.h"

static const char *TAG = "wb_app";

#define QUEUE_DEPTH     12
#define TICK_MS         5000
#define DIM_AFTER_US    (60LL * 1000 * 1000)
#define BRIGHT_PERCENT  100
#define DIM_PERCENT     8

typedef enum { EV_KEY = 0, EV_PORTAL, EV_BOOT_SYNC } ev_type_t;

typedef struct {
    ev_type_t type;
    bsp_btn_t btn;
    bsp_btn_ev_t key_ev;
    wb_portal_event_t portal_ev;
} app_event_t;

typedef enum {
    MODE_HOME = 0, MODE_CARD, MODE_WRITE, MODE_RESULT, MODE_PORTAL, MODE_SYNC,
} app_mode_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_deck_lock;
static wb_deck_t s_deck;
static wb_session_t s_session;
static volatile app_mode_t s_mode;
static volatile bool s_sync_cancel;
static int s_write_idx = -1;
static bool s_boot_pending;     // 开机翻天要等联网对时的结果
static bool s_flipped;
static uint16_t s_sort_known;
static bool s_dimmed;
static int64_t s_last_key_us;
static char s_upload_line[200];

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static bool ui_lock(void) {
    return bsp_lvgl_lock(1000);
}

static void deck_lock(void) {
    xSemaphoreTake(s_deck_lock, portMAX_DELAY);
}

static void deck_unlock(void) {
    xSemaphoreGive(s_deck_lock);
}

static void show_home(void) {
    wb_home_view_t v = {0};
    deck_lock();
    v.day = s_deck.meta.day;
    v.today_done = s_deck.meta.today_done;
    wb_today_due(&s_deck, &v.review, &v.fresh);
    v.counts = wb_deck_counts(&s_deck);
    v.total = s_deck.meta.count;
    v.points = s_deck.meta.points;
    v.badges = s_deck.meta.badges;
    v.collected = s_deck.meta.collected;
    deck_unlock();

    uint32_t date;
    uint8_t wday;
    if (wb_net_today(&date, &wday)) {
        static const char *const WEEK[] = { "日", "一", "二", "三", "四", "五", "六" };
        snprintf(v.date, sizeof(v.date), "%u月%u日 周%s", (unsigned)(date / 100 % 100),
                 (unsigned)(date % 100), WEEK[wday % 7]);
        v.boss_day = wday == 6 && v.counts.box3 > 0;
    }

    s_mode = MODE_HOME;
    if (ui_lock()) {
        wb_ui_show_home(&v);
        bsp_lvgl_unlock();
    }
}

static void show_current_card(void) {
    int idx = wb_session_current(&s_session);
    if (idx < 0) return;
    wb_card_view_t v = {
        .kind = s_session.kind,
        .pos = (uint16_t)(s_session.pos + 1),
        .total = s_session.len,
        .retry = s_session.pos >= s_session.first_pass,
        .flipped = s_flipped,
        .card = &s_deck.cards[idx],
    };
    v.copy_hint = v.kind == WB_SESSION_TODAY && !v.retry && v.card->box == WB_BOX_1;
    if (ui_lock()) {
        wb_ui_show_card(&v);
        bsp_lvgl_unlock();
    }
}

// 当前卡的发音:chunk=false 单词,true 词块。
static void play_current(bool chunk) {
    int idx = s_mode == MODE_WRITE ? s_write_idx : wb_session_current(&s_session);
    if (idx < 0) return;
    wb_audio_play(s_deck.cards[idx].slot, chunk);
}

static void show_result(const char *title, const char *l1, const char *l2,
                        uint16_t gained, bool celebrate) {
    s_mode = MODE_RESULT;
    if (ui_lock()) {
        wb_ui_show_result(title, l1, l2, gained, celebrate);
        bsp_lvgl_unlock();
    }
}

// ---------------------------------------------------------------------------
// 练习
// ---------------------------------------------------------------------------

static void start_session(wb_session_kind_t kind) {
    uint16_t n = wb_session_build(&s_session, &s_deck, kind);
    if (kind == WB_SESSION_TODAY && n > 0) {
        // 新词在建队列时已放进 1 号盒,整副落盘。
        wb_store_save_all(&s_deck);
    }
    if (n == 0) {
        if (kind == WB_SESSION_BOSS) {
            show_result("3 号盒还空着", "卡片在 2 号盒答对后才会进 3 号盒。",
                        "先把每天的任务做完吧", 0, false);
        } else if (kind == WB_SESSION_SORT) {
            show_result("没有要分拣的新词", "长按 OK 进入换词模式，用手机加新词。",
                        NULL, 0, false);
        } else {
            show_home();
        }
        return;
    }
    s_sort_known = 0;
    s_flipped = false;
    s_mode = MODE_CARD;
    show_current_card();
    play_current(false);
}

static void finish_session(void) {
    const wb_session_kind_t kind = s_session.kind;
    const uint16_t total = s_session.first_pass;
    const uint16_t wrong = s_session.wrong;
    uint16_t gained = wb_session_finish(&s_session, &s_deck);
    wb_store_save_meta(&s_deck);

    char l1[80], l2[80];
    if (kind == WB_SESSION_TODAY) {
        snprintf(l1, sizeof(l1), "会了 %u 张  不会 %u 张",
                 (unsigned)(total - wrong), (unsigned)wrong);
        show_result("今日完成！", l1, "明天开机再来", gained, wrong == 0);
    } else if (kind == WB_SESSION_BOSS) {
        if (wrong == 0) {
            snprintf(l1, sizeof(l1), "%u 张全部毕业，徽章 +1", (unsigned)total);
            show_result("Boss 通关！", l1, "收进图鉴啦", gained, true);
        } else {
            snprintf(l1, sizeof(l1), "毕业 %u 张  回 1 号盒 %u 张",
                     (unsigned)(total - wrong), (unsigned)wrong);
            show_result("差一点！", l1, "下次再来挑战", gained, false);
        }
    } else {
        snprintf(l1, sizeof(l1), "认识 %u 张，直接毕业", (unsigned)s_sort_known);
        snprintf(l2, sizeof(l2), "其余 %u 张每天放 %d 张进 1 号盒",
                 (unsigned)(total - s_sort_known), WB_NEW_PER_DAY);
        show_result("分拣完成", l1, l2, 0, false);
    }
}

static void next_card(void) {
    if (wb_session_finished(&s_session)) {
        finish_session();
    } else {
        s_flipped = false;
        s_mode = MODE_CARD;
        show_current_card();
        play_current(false);
    }
}

static void answer(bool known) {
    wb_audio_stop();
    const int cur = wb_session_current(&s_session);
    const uint8_t prev_box = cur >= 0 ? s_deck.cards[cur].box : WB_BOX_1;
    const bool retry = s_session.pos >= s_session.first_pass;
    int idx = wb_session_answer(&s_session, &s_deck, known);
    if (idx >= 0) wb_store_save_card(&s_deck, (uint16_t)idx);
    if (s_session.kind == WB_SESSION_SORT && known) s_sort_known++;

    if (ui_lock()) {
        wb_ui_card_feedback(known);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(WB_UI_FEEDBACK_MS));
    // 动画期间连按的键作废,免得一下子答掉下一张。
    xQueueReset(s_queue);

    // 2 号、3 号盒还答错,说明真没记住:停下来去白板写 3 遍。
    if (!known && idx >= 0 && !retry && s_session.kind != WB_SESSION_SORT &&
        (prev_box == WB_BOX_2 || prev_box == WB_BOX_3)) {
        s_write_idx = idx;
        s_mode = MODE_WRITE;
        if (ui_lock()) {
            wb_ui_show_write(&s_deck.cards[idx], s_session.kind == WB_SESSION_BOSS);
            bsp_lvgl_unlock();
        }
        play_current(false);
        return;
    }
    next_card();
}

// ---------------------------------------------------------------------------
// 换词模式
// ---------------------------------------------------------------------------

static void post_portal(wb_portal_event_t ev, void *user) {
    (void)user;
    const app_event_t e = { .type = EV_PORTAL, .portal_ev = ev };
    (void)xQueueSend(s_queue, &e, 0);
}

// httpd 任务里执行。
static bool on_submit(const char *body, size_t len, char *reply, size_t reply_size, void *user) {
    (void)user;
    bool new_wifi = false;
    size_t w = 0;
    reply[0] = '\0';

    char *words = malloc(len + 1);
    wb_parsed_word_t *parsed = malloc(sizeof(wb_parsed_word_t) * WB_MAX_CARDS);
    if (!words || !parsed) {
        free(words);
        free(parsed);
        snprintf(reply, reply_size, "设备内存不够，请少放一些词再试。");
        return false;
    }

    if (wb_form_get(body, len, "words", words, len + 1) > 0) {
        size_t n = wb_parse_words(words, strlen(words), parsed, WB_MAX_CARDS);
        wb_word_t *list = malloc(sizeof(wb_word_t) * (n ? n : 1));
        if (list) {
            for (size_t i = 0; i < n; i++) {
                list[i] = (wb_word_t){ parsed[i].en, parsed[i].chunk, parsed[i].zh, NULL };
            }
            uint16_t dup = 0, full = 0;
            deck_lock();
            uint16_t added = wb_deck_merge(&s_deck, list, n, &dup, &full);
            esp_err_t err = added ? wb_store_save_all(&s_deck) : ESP_OK;
            deck_unlock();
            free(list);
            w += (size_t)snprintf(reply + w, reply_size - w, "收到 %u 个新词", (unsigned)added);
            if (dup && w < reply_size) {
                w += (size_t)snprintf(reply + w, reply_size - w, "，跳过重复 %u 个", (unsigned)dup);
            }
            if (w < reply_size) w += (size_t)snprintf(reply + w, reply_size - w, "。");
            if (full && w < reply_size) {
                w += (size_t)snprintf(reply + w, reply_size - w,
                                      "\n盒子满了，%u 个没放进去。", (unsigned)full);
            }
            if (err != ESP_OK && w < reply_size) {
                w += (size_t)snprintf(reply + w, reply_size - w, "\n保存出错，请重试。");
            }
        }
    }
    free(words);
    free(parsed);

    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    if (wb_form_get(body, len, "ssid", ssid, sizeof(ssid)) > 0) {
        wb_form_get(body, len, "pass", pass, sizeof(pass));
        if (wb_store_set_wifi(ssid, pass) == ESP_OK) {
            new_wifi = true;
            if (w < reply_size) {
                w += (size_t)snprintf(reply + w, reply_size - w,
                                      "\n家里 Wi-Fi 已保存，设备正在试连。");
            }
        }
    }
    if (w == 0) snprintf(reply, reply_size, "没有收到内容。");

    deck_lock();
    snprintf(s_upload_line, sizeof(s_upload_line), "%s", reply);
    deck_unlock();
    return new_wifi;
}

static void enter_portal(void) {
    s_mode = MODE_PORTAL;
    s_upload_line[0] = '\0';
    esp_err_t err = wb_portal_start(on_submit, post_portal, NULL);
    if (err != ESP_OK) {
        show_result("换词模式打不开", esp_err_to_name(err), "OK 回首页后再试一次", 0, false);
        return;
    }
    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    bool has_home = wb_store_get_wifi(ssid, pass) == ESP_OK && ssid[0];
    if (ui_lock()) {
        wb_ui_show_portal(wb_portal_ap_ssid(), WB_PORTAL_PASS, WB_PORTAL_IP);
        wb_ui_portal_status(NULL, has_home ? "家里 Wi-Fi：连接中..." : "家里 Wi-Fi：未设置");
        bsp_lvgl_unlock();
    }
}

static void leave_portal(void) {
    wb_portal_stop();
    show_home();
}

static void handle_portal_event(wb_portal_event_t ev) {
    if (s_mode != MODE_PORTAL) return;
    char line[sizeof(s_upload_line)];
    const char *upload = NULL;
    const char *home = NULL;
    switch (ev) {
    case WB_PORTAL_EV_PHONE_JOINED: upload = "手机已连上，打开网页吧"; break;
    case WB_PORTAL_EV_PHONE_LEFT: upload = "手机断开了"; break;
    case WB_PORTAL_EV_HOME_CONNECTING: home = "家里 Wi-Fi：连接中..."; break;
    case WB_PORTAL_EV_HOME_CONNECTED: home = "家里 Wi-Fi：已连上 " LV_SYMBOL_OK; break;
    case WB_PORTAL_EV_HOME_FAILED: home = "家里 Wi-Fi：连不上，检查密码"; break;
    case WB_PORTAL_EV_SUBMITTED:
        deck_lock();
        snprintf(line, sizeof(line), "%s", s_upload_line);
        deck_unlock();
        upload = line;
        break;
    default: break;
    }
    if (ui_lock()) {
        wb_ui_portal_status(upload, home);
        bsp_lvgl_unlock();
    }
}

// ---------------------------------------------------------------------------
// 联网同步
// ---------------------------------------------------------------------------

static void sync_progress(const char *line, uint16_t done, uint16_t total) {
    if (ui_lock()) {
        wb_ui_sync_status(line, done, total);
        bsp_lvgl_unlock();
    }
}

// auto=true 是开机自动同步:失败就悄悄回首页,不打扰孩子。
static void run_sync(bool automatic) {
    s_mode = MODE_SYNC;
    s_sync_cancel = false;
    wb_audio_stop();
    if (ui_lock()) {
        wb_ui_show_sync();
        bsp_lvgl_unlock();
    }
    wb_sync_result_t r;
    wb_sync_run(&s_deck, sync_progress, &s_sync_cancel, &r);
    xQueueReset(s_queue);   // 同步期间按的键作废

    if (s_boot_pending) {
        // 没拿到时间就退回老规则:做完任务、下次开机算新一天。
        s_boot_pending = false;
        if (!r.time_ok && wb_deck_boot(&s_deck)) wb_store_save_meta(&s_deck);
    }

    char l1[80], l2[80];
    if (r.wifi == ESP_ERR_NOT_FOUND) {
        show_result("还没设置家里 Wi-Fi", "长按 OK 进设置页，用手机填上 Wi-Fi。", NULL, 0, false);
        return;
    }
    if (r.wifi != ESP_OK) {
        if (automatic) {
            show_home();
        } else {
            show_result("连不上家里 Wi-Fi", "看看路由器开着没，或者长按 OK 重新填密码。",
                        NULL, 0, false);
        }
        return;
    }
    if (automatic && r.pack == ESP_OK && r.added == 0 && r.audio_done == 0) {
        show_home();
        return;
    }
    if (r.pack != ESP_OK) {
        snprintf(l1, sizeof(l1), "词表没下载下来（%s）", esp_err_to_name(r.pack));
    } else {
        snprintf(l1, sizeof(l1), "新词 %u 个，下好发音 %u 个", (unsigned)r.added,
                 (unsigned)r.audio_done);
    }
    if (r.audio_missing) {
        snprintf(l2, sizeof(l2), "还有 %u 个词没有发音，下次长按 " LV_SYMBOL_DOWN " 接着下",
                 (unsigned)r.audio_missing);
    } else {
        snprintf(l2, sizeof(l2), "%s", r.added ? "首页按 " LV_SYMBOL_DOWN " 分拣新词" : "都是最新的");
    }
    show_result(r.cancelled ? "先停在这里" : "更新好了", l1, l2, 0, r.added > 0);
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    const bool click = ev == BSP_BTN_CLICK;
    const bool ok_long = ev == BSP_BTN_LONG && btn == BSP_BTN_OK;
    const bool up_long = ev == BSP_BTN_LONG && btn == BSP_BTN_UP;
    const bool down_long = ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN;
    if (!click && !ok_long && !up_long && !down_long) return;

    switch (s_mode) {
    case MODE_HOME:
        if (ok_long) enter_portal();
        else if (down_long) run_sync(false);
        else if (up_long) break;
        else if (btn == BSP_BTN_OK) start_session(WB_SESSION_TODAY);
        else if (btn == BSP_BTN_UP) start_session(WB_SESSION_BOSS);
        else if (btn == BSP_BTN_DOWN) start_session(WB_SESSION_SORT);
        break;
    case MODE_CARD:
        if (ok_long) {
            wb_audio_stop();
            show_home();
        } else if (up_long) {
            play_current(s_flipped);     // 再听一遍
        } else if (down_long) {
            break;
        } else if (btn == BSP_BTN_OK) {
            if (!s_flipped) {
                s_flipped = true;
                show_current_card();
                play_current(true);
            }
        } else if (s_session.kind != WB_SESSION_SORT && !s_flipped) {
            // 复习和 Boss 战要先翻面看答案,再自评。
            s_flipped = true;
            show_current_card();
            play_current(true);
        } else {
            answer(btn == BSP_BTN_UP);
        }
        break;
    case MODE_WRITE:
        if (up_long) {
            play_current(false);
        } else if (ok_long) {
            wb_audio_stop();
            show_home();
        } else if (click && btn == BSP_BTN_OK) {
            wb_audio_stop();
            s_write_idx = -1;
            next_card();
        }
        break;
    case MODE_SYNC:
        break;   // OK 取消在 wb_app_post_key 里直接置位
    case MODE_RESULT:
        if (click && btn == BSP_BTN_OK) show_home();
        break;
    case MODE_PORTAL:
        if (ok_long) leave_portal();
        break;
    }
}

// 返回 true 表示这次按键只用来点亮屏幕,不再派发。
static bool wake_screen(void) {
    s_last_key_us = esp_timer_get_time();
    if (!s_dimmed) return false;
    s_dimmed = false;
    bsp_display_backlight(BRIGHT_PERCENT);
    return true;
}

static void tick(void) {
    if (ui_lock()) {
        wb_ui_set_battery(bsp_battery_soc());
        bsp_lvgl_unlock();
    }
    // 换词模式下家长在用手机,不熄屏。
    if (!s_dimmed && s_mode != MODE_PORTAL &&
        esp_timer_get_time() - s_last_key_us > DIM_AFTER_US) {
        s_dimmed = true;
        bsp_display_backlight(DIM_PERCENT);
    }
}

static void app_task(void *arg) {
    (void)arg;
    app_event_t e;
    int64_t last_tick = 0;
    for (;;) {
        if (xQueueReceive(s_queue, &e, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            if (e.type == EV_KEY) {
                if (e.key_ev == BSP_BTN_CLICK || e.key_ev == BSP_BTN_LONG) {
                    if (!wake_screen()) handle_key(e.btn, e.key_ev);
                }
            } else if (e.type == EV_BOOT_SYNC) {
                run_sync(true);
            } else {
                handle_portal_event(e.portal_ev);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_tick >= (int64_t)TICK_MS * 1000) {
            last_tick = now;
            tick();
        }
    }
}

void wb_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_queue) return;
    // 同步时应用任务在阻塞下载,取消要在这里直接置位。
    if (s_mode == MODE_SYNC && btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) s_sync_cancel = true;
    const app_event_t e = { .type = EV_KEY, .btn = btn, .key_ev = ev };
    (void)xQueueSend(s_queue, &e, 0);
}

esp_err_t wb_app_start(void) {
    s_deck_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(app_event_t));
    if (!s_deck_lock || !s_queue) return ESP_ERR_NO_MEM;

    esp_err_t err = wb_store_init();
    if (err == ESP_OK) err = wb_store_load(&s_deck);
    if (err != ESP_OK) {
        // 存储坏了也让孩子能看到界面;进度只是存不下来。
        ESP_LOGE(TAG, "存储不可用： %s", esp_err_to_name(err));
        wb_deck_init(&s_deck);
    }
    // 联网对过时的设备按真实日期翻天:先等开机同步拿时间,拿不到再用老规则。
    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    const bool has_wifi = wb_store_get_wifi(ssid, pass) == ESP_OK && ssid[0];
    if (has_wifi && s_deck.meta.last_date != 0) {
        s_boot_pending = true;
    } else if (wb_deck_boot(&s_deck)) {
        wb_store_save_meta(&s_deck);
    }
    if (wb_audio_start() != ESP_OK) ESP_LOGW(TAG, "没有发音,照常练习");

    if (ui_lock()) {
        wb_ui_init();
        bsp_lvgl_unlock();
    }
    s_last_key_us = esp_timer_get_time();
    show_home();

    if (xTaskCreate(app_task, "wb_app", 10240, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (has_wifi) {
        const app_event_t e = { .type = EV_BOOT_SYNC };
        (void)xQueueSend(s_queue, &e, 0);
    }
    ESP_LOGI(TAG, "单词盒子就绪：%u 张卡，第 %u 天",
             (unsigned)s_deck.meta.count, (unsigned)s_deck.meta.day);
    return ESP_OK;
}
