/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：卡组设备存储。导入结果按卡组写成 /flash/anki/<deckid>.akd；
 *       学习时整库载入 PSRAM，进度以 52 字节定长记录原地更新。
 * English: On-device deck storage. Import results are written per deck to
 *          /flash/anki/<deckid>.akd; a study session loads the whole deck into
 *          PSRAM and progress updates patch 52-byte fixed records in place.
 *
 * 冻结：只写 /flash/anki；单文件有界；无 RTC 时沿用已有日计数不推进。
 * Frozen: writes only /flash/anki; bounded file size; without an RTC the day
 * counter holds instead of advancing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anki_apkg.h"
#include "esp_err.h"

#ifndef ANKI_DIR
#define ANKI_DIR "/flash/anki"
#endif
#define ANKI_DECK_FILE_MAX (6U * 1024U * 1024U)
#define ANKI_SOURCE_PATH_MAX 256
#define ANKI_SESSION_MAX 50

/// 评分等级。/ Grade levels.
typedef enum {
    ANKI_GRADE_AGAIN = 0,
    ANKI_GRADE_HARD = 1,
    ANKI_GRADE_GOOD = 2,
    ANKI_GRADE_EASY = 3,
} anki_grade_t;

/// 已加载卡组：内容 + 进度 + 日计数，全部持有在 PSRAM。
/// A loaded deck: content plus progress and the day counter, all in PSRAM.
typedef struct {
    int64_t deck_id;
    char name[ANKI_DECK_NAME_MAX];
    char source[ANKI_SOURCE_PATH_MAX];
    int64_t day;
    int64_t last_rtc_day;
    anki_notetype_t* notetypes;
    int notetype_count;
    anki_note_t* notes;
    int note_count;
    anki_card_t* cards;
    int card_count;
    char path[64];
    /// 文件内卡片表偏移，进度原地更新用。/ In-file card table offset for progress patching.
    uint32_t card_offset;
} anki_deck_t;

/// 列表摘要：不载入整库，只读文件头与卡片表计数。
/// List summary: no full load; counts come from the header and card table.
typedef struct {
    int64_t deck_id;
    char name[ANKI_DECK_NAME_MAX];
    int total;
    int due;
    int fresh;
} anki_deck_summary_t;

/// 导入一个 .apkg / 裸集合：按卡组分别落盘。out_imported 收到成功卡组数。
/// Import an .apkg or bare collection: one file per deck. out_imported receives the number of decks written.
esp_err_t anki_store_import(const char* path, int* out_imported);

/// 枚举已导入卡组；回调返回 false 提前结束。/ Enumerate imported decks; false stops early.
typedef bool (*anki_deck_visit_fn)(const anki_deck_summary_t* summary, void* ctx);
esp_err_t anki_store_list(anki_deck_visit_fn visit, void* ctx);

/// 整库载入；out 由 anki_store_unload 释放。/ Load a whole deck; out is released by anki_store_unload.
esp_err_t anki_store_load(int64_t deck_id, anki_deck_t* out);
/// 释放载入的卡组，允许 NULL。/ Release a loaded deck; accepts NULL.
void anki_store_unload(anki_deck_t* deck);

/// 用 RTC 推进日计数（无 RTC 时保持）；返回当前日。/ Advance the day counter from the RTC (holds without one); returns the current day.
int64_t anki_store_sync_day(anki_deck_t* deck);

/// 取今日会话：先新卡后到期复习，最多 ANKI_SESSION_MAX 张。
/// Build today's session: new cards first, then due reviews, capped at ANKI_SESSION_MAX.
int anki_store_session(anki_deck_t* deck, int* out_indices, int max);

/// 应用评分并更新内存状态；随后调用方保存进度。/ Apply a grade to the in-memory card; the caller then persists progress.
void anki_schedule(anki_deck_t* deck, int card_index, anki_grade_t grade);

/// 把一张卡的进度原地写回文件。/ Persist one card's progress in place.
esp_err_t anki_store_save_card(anki_deck_t* deck, int card_index);

/// 删除卡组文件。/ Delete a deck file.
esp_err_t anki_store_delete(int64_t deck_id);

/// 当前 Unix 日（UTC+8）；未对时返回 0。/ Current Unix day (UTC+8); 0 when time is unset.
int64_t anki_day_now(void);
