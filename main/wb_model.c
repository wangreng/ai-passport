// main/wb_model.c —— 见 wb_model.h。
#include "wb_model.h"

#include <ctype.h>
#include <string.h>

void wb_copy_utf8(char *dst, size_t dst_size, const char *src, size_t src_len) {
    if (!dst || dst_size == 0) return;
    if (!src) src_len = 0;
    size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    // 截断落在多字节字符中间时退回到该字符起点(续字节为 10xxxxxx)。
    if (n < src_len) {
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

void wb_deck_init(wb_deck_t *deck) {
    memset(deck, 0, sizeof(*deck));
    deck->meta.day = 1;
}

wb_counts_t wb_deck_counts(const wb_deck_t *deck) {
    wb_counts_t c = {0};
    for (uint16_t i = 0; i < deck->meta.count; i++) {
        switch (deck->cards[i].box) {
        case WB_BOX_UNSORTED: c.unsorted++; break;
        case WB_BOX_POOL:     c.pool++; break;
        case WB_BOX_1:        c.box1++; break;
        case WB_BOX_2:        c.box2++; break;
        case WB_BOX_3:        c.box3++; break;
        case WB_BOX_DONE:     c.done++; break;
        default: break;
        }
    }
    return c;
}

bool wb_deck_boot(wb_deck_t *deck) {
    wb_meta_t *m = &deck->meta;
    if (m->day == 0) {
        m->day = 1;
        m->today_done = 0;
        m->intro_today = 0;
        return true;
    }
    // 昨天做完了,或者昨天根本没有要做的(否则空闲日会把日子卡住),都进入新的一天。
    if (m->today_done || wb_today_due(deck, NULL, NULL) == 0) {
        if (m->day < UINT16_MAX - 8) m->day++;
        m->today_done = 0;
        m->intro_today = 0;
        return true;
    }
    return false;
}

static bool is_due_review(const wb_deck_t *deck, const wb_card_t *c) {
    return (c->box == WB_BOX_1 || c->box == WB_BOX_2) && c->due_day <= deck->meta.day;
}

static uint16_t fresh_quota(const wb_deck_t *deck, uint16_t pool) {
    uint16_t left = deck->meta.intro_today >= WB_NEW_PER_DAY
                  ? 0 : (uint16_t)(WB_NEW_PER_DAY - deck->meta.intro_today);
    return pool < left ? pool : left;
}

uint16_t wb_today_due(const wb_deck_t *deck, uint16_t *review_out, uint16_t *fresh_out) {
    uint16_t review = 0, pool = 0;
    if (!deck->meta.today_done) {
        for (uint16_t i = 0; i < deck->meta.count; i++) {
            const wb_card_t *c = &deck->cards[i];
            if (is_due_review(deck, c)) review++;
            else if (c->box == WB_BOX_POOL) pool++;
        }
    }
    uint16_t fresh = deck->meta.today_done ? 0 : fresh_quota(deck, pool);
    if (review_out) *review_out = review;
    if (fresh_out) *fresh_out = fresh;
    return (uint16_t)(review + fresh);
}

static void push(wb_session_t *s, uint16_t idx) {
    if (s->len < sizeof(s->items) / sizeof(s->items[0])) s->items[s->len++] = idx;
}

uint16_t wb_session_build(wb_session_t *s, wb_deck_t *deck, wb_session_kind_t kind) {
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    const uint16_t n = deck->meta.count;

    if (kind == WB_SESSION_SORT) {
        for (uint16_t i = 0; i < n; i++) {
            if (deck->cards[i].box == WB_BOX_UNSORTED) push(s, i);
        }
    } else if (kind == WB_SESSION_BOSS) {
        for (uint16_t i = 0; i < n; i++) {
            if (deck->cards[i].box == WB_BOX_3) push(s, i);
        }
    } else {
        if (deck->meta.today_done) return 0;
        // 先 2 号盒(快毕业的),再 1 号盒,最后今天的新词。
        for (uint16_t i = 0; i < n; i++) {
            const wb_card_t *c = &deck->cards[i];
            if (c->box == WB_BOX_2 && c->due_day <= deck->meta.day) push(s, i);
        }
        for (uint16_t i = 0; i < n; i++) {
            const wb_card_t *c = &deck->cards[i];
            if (c->box == WB_BOX_1 && c->due_day <= deck->meta.day) push(s, i);
        }
        uint16_t pool = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (deck->cards[i].box == WB_BOX_POOL) pool++;
        }
        uint16_t quota = fresh_quota(deck, pool);
        for (uint16_t i = 0; i < n && quota > 0; i++) {
            wb_card_t *c = &deck->cards[i];
            if (c->box != WB_BOX_POOL) continue;
            // 建队列时就放进 1 号盒:中途断电,重建时它仍按"今天到期"回来。
            c->box = WB_BOX_1;
            c->due_day = deck->meta.day;
            deck->meta.intro_today++;
            quota--;
            push(s, i);
        }
    }
    s->first_pass = s->len;
    return s->len;
}

int wb_session_current(const wb_session_t *s) {
    return s->pos < s->len ? (int)s->items[s->pos] : -1;
}

bool wb_session_finished(const wb_session_t *s) {
    return s->pos >= s->len;
}

int wb_session_answer(wb_session_t *s, wb_deck_t *deck, bool known) {
    int idx = wb_session_current(s);
    if (idx < 0) return -1;
    wb_card_t *c = &deck->cards[idx];
    const bool retry = s->pos >= s->first_pass;
    const uint16_t day = deck->meta.day;
    s->pos++;

    switch (s->kind) {
    case WB_SESSION_SORT:
        if (known) {
            c->box = WB_BOX_DONE;
            deck->meta.collected++;
        } else {
            c->box = WB_BOX_POOL;
        }
        break;
    case WB_SESSION_BOSS:
        if (known) {
            c->box = WB_BOX_DONE;
            deck->meta.collected++;
        } else {
            c->box = WB_BOX_1;
            c->due_day = (uint16_t)(day + 1);
            s->wrong++;
        }
        break;
    case WB_SESSION_TODAY:
    default:
        if (retry) {
            // 当轮末尾的重练只是再看一遍,不改盒子;答错已经在第一遍处理过。
            return -1;
        }
        if (known) {
            if (c->box == WB_BOX_2) {
                c->box = WB_BOX_3;
            } else {
                c->box = WB_BOX_2;
                c->due_day = (uint16_t)(day + 2);
            }
        } else {
            c->box = WB_BOX_1;
            c->due_day = (uint16_t)(day + 1);
            s->wrong++;
            push(s, (uint16_t)idx);
            s->requeued++;
        }
        break;
    }
    return idx;
}

uint16_t wb_session_finish(wb_session_t *s, wb_deck_t *deck) {
    uint16_t gained = 0;
    if (s->kind == WB_SESSION_TODAY && s->first_pass > 0 && !deck->meta.today_done) {
        deck->meta.today_done = 1;
        gained = 1;
    } else if (s->kind == WB_SESSION_BOSS && s->first_pass > 0 && s->wrong == 0) {
        deck->meta.badges++;
        gained = WB_BOSS_POINTS;
    }
    deck->meta.points = (uint16_t)(deck->meta.points + gained);
    s->pos = s->len;
    return gained;
}

static bool same_word(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool remove_oldest_done(wb_deck_t *deck) {
    for (uint16_t i = 0; i < deck->meta.count; i++) {
        if (deck->cards[i].box != WB_BOX_DONE) continue;
        memmove(&deck->cards[i], &deck->cards[i + 1],
                (size_t)(deck->meta.count - i - 1) * sizeof(wb_card_t));
        deck->meta.count--;
        memset(&deck->cards[deck->meta.count], 0, sizeof(wb_card_t));
        return true;
    }
    return false;
}

uint16_t wb_deck_merge(wb_deck_t *deck, const wb_word_t *words, size_t n,
                       uint16_t *skipped_dup, uint16_t *dropped_full) {
    uint16_t added = 0, dup = 0, full = 0;
    for (size_t w = 0; w < n; w++) {
        if (!words[w].en || !words[w].en[0]) continue;
        char en[WB_EN_MAX];
        wb_copy_utf8(en, sizeof(en), words[w].en, strlen(words[w].en));
        bool exists = false;
        for (uint16_t i = 0; i < deck->meta.count && !exists; i++) {
            exists = same_word(deck->cards[i].en, en);
        }
        if (exists) {
            dup++;
            continue;
        }
        if (deck->meta.count >= WB_MAX_CARDS && !remove_oldest_done(deck)) {
            full++;
            continue;
        }
        wb_card_t *c = &deck->cards[deck->meta.count++];
        memset(c, 0, sizeof(*c));
        memcpy(c->en, en, sizeof(en));
        const char *chunk = words[w].chunk ? words[w].chunk : "";
        const char *zh = words[w].zh ? words[w].zh : "";
        wb_copy_utf8(c->chunk, sizeof(c->chunk), chunk, strlen(chunk));
        wb_copy_utf8(c->zh, sizeof(c->zh), zh, strlen(zh));
        c->box = WB_BOX_UNSORTED;
        added++;
    }
    if (skipped_dup) *skipped_dup = dup;
    if (dropped_full) *dropped_full = full;
    return added;
}
