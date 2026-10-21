// Host tests for the word-box scheduling model and word-list parser.
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "wb_adpcm.h"
#include "wb_model.h"
#include "wb_parse.h"
#include "wb_adpcm_vector.h"

static wb_deck_t deck;
static wb_session_t session;

static void add_words(int n) {
    static char names[WB_MAX_CARDS + 10][16];
    wb_word_t words[WB_MAX_CARDS + 10];
    for (int i = 0; i < n; i++) {
        names[i][0] = 'w';
        names[i][1] = (char)('a' + i / 26 % 26);
        names[i][2] = (char)('a' + i % 26);
        names[i][3] = (char)('0' + i / 676);
        names[i][4] = '\0';
        words[i] = (wb_word_t){ names[i], "a chunk", "\xE5\x80\x9F", NULL };
    }
    wb_deck_merge(&deck, words, (size_t)n, NULL, NULL);
}

static void test_sort_then_daily_intro(void) {
    wb_deck_init(&deck);
    add_words(10);
    wb_counts_t c = wb_deck_counts(&deck);
    assert(c.unsorted == 10);

    // Sort: first 3 known (graduate), rest unknown (pool).
    assert(wb_session_build(&session, &deck, WB_SESSION_SORT) == 10);
    for (int i = 0; i < 10; i++) wb_session_answer(&session, &deck, i < 3);
    assert(wb_session_finished(&session));
    assert(wb_session_finish(&session, &deck) == 0);
    c = wb_deck_counts(&deck);
    assert(c.done == 3 && c.pool == 7 && deck.meta.collected == 3);

    // Day 1: only WB_NEW_PER_DAY new words come in.
    uint16_t review, fresh;
    assert(wb_today_due(&deck, &review, &fresh) == WB_NEW_PER_DAY);
    assert(review == 0 && fresh == WB_NEW_PER_DAY);
    assert(wb_session_build(&session, &deck, WB_SESSION_TODAY) == WB_NEW_PER_DAY);
    c = wb_deck_counts(&deck);
    assert(c.box1 == WB_NEW_PER_DAY && c.pool == 1);

    // Answer: first wrong, others right. Wrong card comes back once at the end.
    for (int i = 0; i < WB_NEW_PER_DAY; i++) wb_session_answer(&session, &deck, i != 0);
    assert(!wb_session_finished(&session));
    int retry = wb_session_current(&session);
    assert(retry >= 0);
    assert(wb_session_answer(&session, &deck, true) == -1); // practice only
    assert(deck.cards[retry].box == WB_BOX_1);
    assert(wb_session_finished(&session));
    assert(wb_session_finish(&session, &deck) == 1);
    assert(deck.meta.today_done && deck.meta.points == 1);
    c = wb_deck_counts(&deck);
    assert(c.box1 == 1 && c.box2 == WB_NEW_PER_DAY - 1);

    // Nothing more today, even if the device is not rebooted.
    assert(wb_today_due(&deck, NULL, NULL) == 0);
    assert(wb_session_build(&session, &deck, WB_SESSION_TODAY) == 0);

    // Day 2: box1 card due, box2 not yet (due day 3), last pool word comes in.
    assert(wb_deck_boot(&deck));
    assert(deck.meta.day == 2 && !deck.meta.today_done);
    assert(!wb_deck_boot(&deck)); // reboot without finishing keeps the same day
    assert(wb_today_due(&deck, &review, &fresh) == 2);
    assert(review == 1 && fresh == 1);
}

static void test_box2_to_box3_and_boss(void) {
    wb_deck_init(&deck);
    add_words(2);
    wb_session_build(&session, &deck, WB_SESSION_SORT);
    wb_session_answer(&session, &deck, false);
    wb_session_answer(&session, &deck, false);
    wb_session_finish(&session, &deck);

    // Day 1: both right -> box2 (due day 3).
    wb_session_build(&session, &deck, WB_SESSION_TODAY);
    while (!wb_session_finished(&session)) wb_session_answer(&session, &deck, true);
    wb_session_finish(&session, &deck);
    wb_deck_boot(&deck); // day 2
    assert(wb_today_due(&deck, NULL, NULL) == 0);
    // A day with nothing due does not block the calendar.
    assert(wb_deck_boot(&deck));
    assert(deck.meta.day == 3);
    assert(wb_session_build(&session, &deck, WB_SESSION_TODAY) == 2);
    while (!wb_session_finished(&session)) wb_session_answer(&session, &deck, true);
    wb_session_finish(&session, &deck);
    assert(wb_deck_counts(&deck).box3 == 2);

    // Boss with one mistake: no badge, wrong card back to box1.
    assert(wb_session_build(&session, &deck, WB_SESSION_BOSS) == 2);
    wb_session_answer(&session, &deck, true);
    wb_session_answer(&session, &deck, false);
    assert(wb_session_finished(&session)); // no retries in boss
    assert(wb_session_finish(&session, &deck) == 0);
    wb_counts_t c = wb_deck_counts(&deck);
    assert(c.done == 1 && c.box1 == 1 && deck.meta.badges == 0);

    // Empty boss gives nothing.
    assert(wb_session_build(&session, &deck, WB_SESSION_BOSS) == 0);
    assert(wb_session_finish(&session, &deck) == 0);
}

static void test_boss_clear_badge(void) {
    wb_deck_init(&deck);
    add_words(3);
    for (uint16_t i = 0; i < 3; i++) deck.cards[i].box = WB_BOX_3;
    wb_session_build(&session, &deck, WB_SESSION_BOSS);
    while (!wb_session_finished(&session)) wb_session_answer(&session, &deck, true);
    assert(wb_session_finish(&session, &deck) == WB_BOSS_POINTS);
    assert(deck.meta.badges == 1 && deck.meta.collected == 3);
}

static void test_set_date(void) {
    wb_deck_init(&deck);
    assert(wb_deck_set_date(&deck, 20261021));   // first sync: remember only
    assert(deck.meta.day == 1 && deck.meta.last_date == 20261021);
    assert(!wb_deck_set_date(&deck, 20261021));  // same day
    deck.meta.intro_today = 3;
    deck.meta.today_done = 1;
    assert(wb_deck_set_date(&deck, 20261022));
    assert(deck.meta.day == 2 && !deck.meta.today_done && deck.meta.intro_today == 0);
    assert(!wb_deck_set_date(&deck, 20261020));  // clock went backwards: ignore
    assert(!wb_deck_set_date(&deck, 0));
}

static void test_slug_and_slots(void) {
    wb_deck_init(&deck);
    wb_word_t w1[1] = { { "go", "", "", NULL } };
    wb_deck_merge(&deck, w1, 1, NULL, NULL);
    assert(deck.cards[0].slug[0] == '\0' && deck.cards[0].slot == WB_SLOT_NONE);
    wb_word_t w2[2] = { { "Go", "go home", "", "go-1a2b3c" }, { "run", "", "", "run-4d5e6f" } };
    uint16_t dup = 0;
    assert(wb_deck_merge(&deck, w2, 2, &dup, NULL) == 1 && dup == 1);
    assert(strcmp(deck.cards[0].slug, "go-1a2b3c") == 0);   // patched onto the old card
    assert(strcmp(deck.cards[1].slug, "run-4d5e6f") == 0);
    assert(wb_deck_free_slot(&deck) == 0);
    deck.cards[0].slot = 0;
    assert(wb_deck_free_slot(&deck) == 1);
    deck.cards[1].slot = 1;
    assert(wb_deck_free_slot(&deck) == 2);

    // A corrected chunk in the pack updates the card and drops its audio slot.
    wb_word_t w3[1] = { { "run", "run fast", "\xE8\xB7\x91", "run-4d5e6f" } };
    wb_deck_merge(&deck, w3, 1, NULL, NULL);
    assert(strcmp(deck.cards[1].chunk, "run fast") == 0 && deck.cards[1].slot == WB_SLOT_NONE);
    assert(deck.cards[0].slot == 0);
    // Same text again keeps the slot.
    deck.cards[1].slot = 1;
    wb_deck_merge(&deck, w3, 1, NULL, NULL);
    assert(deck.cards[1].slot == 1);
}

static void test_pack(void) {
    const char text[] =
        "#wordpack 1\r\n"
        "version=abc123\n"
        "borrow|borrow a book|\xE5\x80\x9F|borrow-72cd0a\n"
        "bad|line\n"
        "jacket||\xE5\xA4\xB9\xE5\x85\x8B|jacket-000001\n";
    wb_parsed_word_t out[4];
    char ver[WB_PACK_VERSION_MAX];
    assert(wb_parse_pack(text, sizeof(text) - 1, out, 4, ver, sizeof(ver)) == 2);
    assert(strcmp(ver, "abc123") == 0);
    assert(strcmp(out[0].slug, "borrow-72cd0a") == 0);
    assert(strcmp(out[1].en, "jacket") == 0 && out[1].chunk[0] == '\0');
    assert(wb_parse_pack("<html>", 6, out, 4, ver, sizeof(ver)) == -1);
}

static void test_adpcm(void) {
    uint32_t rate = 0;
    assert(wb_adpcm_parse_header(WB_VEC_ADP, sizeof(WB_VEC_ADP), &rate) == 64);
    assert(rate == 12000);
    assert(wb_adpcm_parse_header(WB_VEC_ADP, 20, NULL) == 0);      // truncated
    uint8_t bad[sizeof(WB_VEC_ADP)];
    memcpy(bad, WB_VEC_ADP, sizeof(bad));
    bad[0] = 'X';
    assert(wb_adpcm_parse_header(bad, sizeof(bad), NULL) == 0);
    // Decode in two pieces to cover the streaming state.
    int16_t pcm[64];
    wb_adpcm_state_t st;
    wb_adpcm_init(&st);
    wb_adpcm_decode(&st, WB_VEC_ADP + WB_ADPCM_HEADER, 10, pcm);
    wb_adpcm_decode(&st, WB_VEC_ADP + WB_ADPCM_HEADER + 10, 22, pcm + 20);
    for (int i = 0; i < 64; i++) assert(pcm[i] == WB_VEC_PCM[i]);
    // After the step size ramps up, the decoder tracks the source closely.
    for (int i = 40; i < 64; i++) assert(abs(pcm[i] - WB_VEC_SRC[i]) < 1200);

    // 12 kHz -> 16 kHz: 3 in, 4 out; split calls give the same stream.
    wb_resample_t rs;
    int16_t up[100];
    const int16_t ramp[6] = { 300, 600, 900, 1200, 1500, 1800 };
    wb_resample_init(&rs, 12000);
    size_t n = wb_resample(&rs, ramp, 2, up);
    n += wb_resample(&rs, ramp + 2, 4, up + n);
    assert(n == 8);
    // The line through (0,0),(1,300),... sampled every 0.75 input steps.
    for (size_t i = 0; i < n; i++) assert(up[i] == (int16_t)(i * 225));
    wb_resample_init(&rs, 16000);
    assert(wb_resample(&rs, ramp, 6, up) == 6 && up[5] == 1800);
}

static void test_merge_dup_and_full(void) {
    wb_deck_init(&deck);
    wb_word_t w[3] = {
        { "Borrow", "borrow a book", "\xE5\x80\x9F", NULL },
        { "borrow", "", "", NULL },
        { "", "", "", NULL },
    };
    uint16_t dup = 0, full = 0;
    assert(wb_deck_merge(&deck, w, 3, &dup, &full) == 1);
    assert(dup == 1 && full == 0);

    wb_deck_init(&deck);
    add_words(WB_MAX_CARDS);
    assert(deck.meta.count == WB_MAX_CARDS);
    wb_word_t extra[2] = { { "zebra", "", "", NULL }, { "yak", "", "", NULL } };
    assert(wb_deck_merge(&deck, extra, 2, &dup, &full) == 0);
    assert(full == 2);

    deck.cards[5].box = WB_BOX_DONE;
    assert(wb_deck_merge(&deck, extra, 2, &dup, &full) == 1);
    assert(full == 1 && deck.meta.count == WB_MAX_CARDS);
    assert(strcmp(deck.cards[WB_MAX_CARDS - 1].en, "zebra") == 0);
}

static void test_copy_utf8(void) {
    char buf[5];
    // "借书" is 6 bytes; only one full character fits in 4 bytes + NUL.
    wb_copy_utf8(buf, sizeof(buf), "\xE5\x80\x9F\xE4\xB9\xA6", 6);
    assert(strcmp(buf, "\xE5\x80\x9F") == 0);
    wb_copy_utf8(buf, sizeof(buf), "abcdefg", 7);
    assert(strcmp(buf, "abcd") == 0);
}

static void test_parse(void) {
    const char text[] =
        "borrow | borrow a book | \xE5\x80\x9F\n"
        "\n"
        "# comment\n"
        "heavy\t\xE9\x87\x8D\xE7\x9A\x84\r\n"
        "  jacket  \n"
        "quiet\xEF\xBD\x9C" "a quiet room\xEF\xBD\x9C\xE5\xAE\x89\xE9\x9D\x99";
    wb_parsed_word_t out[8];
    size_t n = wb_parse_words(text, sizeof(text) - 1, out, 8);
    assert(n == 4);
    assert(strcmp(out[0].en, "borrow") == 0);
    assert(strcmp(out[0].chunk, "borrow a book") == 0);
    assert(strcmp(out[0].zh, "\xE5\x80\x9F") == 0);
    assert(strcmp(out[1].en, "heavy") == 0 && out[1].chunk[0] == '\0');
    assert(strcmp(out[1].zh, "\xE9\x87\x8D\xE7\x9A\x84") == 0);
    assert(strcmp(out[2].en, "jacket") == 0 && out[2].zh[0] == '\0');
    assert(strcmp(out[3].chunk, "a quiet room") == 0);
    assert(strcmp(out[3].zh, "\xE5\xAE\x89\xE9\x9D\x99") == 0);

    assert(wb_parse_words(text, sizeof(text) - 1, out, 2) == 2);
}

static void test_form(void) {
    const char body[] = "ssid=My+Home%20WiFi&pass=a%26b&words=go+%7C+%E5%8E%BB%0Aok";
    char v[64];
    assert(wb_form_get(body, sizeof(body) - 1, "ssid", v, sizeof(v)) == 12);
    assert(strcmp(v, "My Home WiFi") == 0);
    assert(wb_form_get(body, sizeof(body) - 1, "pass", v, sizeof(v)) == 3);
    assert(strcmp(v, "a&b") == 0);
    assert(wb_form_get(body, sizeof(body) - 1, "words", v, sizeof(v)) > 0);
    assert(strcmp(v, "go | \xE5\x8E\xBB\nok") == 0);
    assert(wb_form_get(body, sizeof(body) - 1, "nope", v, sizeof(v)) == -1);
    // Truncation never leaves half a character.
    char small[8];
    wb_form_get(body, sizeof(body) - 1, "words", small, 7);
    assert(strcmp(small, "go | ") == 0 || strcmp(small, "go |") == 0);
}

int main(void) {
    test_sort_then_daily_intro();
    test_box2_to_box3_and_boss();
    test_boss_clear_badge();
    test_set_date();
    test_slug_and_slots();
    test_pack();
    test_adpcm();
    test_merge_dup_and_full();
    test_copy_utf8();
    test_parse();
    test_form();
    return 0;
}
