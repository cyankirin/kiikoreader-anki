/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡片模板渲染。字段替换、条件段、cloze 与 hint，再把生成的 HTML
 *       交给 html_text 转成可排版的纯文本。
 * English: Anki card template rendering. Field substitution, conditional
 *          sections, cloze and hint, then hands the generated HTML to
 *          html_text for plain-text layout.
 *
 * 冻结：不执行脚本、不加载网络资源；type: 输入域与媒体取占位文本。
 * Frozen: never executes scripts or loads network resources; type: inputs and
 * media degrade to placeholder text.
 */
#pragma once

#include <stddef.h>

#include "anki_apkg.h"
#include "esp_err.h"

#define ANKI_CARD_TEXT_MAX (16U * 1024U)

/// 卡片内容来源：导入结果与已载入卡组共用这一视图。
/// Card content source: import results and loaded decks share this view.
typedef struct {
    const anki_notetype_t* notetypes;
    int notetype_count;
    const anki_note_t* notes;
    int note_count;
} anki_collection_t;

/// 从导入结果建视图。/ Build a view from an import result.
void anki_collection_from_import(const anki_import_t* imp, anki_collection_t* out);

/// 渲染卡片正面（问题）为纯文本；out 由调用方持有并 NUL 结尾。
/// Render the card front (question) to plain text; out is caller-owned and NUL-terminated.
esp_err_t anki_render_front(const anki_collection_t* col, const anki_card_t* card, char* out,
                            size_t cap);

/// 渲染卡片背面（答案）；front_text 为已渲染正面，用于 {{FrontSide}}。
/// Render the card back (answer); front_text is the rendered front for {{FrontSide}}.
esp_err_t anki_render_back(const anki_collection_t* col, const anki_card_t* card,
                           const char* front_text, char* out, size_t cap);
