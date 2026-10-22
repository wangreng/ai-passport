// main/wb_parse.c —— 见 wb_parse.h。
#include "wb_parse.h"

#include <string.h>

// 全角竖线 U+FF5C 的 UTF-8。
static const char FULLWIDTH_BAR[] = "\xEF\xBD\x9C";

static int sep_len_at(const char *p, const char *end) {
    if (p >= end) return 0;
    if (*p == '|' || *p == '\t') return 1;
    if (end - p >= 3 && memcmp(p, FULLWIDTH_BAR, 3) == 0) return 3;
    return 0;
}

static void trim(const char **b, const char **e) {
    // 只去 ASCII 空白和全角空格 U+3000。
    for (;;) {
        if (*b < *e && (**b == ' ' || **b == '\r' || **b == '\t')) { (*b)++; continue; }
        if (*e - *b >= 3 && memcmp(*b, "\xE3\x80\x80", 3) == 0) { *b += 3; continue; }
        break;
    }
    for (;;) {
        if (*e > *b && ((*e)[-1] == ' ' || (*e)[-1] == '\r' || (*e)[-1] == '\t')) { (*e)--; continue; }
        if (*e - *b >= 3 && memcmp(*e - 3, "\xE3\x80\x80", 3) == 0) { *e -= 3; continue; }
        break;
    }
}

size_t wb_parse_words(const char *text, size_t len, wb_parsed_word_t *out, size_t max) {
    size_t n = 0;
    const char *p = text;
    const char *end = text + len;
    while (p < end && n < max) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;

        const char *fb[3], *fe[3];
        int fields = 0;
        const char *q = p;
        const char *start = p;
        while (q <= line_end && fields < 3) {
            int sl = q < line_end ? sep_len_at(q, line_end) : 0;
            if (q == line_end || sl) {
                fb[fields] = start;
                fe[fields] = q;
                trim(&fb[fields], &fe[fields]);
                fields++;
                if (q == line_end) break;
                q += sl;
                start = q;
                continue;
            }
            q++;
        }

        if (fields > 0 && fe[0] > fb[0] && *fb[0] != '#') {
            wb_parsed_word_t *w = &out[n];
            memset(w, 0, sizeof(*w));
            wb_copy_utf8(w->en, sizeof(w->en), fb[0], (size_t)(fe[0] - fb[0]));
            if (fields == 2) {
                wb_copy_utf8(w->zh, sizeof(w->zh), fb[1], (size_t)(fe[1] - fb[1]));
            } else if (fields == 3) {
                wb_copy_utf8(w->chunk, sizeof(w->chunk), fb[1], (size_t)(fe[1] - fb[1]));
                wb_copy_utf8(w->zh, sizeof(w->zh), fb[2], (size_t)(fe[2] - fb[2]));
            }
            n++;
        }
        p = line_end + 1;
    }
    return n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t wb_url_decode(char *s, size_t len) {
    size_t r = 0, w = 0;
    while (r < len) {
        char c = s[r];
        if (c == '+') {
            s[w++] = ' ';
            r++;
        } else if (c == '%' && r + 2 < len && hexval(s[r + 1]) >= 0 && hexval(s[r + 2]) >= 0) {
            s[w++] = (char)(hexval(s[r + 1]) * 16 + hexval(s[r + 2]));
            r += 3;
        } else {
            s[w++] = c;
            r++;
        }
    }
    s[w] = '\0';
    return w;
}

// 截断后末尾可能留下半个多字节字符,去掉它。
static size_t utf8_trim_incomplete(char *s, size_t len) {
    size_t k = len;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) k--;
    if (k == 0) return len;
    unsigned char lead = (unsigned char)s[k - 1];
    size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    if (len - (k - 1) < need) {
        s[k - 1] = '\0';
        return k - 1;
    }
    return len;
}

int wb_form_get(const char *body, size_t body_len, const char *key, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    const size_t klen = strlen(key);
    const char *p = body;
    const char *end = body + body_len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *pair_end = amp ? amp : end;
        if ((size_t)(pair_end - p) > klen && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = (size_t)(pair_end - v);
            // 先截到缓冲大小再解码:解码只会变短,不会越界。
            size_t n = vlen < out_size - 1 ? vlen : out_size - 1;
            memcpy(out, v, n);
            size_t dec = wb_url_decode(out, n);
            if (n < vlen) dec = utf8_trim_incomplete(out, dec);
            return (int)dec;
        }
        p = pair_end + 1;
    }
    return -1;
}

static void copy_field(char *dst, size_t size, const char *b, const char *e) {
    wb_copy_utf8(dst, size, b, (size_t)(e - b));
}

int wb_parse_pack(const char *text, size_t len, wb_parsed_word_t *out, size_t max,
                  char *version_out, size_t version_size) {
    static const char HEAD[] = "#wordpack 1";
    if (version_out && version_size) version_out[0] = '\0';
    if (len < sizeof(HEAD) - 1 || memcmp(text, HEAD, sizeof(HEAD) - 1) != 0) return -1;

    size_t n = 0;
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const char *e = line_end;
        if (e > p && e[-1] == '\r') e--;

        if (e - p > 8 && memcmp(p, "version=", 8) == 0) {
            if (version_out && version_size) copy_field(version_out, version_size, p + 8, e);
        } else if (e > p && *p != '#' && n < max) {
            const char *f[4];
            const char *fe[4];
            int k = 0;
            const char *start = p;
            for (const char *q = p; q <= e && k < 4; q++) {
                if (q == e || *q == '|') {
                    f[k] = start;
                    fe[k] = q;
                    k++;
                    start = q + 1;
                }
            }
            if (k == 4 && fe[0] > f[0]) {
                wb_parsed_word_t *w = &out[n++];
                memset(w, 0, sizeof(*w));
                copy_field(w->en, sizeof(w->en), f[0], fe[0]);
                copy_field(w->chunk, sizeof(w->chunk), f[1], fe[1]);
                copy_field(w->zh, sizeof(w->zh), f[2], fe[2]);
                copy_field(w->slug, sizeof(w->slug), f[3], fe[3]);
            }
        }
        p = line_end + 1;
    }
    return (int)n;
}
