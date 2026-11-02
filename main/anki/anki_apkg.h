/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡组导入。.apkg（ZIP）或裸 collection.anki2/anki21（SQLite）
 *       解析为设备内存模型；不依赖 Anki 运行时。
 * English: Anki deck import. Parses .apkg (ZIP) or a bare
 *          collection.anki2/anki21 (SQLite) into an in-memory model; no Anki
 *          runtime required.
 *
 * 冻结：只读源文件；v2（col JSON）与 v3（notetypes 表）两种 schema 都支持；
 * 过滤动态卡组；容量有界，超出返回错误。
 * Frozen: read-only sources; both v2 (col JSON) and v3 (notetypes tables)
 * schemas; dynamic decks skipped; bounded capacity with errors on overflow.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ANKI_MAX_FIELDS 32
#define ANKI_FIELD_NAME_MAX 64
#define ANKI_TEMPLATE_NAME_MAX 64
#define ANKI_MAX_TEMPLATES 16
#define ANKI_DECK_NAME_MAX 96
#define ANKI_NOTE_FIELDS_MAX (48U * 1024U)
#define ANKI_IMPORT_NOTE_MAX 100000
#define ANKI_IMPORT_CARD_MAX 200000
#define ANKI_IMPORT_TEMPLATE_MAX 8192

typedef struct {
    int64_t id;
    char name[ANKI_DECK_NAME_MAX];
} anki_deck_info_t;

typedef struct {
    char name[ANKI_TEMPLATE_NAME_MAX];
    char* qfmt; // 模块持有 / owned by the module
    char* afmt;
} anki_template_t;

typedef struct {
    int64_t mid;
    char name[ANKI_TEMPLATE_NAME_MAX];
    char fields[ANKI_MAX_FIELDS][ANKI_FIELD_NAME_MAX];
    int field_count;
    anki_template_t templates[ANKI_MAX_TEMPLATES];
    int template_count;
} anki_notetype_t;

typedef struct {
    int64_t id;
    int64_t mid;
    char* flds; // \x1f 分隔的字段原文 / raw fields joined by \x1f
} anki_note_t;

typedef struct {
    int64_t id;
    int64_t nid;
    int64_t did;
    int ord;
    int type;   // 0 新卡 / new, 1 学习中 / learning, 2 复习 / review
    int queue;  // 0 新 / new, 1 学习 / learning, 2 复习 / review, 3 日内学习 / day learning
    int64_t due;
    int ivl;
    int factor;
    int reps;
    int lapses;
} anki_card_t;

typedef struct {
    anki_deck_info_t* decks;
    int deck_count;
    anki_notetype_t* notetypes;
    int notetype_count;
    anki_note_t* notes;
    int note_count;
    anki_card_t* cards;
    int card_count;
    /// 1 = 复习卡的 due 是 Unix 日（v2 调度器/v3 schema）；0 = 相对集合创建日（v1）。
    /// 1 = review due is a Unix day (v2 scheduler / v3 schema); 0 = days since collection creation (v1).
    int due_epoch_day;
    /// 集合创建日（Unix 日），仅 v1 调度器的 due 重映射需要。
    /// Collection creation day (Unix day); only the v1 scheduler due remap needs it.
    int64_t crt_day;
} anki_import_t;

/// 导入 .apkg 或裸 SQLite 集合；out 须已清零。成功时交出所有权，失败清零。
/// Import an .apkg or a bare SQLite collection; out must be zeroed. Ownership
/// transfers on success; out is cleared on failure.
esp_err_t anki_apkg_import(const char* path, anki_import_t* out);

/// 释放导入结果的全部缓冲区，允许 NULL。/ Release every buffer of an import
/// result; accepts NULL.
void anki_apkg_free(anki_import_t* result);

/// 按 id 找笔记，未找到返回 NULL。/ Find a note by id; NULL when absent.
const anki_note_t* anki_apkg_note(const anki_import_t* imp, int64_t nid);
/// 按 mid 找笔记类型，未找到返回 NULL。/ Find a notetype by mid; NULL when absent.
const anki_notetype_t* anki_apkg_notetype(const anki_import_t* imp, int64_t mid);
