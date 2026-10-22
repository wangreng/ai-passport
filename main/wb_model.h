// main/wb_model.h —— 单词盒子的纯逻辑:卡片、三盒复习调度、分拣、Boss 战、合并新词。
//
// 不依赖 ESP-IDF / LVGL,便于 host 测试(tests/test_wb_model.c)。
//
// 盒子规则:
//   UNSORTED  新上传、等孩子分拣
//   POOL      分拣为"不认识",排队每天放几张进 1 号盒
//   BOX1      每天复习
//   BOX2      隔天复习
//   BOX3      只在 Boss 战里出
//   DONE      毕业(Boss 战答对,或分拣时直接认识)
// 答对升一盒,答错回 1 号盒。
//
// "天"不依赖实时时钟:当天任务完成(或无事可做)后,下一次开机算新的一天。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WB_MAX_CARDS     120
#define WB_EN_MAX        32   // 含结尾 NUL
#define WB_CHUNK_MAX     48
#define WB_ZH_MAX        40   // UTF-8 字节,约 12 个汉字
#define WB_SLUG_MAX      24   // 词包里的音频文件名前缀
#define WB_SLOT_COUNT    120  // 发音槽数量,见 wb_slots.h
#define WB_SLOT_NONE     0xFF
#define WB_NEW_PER_DAY   6
#define WB_BOSS_POINTS   5

typedef enum {
    WB_BOX_UNSORTED = 0,
    WB_BOX_POOL,
    WB_BOX_1,
    WB_BOX_2,
    WB_BOX_3,
    WB_BOX_DONE,
} wb_box_t;

typedef struct {
    char en[WB_EN_MAX];
    char chunk[WB_CHUNK_MAX];
    char zh[WB_ZH_MAX];
    char slug[WB_SLUG_MAX]; // 来自词包;网页手填的卡为空
    uint8_t box;        // wb_box_t
    uint8_t slot;       // 发音槽,WB_SLOT_NONE 表示还没有发音
    uint16_t due_day;   // BOX1/BOX2 在这一天(含)之后到期
} wb_card_t;

// 持久化的进度。cards 单独逐张存,这里只放计数。
typedef struct {
    uint16_t day;            // 第几天,从 1 开始
    uint8_t today_done;      // 当天任务已完成
    uint8_t intro_today;     // 当天已放进 1 号盒的新词数
    uint16_t points;
    uint16_t badges;         // Boss 战通关次数
    uint16_t collected;      // 历史毕业总数(含已被新词挤掉的)
    uint16_t count;          // cards 有效张数
    uint32_t last_date;      // 联网对时后的日期 YYYYMMDD,0 表示从没对过时
} wb_meta_t;

typedef struct {
    wb_meta_t meta;
    wb_card_t cards[WB_MAX_CARDS];
} wb_deck_t;

// 一轮练习的队列(不持久化,开机或进入模式时重建)。
typedef enum {
    WB_SESSION_TODAY = 0,
    WB_SESSION_SORT,
    WB_SESSION_BOSS,
} wb_session_kind_t;

typedef struct {
    wb_session_kind_t kind;
    uint16_t items[WB_MAX_CARDS + WB_MAX_CARDS / 2]; // 答错的卡在当轮末尾再出一次
    uint16_t len;
    uint16_t pos;
    uint16_t first_pass;     // 第一遍的张数;pos 超过它就是末尾重练
    uint16_t wrong;          // 本轮答错次数
    uint16_t requeued;       // 已追加到末尾的张数
} wb_session_t;

typedef struct {
    uint16_t unsorted, pool, box1, box2, box3, done;
} wb_counts_t;

void wb_deck_init(wb_deck_t *deck);
wb_counts_t wb_deck_counts(const wb_deck_t *deck);

// 开机调用:当天已完成、或当天没有任何要做的,就进入新的一天。返回 true 表示 day 有变化(需保存 meta)。
bool wb_deck_boot(wb_deck_t *deck);

// 联网拿到北京时间后调用,date 为 YYYYMMDD。日期变了就进入新的一天
// (第一次对时只记日期不翻天)。返回 true 表示 meta 有变化。
bool wb_deck_set_date(wb_deck_t *deck, uint32_t date);

// 今日任务的张数(到期复习 + 还能放进来的新词)。today_done 时为 0。
uint16_t wb_today_due(const wb_deck_t *deck, uint16_t *review_out, uint16_t *fresh_out);

// 建队列。返回张数,0 表示没东西可练。
uint16_t wb_session_build(wb_session_t *s, wb_deck_t *deck, wb_session_kind_t kind);

// 当前卡片下标;队列结束返回 -1。
int wb_session_current(const wb_session_t *s);

// 记录答案并前进。返回被修改的卡片下标(调用方据此保存),无卡返回 -1。
// 分拣时 known=认识。
int wb_session_answer(wb_session_t *s, wb_deck_t *deck, bool known);

bool wb_session_finished(const wb_session_t *s);

// 一轮结束后的结算。返回本次加的积分。
//   TODAY:标记 today_done,+1 分
//   BOSS :零失误且至少一张,+WB_BOSS_POINTS 分、徽章 +1
//   SORT :无积分
uint16_t wb_session_finish(wb_session_t *s, wb_deck_t *deck);

// 合并新词。en 重复(忽略大小写)跳过;满了就挤掉最早的毕业卡。
// 返回实际加入张数;skipped_dup / dropped_full 可为 NULL。
// slug 可为 NULL(网页手填)。带 slug 的词来自词包:已存在的卡会跟着更新
// slug、词块和中文,词块变了就把 slot 清成 WB_SLOT_NONE 等重下发音。
typedef struct {
    const char *en;
    const char *chunk;
    const char *zh;
    const char *slug;
} wb_word_t;
uint16_t wb_deck_merge(wb_deck_t *deck, const wb_word_t *words, size_t n,
                       uint16_t *skipped_dup, uint16_t *dropped_full);

// 找一个没被任何卡占用的发音槽;都满了返回 WB_SLOT_NONE。
uint8_t wb_deck_free_slot(const wb_deck_t *deck);

// 按 UTF-8 字符边界安全复制,dst 总以 NUL 结尾。
void wb_copy_utf8(char *dst, size_t dst_size, const char *src, size_t src_len);
