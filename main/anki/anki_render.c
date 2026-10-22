/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：模板渲染实现。有界输出构建器 + 字段查找 + 条件段 + cloze 处理，
 *       最后复用 html_to_blocks 得到块文本。
 * English: Template rendering. Bounded output builder, field lookup,
 *          conditional sections and cloze handling, then html_to_blocks for
 *          block text.
 */
#include "anki_render.h"

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
    return render_card(col, card, false, NULL, out, cap);
}

esp_err_t anki_render_back(const anki_collection_t* col, const anki_card_t* card,
                           const char* front_text, char* out, size_t cap) {
    if (!col || !card || !out || cap == 0) return ESP_ERR_INVALID_ARG;
    return render_card(col, card, true, front_text, out, cap);
}
