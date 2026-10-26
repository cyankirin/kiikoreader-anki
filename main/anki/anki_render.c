/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：模板渲染实现。有界输出构建器 + 字段查找 + 条件段 + cloze 处理，
 *       最后复用 html_to_blocks 得到块文本；选择题模型另走专用版面
 *       （正面：元信息 / 【题型】题干 / 选项带字母；背面再加错项 \x01、
 *       答案与备注），模板里只写在属性上的字母与隐藏占位条目不进正文。
 * English: Template rendering. Bounded output builder, field lookup,
 *          conditional sections and cloze handling, then html_to_blocks for
 *          block text; choice models take dedicated layouts (front: meta line,
 *          【type】question, lettered options; back: the same plus \x01 on
 *          wrong options, the answer and the note), and letters that only
 *          lived in template attributes or hidden placeholders never reach the
 *          text.
 */
#include "anki_render.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "html_text.h"

#define TAG "anki_render"

#define HTML_BUILD_MAX (48U * 1024U)
#define FIELD_VALUE_MAX (16U * 1024U)

typedef struct {
    char* out;
    size_t len;
    size_t cap;
    bool overflow;
} builder_t;

static void put(builder_t* b, const char* text, size_t len) {
    if (b->overflow || !text || !len) return;
    if (b->len + len >= b->cap) {
        b->overflow = true;
        return;
    }
    memcpy(b->out + b->len, text, len);
    b->len += len;
}

static void puts_c(builder_t* b, const char* text) { put(b, text, strlen(text)); }

/// 有界追加：超长按现有策略截断——保留已写内容，始终给结尾 NUL 留位。
/// Bounded append: overflow truncates like the existing strategy, keeping
/// what fit and always leaving room for the terminating NUL.
static void put_trunc(builder_t* b, const char* text, size_t len) {
    if (!b || !text || !len || b->len + 1 >= b->cap) return;
    size_t room = b->cap - b->len - 1;
    size_t n = len < room ? len : room;
    memcpy(b->out + b->len, text, n);
    b->len += n;
}

typedef struct {
    const anki_notetype_t* nt;
    const char* fields[ANKI_MAX_FIELDS]; // 借自 flds 切分 / borrowed from the flds split
    size_t field_lens[ANKI_MAX_FIELDS];
    int field_count;
    const char* front_text;
    int active_cloze; // 0 表示全部按未激活 / 0 means all clozes inactive
    /// 背面渲染：激活项揭示答案。/ Back side: the active cloze is revealed.
    bool reveal_cloze;
} render_ctx_t;

/// 按名字查字段（精确优先，大小写回退）。/ Look up a field by name (exact first,
/// case-insensitive fallback).
static const char* field_value(const render_ctx_t* rc, const char* name, size_t name_len,
                               size_t* value_len) {
    for (int i = 0; i < rc->field_count; ++i) {
        if (strlen(rc->nt->fields[i]) == name_len &&
            memcmp(rc->nt->fields[i], name, name_len) == 0) {
            *value_len = rc->field_lens[i];
            return rc->fields[i];
        }
    }
    for (int i = 0; i < rc->field_count; ++i) {
        if (strlen(rc->nt->fields[i]) == name_len &&
            strncasecmp(rc->nt->fields[i], name, name_len) == 0) {
            *value_len = rc->field_lens[i];
            return rc->fields[i];
        }
    }
    *value_len = 0;
    return NULL;
}

/// 按索引取上下文字段；越界（flds 段数少于模型字段数）返回 NULL。
/// Fetch a context field by index; NULL when out of range (fewer flds parts
/// than the model declares).
static const char* ctx_field(const render_ctx_t* rc, int index, size_t* value_len) {
    if (index < 0 || index >= rc->field_count) {
        *value_len = 0;
        return NULL;
    }
    *value_len = rc->field_lens[index];
    return rc->fields[index];
}

/// 处理字段文本里的 cloze 标记；active 为激活序号（0 = 全未激活）。
/// 激活项是这张卡要考的那个：正面挖空成 […]，背面揭示其答案；其余序号始终
/// 显示原文（Anki 的行为）。reveal 为 true 表示正在渲染背面。
/// Process cloze markers inside field text; active is the active index (0 = none).
/// The active index is what this card asks: it is blanked as […] on the front
/// and revealed on the back, while every other index keeps its source text
/// (Anki behaviour). reveal means the back side is being rendered.
static void emit_cloze_field(builder_t* b, const char* text, size_t len, int active,
                             bool reveal) {
    size_t i = 0;
    while (i < len) {
        const char* open = memchr(text + i, '{', len - i);
        if (!open) {
            put(b, text + i, len - i);
            return;
        }
        size_t at = (size_t)(open - text);
        if (at + 2 > len || open[1] != '{') {
            put(b, text + i, at + 1 - i);
            i = at + 1;
            continue;
        }
        // 形如 {{cN::text}} 或 {{cN::text::hint}}。/ Shape: {{cN::text}} or {{cN::text::hint}}.
        const char* p = open + 2;
        if ((size_t)(p - text) < len && *p == 'c') {
            const char* digits = p + 1;
            const char* d = digits;
            while (d < text + len && *d >= '0' && *d <= '9') ++d;
            if (d > digits && (size_t)(d - text) + 1 < len && d[0] == ':' && d[1] == ':') {
                long n = strtol(digits, NULL, 10);
                const char* close = NULL;
                for (const char* q = d + 2; q + 1 < text + len; ++q) {
                    if (q[0] == '}' && q[1] == '}') {
                        close = q;
                        break;
                    }
                }
                if (close) {
                    put(b, text + i, at - i); // 标记前的原文 / text before the marker
                    const char* body = d + 2;
                    size_t body_len = (size_t)(close - body);
                    // 取 :: 前的主体，丢掉 hint。/ Body before ::, hint dropped.
                    const char* sep = memchr(body, ':', body_len);
                    if (sep) body_len = (size_t)(sep - body);
                    if (n == (long)active && !reveal) {
                        puts_c(b, "[…]"); // 正面挖空 / blanked on the front
                    } else {
                        put(b, body, body_len); // 背面揭示或本就未激活 / revealed or inactive
                    }
                    i = (size_t)(close - text) + 2;
                    continue;
                }
            }
        }
        put(b, text + i, at + 2 - i);
        i = at + 2;
    }
}

static void emit_field(builder_t* b, const render_ctx_t* rc, const char* name, size_t name_len) {
    if (name_len == 9 && memcmp(name, "FrontSide", 9) == 0) {
        if (rc->front_text) puts_c(b, rc->front_text);
        return;
    }
    if (name_len == 4 && memcmp(name, "Tags", 4) == 0) return;   // 标签未导入 / tags not imported
    if (name_len == 4 && memcmp(name, "Deck", 4) == 0) return;   // 卡组名由界面给出 / deck name comes from the UI
    if (name_len == 4 && memcmp(name, "Card", 4) == 0) return;
    if (name_len == 8 && memcmp(name, "Subtitle", 8) == 0) return;
    if (name_len >= 5 && memcmp(name, "type:", 5) == 0) return;  // 输入域不支持 / inputs unsupported
    if (name_len >= 6 && memcmp(name, "cloze:", 6) == 0) {
        size_t value_len = 0;
        const char* value = field_value(rc, name + 6, name_len - 6, &value_len);
        if (value) emit_cloze_field(b, value, value_len, rc->active_cloze, rc->reveal_cloze);
        return;
    }
    if (name_len >= 5 && memcmp(name, "hint:", 5) == 0) {
        size_t value_len = 0;
        const char* value = field_value(rc, name + 5, name_len - 5, &value_len);
        if (value) put(b, value, value_len); // 电纸屏无悬停，直接给提示文本 / no hover on e-paper
        return;
    }
    size_t value_len = 0;
    const char* value = field_value(rc, name, name_len, &value_len);
    // 普通引用全部未激活：显示原文。/ Plain refs are never active: source text.
    if (value) emit_cloze_field(b, value, value_len, 0, rc->reveal_cloze);
}

/// 找与 name 匹配的 {{/name}} 结束标记，返回其内容区间。/ Find the matching
/// {{/name}} end marker and return the inner span.
static bool find_section(const char* tpl, size_t len, size_t from, const char* name,
                         size_t name_len, size_t* inner_start, size_t* inner_end,
                         size_t* after_end) {
    char closer[ANKI_FIELD_NAME_MAX + 8];
    if (name_len + 4 >= sizeof(closer)) return false;
    memcpy(closer, "{{/", 3);
    memcpy(closer + 3, name, name_len);
    memcpy(closer + 3 + name_len, "}}", 2);
    closer[3 + name_len + 2] = 0;
    for (size_t i = from; i + 3 + name_len + 2 <= len; ++i) {
        if (memcmp(tpl + i, closer, 3 + name_len + 2) == 0) {
            *inner_start = from;
            *inner_end = i;
            *after_end = i + 3 + name_len + 2;
            return true;
        }
    }
    return false;
}

static void substitute(render_ctx_t* rc, builder_t* b, const char* tpl, size_t len) {
    size_t i = 0;
    while (i < len && !b->overflow) {
        char c = tpl[i];
        if (c != '{' || i + 1 >= len || tpl[i + 1] != '{') {
            put(b, tpl + i, 1);
            ++i;
            continue;
        }
        const char* close = NULL;
        for (size_t q = i + 2; q + 1 < len; ++q) {
            if (tpl[q] == '}' && tpl[q + 1] == '}') {
                close = tpl + q;
                break;
            }
        }
        if (!close) {
            put(b, tpl + i, 1);
            ++i;
            continue;
        }
        size_t content_len = (size_t)(close - (tpl + i + 2));
        const char* content = tpl + i + 2;
        i = (size_t)(close - tpl) + 2;

        if (content_len == 0) continue; // {{}}：空名，忽略 / empty name, ignored
        if (content[0] == '/') continue; // 结束标记由条件段消费 / end markers consumed by sections
        if (content[0] == '#' || content[0] == '^') {
            const char* name = content + 1;
            size_t name_len = content_len - 1;
            size_t value_len = 0;
            const char* value = field_value(rc, name, name_len, &value_len);
            bool present = value && value_len > 0;
            bool wanted = content[0] == '#' ? present : !present;
            size_t inner_start = 0, inner_end = 0, after_end = 0;
            if (wanted && find_section(tpl, len, i, name, name_len, &inner_start, &inner_end,
                                       &after_end)) {
                substitute(rc, b, tpl + inner_start, inner_end - inner_start);
                i = after_end;
            } else if (find_section(tpl, len, i, name, name_len, &inner_start, &inner_end,
                                    &after_end)) {
                i = after_end; // 条件不成立，整段跳过 / condition false, skip the section
            }
            continue;
        }
        emit_field(b, rc, content, content_len);
    }
}

/// 把 flds（\x1f 分隔）切进上下文。/ Split the \x1f-joined flds into the context.
static void split_fields(render_ctx_t* rc, const char* flds, size_t len, const char* nt_fields,
                         int nt_field_count) {
    int index = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && index < ANKI_MAX_FIELDS; ++i) {
        if (i == len || flds[i] == '\x1f') {
            rc->fields[index] = flds + start;
            rc->field_lens[index] = i - start;
            ++index;
            start = i + 1;
        }
    }
    rc->field_count = index;
    // 只保留笔记类型声明的字段数，多出来的忽略。/ Keep only declared fields.
    if (nt_field_count > 0 && rc->field_count > nt_field_count) rc->field_count = nt_field_count;
    (void)nt_fields;
}

/// HTML -> 纯文本：块间换行，图片块占位。/ HTML to plain text: newline between
/// blocks, placeholder for image blocks.
static esp_err_t html_to_text(const char* html, size_t len, char* out, size_t cap) {
    html_text_t text = {0};
    esp_err_t err = html_to_blocks(html, len, &text);
    if (err != ESP_OK) return err;
    size_t used = 0;
    for (size_t i = 0; i < text.count; ++i) {
        const blk_t* block = &text.blocks[i];
        const char* piece;
        size_t piece_len;
        if (block->image >= 0) {
            piece = "[图片]";
            piece_len = 4;
        } else {
            piece = text.utf8 + block->offset;
            piece_len = block->len;
        }
        bool need_newline = used > 0;
        if (need_newline + used + piece_len + 1 > cap) {
            html_text_free(&text);
            return ESP_ERR_INVALID_SIZE;
        }
        if (need_newline) out[used++] = '\n';
        memcpy(out + used, piece, piece_len);
        used += piece_len;
    }
    out[used] = 0;
    html_text_free(&text);
    return ESP_OK;
}

/// 视图内按 nid 找笔记。/ Find a note by nid within the view.
static const anki_note_t* view_note(const anki_collection_t* col, int64_t nid) {
    for (int i = 0; i < col->note_count; ++i) {
        if (col->notes[i].id == nid) return &col->notes[i];
    }
    return NULL;
}

/// 视图内按 mid 找笔记类型。/ Find a notetype by mid within the view.
static const anki_notetype_t* view_notetype(const anki_collection_t* col, int64_t mid) {
    for (int i = 0; i < col->notetype_count; ++i) {
        if (col->notetypes[i].mid == mid) return &col->notetypes[i];
    }
    return NULL;
}

/// 选择题版面：选项字段（单字母字段名）与答案字段的位置。
/// Choice layout: where the option fields (single-letter names) and the
/// answer field live.
typedef struct {
    int option_field[ANKI_MAX_FIELDS];   ///< 选项字段索引 / option field indices
    char option_letter[ANKI_MAX_FIELDS]; ///< 选项字母（大写）/ option letters, uppercased
    int option_count;                    ///< 选项数量 / option count
    int answer_field;                    ///< 答案字段索引 / answer field index
    int question_field;                  ///< 题干字段索引 / question field index
    int note_field;                      ///< 注释字段索引，-1 表示无 / note field index, -1 when absent
} choice_layout_t;

/// 单字母字段名（大小写不敏感）的大写形式；非单 ASCII 字母返回 0。
/// Uppercase form of a single-letter field name (case-insensitive); 0 when
/// the name is not a single ASCII letter.
static char single_letter_field(const char* name) {
    if (!name || name[0] == 0 || name[1] != 0) return 0;
    char c = name[0];
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c;
    return 0;
}

/// 该字段索引是否已是选项字段。/ Whether the index is already an option field.
static bool choice_is_option(const choice_layout_t* layout, int index) {
    for (int i = 0; i < layout->option_count; ++i) {
        if (layout->option_field[i] == index) return true;
    }
    return false;
}

/// 识别选择题模型：选项字段名是单个 ASCII 字母，且字母构成从 'A' 起的连续段
/// （A、B、C、D…，数量 ≥2）；另有非选项字段，其值 trim+大写后非空且每个
/// 字符都是选项字母（如 "AC"）→ 答案字段。题干取名为 "Q" 的字段（找不到则
/// 退化为第一个非选项非答案字段），注释取名为 "N" 的字段（没有则 -1）。
/// 任一条件不满足 → 非选择题，返回 false（现有行为不变）。
/// Detect a choice model: option field names are single ASCII letters forming
/// a consecutive run from 'A' (A, B, C, D, …, at least two); a non-option
/// field whose trimmed, uppercased value is non-empty and made only of option
/// letters (e.g. "AC") is the answer field. The question is the field named
/// "Q" (falling back to the first non-option, non-answer field) and the note
/// the field named "N" (-1 when absent). Any miss means not a choice card
/// and returns false (existing behaviour unchanged).
static bool choice_layout(const anki_collection_t* col, const anki_card_t* card,
                          choice_layout_t* out) {
    if (!col || !card || !out) return false;
    const anki_note_t* note = view_note(col, card->nid);
    if (!note) return false;
    const anki_notetype_t* nt = view_notetype(col, note->mid);
    if (!nt || nt->field_count <= 0) return false;

    out->option_count = 0;
    out->answer_field = -1;
    out->question_field = -1;
    out->note_field = -1;

    // 真实选择题模型（如「业余无线电新版」：I/W/J/P/Q/A/B/C/D/T/X/Y/Z/F/N）
    // 里绝大多数字段名同样是单字母，若把全部单字母字段都当选项，则整表皆
    // 选项、反而找不到「另一字段」充当答案。选项字母因此取从 'A' 起的连续
    // 段：A、B、C、D 在而 E 不在时，选项恰为 A–D，与设计示例 "A".."D" 一致。
    // In real choice models (e.g. the amateur-radio notetype with fields
    // I/W/J/P/Q/A/B/C/D/T/X/Y/Z/F/N) most names are single letters too;
    // treating every single-letter field as an option would make the whole
    // table options and leave no "other" field for the answer. Option
    // letters therefore take the consecutive run from 'A': with A, B, C, D
    // present and E absent the options are exactly A–D, matching the
    // design's "A".."D" example.
    bool present[26] = {false};
    for (int i = 0; i < nt->field_count; ++i) {
        char c = single_letter_field(nt->fields[i]);
        if (c) present[c - 'A'] = true;
    }
    int run = 0;
    while (run < 26 && present[run]) ++run;
    if (run < 2) return false; // 选项不足两个 / fewer than two options

    // 按字段顺序记录选项字段与字母。/ Record option fields and letters in field order.
    for (int i = 0; i < nt->field_count && out->option_count < ANKI_MAX_FIELDS; ++i) {
        char c = single_letter_field(nt->fields[i]);
        if (!c || c - 'A' >= run) continue;
        out->option_field[out->option_count] = i;
        out->option_letter[out->option_count] = c;
        ++out->option_count;
    }

    // 答案字段：非选项字段中，值 trim+大写后非空且全部字符都是选项字母。
    // Answer field: among non-option fields, the first whose trimmed,
    // uppercased value is non-empty and consists only of option letters.
    render_ctx_t rc = {0};
    rc.nt = nt;
    split_fields(&rc, note->flds, strlen(note->flds), NULL, nt->field_count);
    for (int i = 0; i < rc.field_count; ++i) {
        if (choice_is_option(out, i)) continue;
        const char* v = rc.fields[i];
        size_t len = rc.field_lens[i];
        size_t s = 0;
        while (s < len && isspace((unsigned char)v[s])) ++s;
        size_t e = len;
        while (e > s && isspace((unsigned char)v[e - 1])) --e;
        if (s == e) continue; // 空值不计 / an empty value does not count
        bool subset = true;
        for (size_t k = s; k < e; ++k) {
            char c = (char)toupper((unsigned char)v[k]);
            bool is_option_letter = false;
            for (int o = 0; o < out->option_count; ++o) {
                if (out->option_letter[o] == c) {
                    is_option_letter = true;
                    break;
                }
            }
            if (!is_option_letter) {
                subset = false;
                break;
            }
        }
        if (subset) {
            out->answer_field = i;
            break;
        }
    }
    if (out->answer_field < 0) return false;

    // 题干：名为 "Q" 的字段，找不到则退化为第一个非选项非答案字段。
    // Question: the field named "Q", else the first non-option, non-answer field.
    for (int i = 0; i < nt->field_count; ++i) {
        if (choice_is_option(out, i) || i == out->answer_field) continue;
        if (strcasecmp(nt->fields[i], "Q") == 0) {
            out->question_field = i;
            break;
        }
    }
    if (out->question_field < 0) {
        for (int i = 0; i < nt->field_count; ++i) {
            if (choice_is_option(out, i) || i == out->answer_field) continue;
            out->question_field = i;
            break;
        }
    }
    // 注释：名为 "N" 的字段，没有则保持 -1。/ Note: the field named "N", else -1.
    for (int i = 0; i < nt->field_count; ++i) {
        if (choice_is_option(out, i) || i == out->answer_field) continue;
        if (strcasecmp(nt->fields[i], "N") == 0) {
            out->note_field = i;
            break;
        }
    }
    return true;
}

/// 把一个字段的 HTML 洗成纯文本追加进去；空字段或清洗失败时跳过。
/// Clean one field's HTML to plain text and append it; empty fields and
/// cleaning failures are skipped.
static void put_field_text(builder_t* b, char* buf, const char* value, size_t len) {
    if (!value || len == 0) return;
    if (html_to_text(value, len, buf, FIELD_VALUE_MAX) != ESP_OK) return;
    put_trunc(b, buf, strlen(buf));
}

/// 选择题题型字段：约定名为 "W"（业余无线电新版的模型），值为「单选」/「多选」。
/// 找不到时返回 -1，正面背面都不加【】前缀。
/// The choice type field: conventionally named "W" (the amateur-radio model)
/// with values 单选 / 多选. Returns -1 when absent; neither side then carries
/// a bracket prefix.
static int choice_type_field(const anki_notetype_t* nt, const choice_layout_t* layout) {
    for (int i = 0; i < nt->field_count; ++i) {
        if (choice_is_option(layout, i) || i == layout->answer_field) continue;
        if (strcasecmp(nt->fields[i], "W") == 0) return i;
    }
    return -1;
}

/// 追加「【题型】」前缀：题型字段为空或清洗失败时不加，题型只取第一行并限长。
/// Append the "【type】" prefix; skipped when the field is empty or cannot be
/// cleaned. Only the first line is used and its length is capped.
static void put_type_prefix(builder_t* b, const render_ctx_t* rc, int type_field, char* buf) {
    if (type_field < 0) return;
    size_t len = 0;
    const char* value = ctx_field(rc, type_field, &len);
    if (!value || len == 0) return;
    if (html_to_text(value, len, buf, FIELD_VALUE_MAX) != ESP_OK) return;
    size_t n = 0;
    while (buf[n] && buf[n] != '\n') ++n;
    size_t start = 0;
    while (start < n && (buf[start] == ' ' || buf[start] == '\t')) ++start;
    while (n > start && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) --n;
    // 限长后退回字符边界，不切断多字节字符。
    // Cap the length and back off to a character boundary.
    if (n - start > 12) {
        n = start + 12;
        while (n > start && ((unsigned char)buf[n] & 0xC0) == 0x80) --n;
    }
    if (n == start) return;
    put_trunc(b, "【", strlen("【"));
    put_trunc(b, buf + start, n - start);
    put_trunc(b, "】", strlen("】"));
}

/// 追加题干之外的元信息行：模板里是 `<small>{{I}} / {{J}} / {{P}}</small>`
/// （题号、来源与章节号）。按这三个字段名取，空字段跳过，三个都空时不加这一行；
/// 其余字段（如 X/Y/Z 那类 TRUE/FALSE 标记）不进版面。
/// Append the meta line: the template spelled it
/// `<small>{{I}} / {{J}} / {{P}}</small>` (card id, source, section). The three
/// names are looked up directly, empty ones are skipped, and the line vanishes
/// when all three are empty; other fields (the TRUE/FALSE flags and the like)
/// stay out of the layout.
static void put_meta_line(builder_t* b, const render_ctx_t* rc, char* buf) {
    static const char* const k_meta_fields[] = {"I", "J", "P"};
    bool first = true;
    for (size_t f = 0; f < sizeof(k_meta_fields) / sizeof(k_meta_fields[0]); ++f) {
        size_t len = 0;
        const char* value =
            field_value(rc, k_meta_fields[f], strlen(k_meta_fields[f]), &len);
        if (!value || len == 0) continue;
        if (html_to_text(value, len, buf, FIELD_VALUE_MAX) != ESP_OK) continue;
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
        size_t start = 0;
        while (buf[start] == ' ' || buf[start] == '\t') ++start;
        if (!buf[start]) continue;
        // 单个字段限长，同样退回字符边界。
        // Cap one field, again on a character boundary.
        if (n - start > 40) {
            n = start + 40;
            while (n > start && ((unsigned char)buf[n] & 0xC0) == 0x80) --n;
            buf[n] = 0;
        }
        for (char* p = buf + start; *p; ++p) {
            if (*p == '\n') *p = ' ';
        }
        if (!first) put_trunc(b, " / ", 3);
        put_trunc(b, buf + start, n - start);
        first = false;
    }
    if (!first) put_trunc(b, "\n", 1);
}

/// 选择题正面：与背面同构但不显示答案——元信息行（模板里有题号/来源字段时）、
/// 第 1 行 `【题型】<题干>`、空行、每个选项一行 `<字母>. <选项纯文本>`。
/// 模板里的隐藏占位条目（如 `<li class="hidden">placeholder</li>`）与脚本都不进
/// 正文；正面不产生 \x01。
/// Choice front: the back's shape without the answer — the meta line (when the
/// template carries card-id or source fields), `【type】<question>` as the first
/// line, a blank line, then one `<letter>. <option text>` line per option.
/// Hidden placeholder items (`<li class="hidden">placeholder</li>`) and scripts
/// never reach the text, and the front never emits \x01.
static esp_err_t render_choice_front(const anki_collection_t* col, const anki_card_t* card,
                                     const choice_layout_t* layout, char* out, size_t cap) {
    const anki_note_t* note = view_note(col, card->nid);
    if (!note) return ESP_ERR_NOT_FOUND;
    const anki_notetype_t* nt = view_notetype(col, note->mid);
    if (!nt) return ESP_ERR_NOT_FOUND;

    render_ctx_t rc = {0};
    rc.nt = nt;
    split_fields(&rc, note->flds, strlen(note->flds), NULL, nt->field_count);

    // 字段 HTML→文本的临时缓冲走堆：内部 RAM 静态区已近满，不给 BSS 添 16KB。
    // The field HTML-to-text scratch buffer is heap-allocated: internal RAM
    // is nearly full of statics, so BSS must not grow by 16KB.
    char* buf = malloc(FIELD_VALUE_MAX);
    if (!buf) return ESP_ERR_NO_MEM;

    builder_t b = {.out = out, .len = 0, .cap = cap, .overflow = false};
    const int type_field = choice_type_field(nt, layout);

    // 元信息行：模板把它放在最前面，这里同样。/ The meta line comes first, as in the template.
    put_meta_line(&b, &rc, buf);

    // 第 1 行：【题型】题干。/ First line: 【type】question.
    size_t q_len = 0;
    const char* question = ctx_field(&rc, layout->question_field, &q_len);
    put_type_prefix(&b, &rc, type_field, buf);
    put_field_text(&b, buf, question, q_len);
    put_trunc(&b, "\n\n", 2);

    // 选项行：字母来自字段顺序，模板里的 id 属性本来就不出文本。
    // Option lines: the letter comes from the field order; the template only
    // ever put it in an id attribute, which is why the front had no letters.
    for (int i = 0; i < layout->option_count; ++i) {
        size_t opt_len = 0;
        const char* opt = ctx_field(&rc, layout->option_field[i], &opt_len);
        if (!opt) continue;
        char head[3] = {layout->option_letter[i], '.', ' '};
        put_trunc(&b, head, sizeof(head));
        put_field_text(&b, buf, opt, opt_len);
        put_trunc(&b, "\n", 1);
    }

    out[b.len] = 0;
    free(buf);
    return ESP_OK;
}

/// 选择题背面：不走通用 HTML 模板路径，直接按字段拼装纯文本（行 = '\n'
/// 分隔）：第 1 行题干；空行分层；每个选项一行 `<字母>. <选项纯文本>`，
/// 错误选项行首加强调标记 \x01（正确项保持常规黑字，答案陈述也常规）；
/// 空行分层后是一行 `答案：<T 的原值>`；注释字段非空时追加 `备注：<N>`。
/// 输出受出参容量约束，超长截断并保证 NUL 结尾。
/// Choice back: bypass the generic HTML template path and assemble plain
/// text from fields (lines separated by '\n'): the question first; a blank
/// line for hierarchy; one line per option as `<letter>. <option text>` with
/// the \x01 emphasis marker on wrong options (correct options keep ordinary
/// black ink and so does the answer statement); another blank line; one line
/// `答案：<raw T>`; and `备注：<N>` when the note field is non-empty. Output
/// respects the caller's cap: overflow truncates and the result stays
/// NUL-terminated.
static esp_err_t render_choice_back(const anki_collection_t* col, const anki_card_t* card,
                                    const choice_layout_t* layout, char* out, size_t cap) {
    const anki_note_t* note = view_note(col, card->nid);
    if (!note) return ESP_ERR_NOT_FOUND;
    const anki_notetype_t* nt = view_notetype(col, note->mid);
    if (!nt) return ESP_ERR_NOT_FOUND;

    render_ctx_t rc = {0};
    rc.nt = nt;
    split_fields(&rc, note->flds, strlen(note->flds), NULL, nt->field_count);

    // 字段 HTML→文本的临时缓冲走堆：内部 RAM 静态区已近满，不给 BSS 添 16KB。
    // The field HTML-to-text scratch buffer is heap-allocated: internal RAM
    // is nearly full of statics, so BSS must not grow by 16KB.
    char* buf = malloc(FIELD_VALUE_MAX);
    if (!buf) return ESP_ERR_NO_MEM;

    builder_t b = {.out = out, .len = 0, .cap = cap, .overflow = false};
    const int type_field = choice_type_field(nt, layout);

    // 答案原文：判定正确选项按 trim+大写逐个字母比对，答案行显示原值。
    // The raw answer: the correct-option test compares letters trimmed and
    // uppercased, while the answer line shows the original value.
    size_t answer_len = 0;
    const char* answer = ctx_field(&rc, layout->answer_field, &answer_len);

    // 第 1 行：【题型】题干（与正面一致，两侧都能看到单选/多选），复用现有
    // HTML→文本清洗。/ First line: 【type】question, matching the front so both
    // sides show 单选/多选, reusing the existing HTML-to-text cleaning.
    size_t q_len = 0;
    const char* question = ctx_field(&rc, layout->question_field, &q_len);
    put_type_prefix(&b, &rc, type_field, buf);
    put_field_text(&b, buf, question, q_len);
    // 题干与选项之间留一空行：同字号下靠间距分层，不靠更小的字号。
    // A blank line separates the question from the options: the hierarchy
    // comes from spacing, not from a smaller size.
    put_trunc(&b, "\n\n", 2);

    // 选项行：`<字母>. <选项纯文本>`，错误选项行首加 \x01。
    // Option lines: `<letter>. <option text>`, wrong ones prefixed with \x01.
    for (int i = 0; i < layout->option_count; ++i) {
        size_t opt_len = 0;
        const char* opt = ctx_field(&rc, layout->option_field[i], &opt_len);
        if (!opt) continue;
        bool correct = false;
        for (size_t k = 0; k < answer_len && !correct; ++k) {
            if ((char)toupper((unsigned char)answer[k]) == layout->option_letter[i]) {
                correct = true;
            }
        }
        // 强调标记落在错误选项上：正确项与答案陈述保持常规黑字，墨水屏上
        // 白字反色看不清，错的才需要被抹掉。
        // The emphasis marker lands on wrong options: correct ones and the
        // answer statement keep ordinary black ink, since inverted white on
        // e-paper is hard to read and only the wrong ones need muting.
        if (!correct) put_trunc(&b, "\x01", 1);
        char head[3] = {layout->option_letter[i], '.', ' '};
        put_trunc(&b, head, sizeof(head));
        put_field_text(&b, buf, opt, opt_len);
        put_trunc(&b, "\n", 1);
    }

    // 选项与答案之间同样留一空行。/ One blank line separates options from the answer.
    put_trunc(&b, "\n", 1);

    // 答案行：陈述本身不加强调标记。/ Answer line: the statement carries no marker.
    put_trunc(&b, "答案：", strlen("答案："));
    put_field_text(&b, buf, answer, answer_len);
    put_trunc(&b, "\n", 1);

    // 备注行：注释字段非空时才追加。/ Note line: only when the note field is non-empty.
    size_t note_len = 0;
    const char* note_text = ctx_field(&rc, layout->note_field, &note_len);
    if (note_text && note_len > 0) {
        put_trunc(&b, "备注：", strlen("备注："));
        put_field_text(&b, buf, note_text, note_len);
        put_trunc(&b, "\n", 1);
    }

    out[b.len] = 0;
    free(buf);
    return ESP_OK;
}

static esp_err_t render_card(const anki_collection_t* col, const anki_card_t* card, bool back,
                             const char* front_text, char* out, size_t cap) {
    const anki_note_t* note = view_note(col, card->nid);
    if (!note) return ESP_ERR_NOT_FOUND;
    const anki_notetype_t* nt = view_notetype(col, note->mid);
    if (!nt || nt->template_count == 0) return ESP_ERR_NOT_FOUND;
    const anki_template_t* tpl = &nt->templates[card->ord < nt->template_count ? card->ord : 0];
    const char* format = back ? tpl->afmt : tpl->qfmt;
    if (!format) return ESP_ERR_NOT_FOUND;

    // 中间 HTML 走堆：内部 RAM 静态占用已近满，不给 BSS 添 48KB。
    // The intermediate HTML is heap-allocated: internal RAM is nearly full of
    // statics, so BSS must not grow by 48KB.
    char* html = malloc(HTML_BUILD_MAX);
    if (!html) return ESP_ERR_NO_MEM;
    builder_t builder = {.out = html, .len = 0, .cap = HTML_BUILD_MAX, .overflow = false};
    render_ctx_t rc = {0};
    rc.nt = nt;
    rc.front_text = front_text;
    // cloze 卡的激活序号为 ord+1，即这张卡要考的那个；普通卡全部未激活。
    // Cloze cards activate ord+1, the index this card asks about; other cards
    // leave every cloze inactive.
    rc.active_cloze = card->ord + 1;
    rc.reveal_cloze = back;
    split_fields(&rc, note->flds, strlen(note->flds), NULL, nt->field_count);
    substitute(&rc, &builder, format, strlen(format));
    esp_err_t err = ESP_OK;
    if (builder.overflow) {
        ESP_LOGW(TAG, "template output overflow");
        err = ESP_ERR_INVALID_SIZE;
    } else {
        html[builder.len] = 0;
        err = html_to_text(html, builder.len, out, cap);
    }
    free(html);
    return err;
}

void anki_collection_from_import(const anki_import_t* imp, anki_collection_t* out) {
    if (!imp || !out) return;
    out->notetypes = imp->notetypes;
    out->notetype_count = imp->notetype_count;
    out->notes = imp->notes;
    out->note_count = imp->note_count;
}

esp_err_t anki_render_front(const anki_collection_t* col, const anki_card_t* card, char* out,
                            size_t cap) {
    if (!col || !card || !out || cap == 0) return ESP_ERR_INVALID_ARG;
    // 选择题模型走专用正面版面：选项带 A/B/C/D 字母，模板里只写在 id 属性上的
    // 字母与隐藏占位条目都不会进正文；其余卡组继续走模板路径，行为不变。
    // Choice models take the dedicated front layout: options carry A/B/C/D
    // letters, while the template-only id attributes and hidden placeholder
    // items never reach the text. Every other deck keeps the template path
    // and its behaviour is unchanged.
    choice_layout_t layout;
    if (choice_layout(col, card, &layout)) {
        return render_choice_front(col, card, &layout, out, cap);
    }
    return render_card(col, card, false, NULL, out, cap);
}

esp_err_t anki_render_back(const anki_collection_t* col, const anki_card_t* card,
                           const char* front_text, char* out, size_t cap) {
    if (!col || !card || !out || cap == 0) return ESP_ERR_INVALID_ARG;
    // 选择题模型走专用版面：错误选项行加 \x01 强调标记（正面与此无关，
    // anki_render_front 不产生任何 \x01）。
    // Choice models take the dedicated layout: wrong option lines carry the
    // \x01 emphasis marker (the front is untouched and never emits \x01).
    choice_layout_t layout;
    if (choice_layout(col, card, &layout)) {
        return render_choice_back(col, card, &layout, out, cap);
    }
    return render_card(col, card, true, front_text, out, cap);
}
