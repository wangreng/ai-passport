// main/wb_parse.h —— 换词网页提交内容的解析(纯逻辑,host 测试覆盖)。
//
// 词表一行一张卡,字段用 | 、全角 ｜ 或 Tab 分隔:
//   borrow | borrow a book | 借
//   heavy | 重的                 (两个字段 = 英文 | 中文)
//   jacket                       (只有英文)
// 空行和 # 开头的行忽略。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wb_model.h"

typedef struct {
    char en[WB_EN_MAX];
    char chunk[WB_CHUNK_MAX];
    char zh[WB_ZH_MAX];
    char slug[WB_SLUG_MAX];
} wb_parsed_word_t;

// 原地解析,返回写入 out 的条数(最多 max)。slug 恒为空。text 不必以 NUL 结尾。
size_t wb_parse_words(const char *text, size_t len, wb_parsed_word_t *out, size_t max);

// application/x-www-form-urlencoded:原地解码 '+' 与 %XX,返回解码后长度并补 NUL。
size_t wb_url_decode(char *s, size_t len);

// 在已解码前的表单 body 中找 key=value,把 value 复制到 out 并 URL 解码。
// 找到返回解码后长度,找不到返回 -1。out 总以 NUL 结尾。
int wb_form_get(const char *body, size_t body_len, const char *key, char *out, size_t out_size);

// 词包清单(packs/out/pack.txt,由 tools/build_wordpack.py 生成):
//   #wordpack 1
//   version=<hash>
//   en|chunk|zh|slug
// version 写入 version_out(可为 NULL)。格式头不对返回 -1,否则返回卡片条数。
#define WB_PACK_VERSION_MAX 24
int wb_parse_pack(const char *text, size_t len, wb_parsed_word_t *out, size_t max,
                  char *version_out, size_t version_size);
