/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：卡组设备存储。导入结果按卡组写成 .akd：TF 卡在位且可写时落
 *       /sdcard/anki，否则落 /flash/anki；读取与删除覆盖两个根（同 id 时
 *       活动根优先）。学习时整库载入 PSRAM，进度以 52 字节定长记录原地更新。
 * English: On-device deck storage. Import results are written per deck as
 *          .akd files: to /sdcard/anki when the TF card is present and
 *          writable, else to /flash/anki; reads and deletes cover both roots
 *          (the active root wins on a shared id). A study session loads the
 *          whole deck into PSRAM and progress updates patch 52-byte fixed
 *          records in place.
 *
 * 冻结：单文件有界；无 RTC 时沿用已有日计数不推进。
 * Frozen: bounded file size; without an RTC the day counter holds instead of
 * advancing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anki_apkg.h"
#include "esp_err.h"

/* 宿主替身 esp_err.h 未定义 ESP_ERR_INVALID_VERSION；真机 ESP-IDF 已定义。
   / The host esp_err stub lacks ESP_ERR_INVALID_VERSION; ESP-IDF defines it. */
#ifndef ESP_ERR_INVALID_VERSION
#define ESP_ERR_INVALID_VERSION 0x10A
#endif

#ifndef ANKI_DIR
#define ANKI_DIR "/flash/anki"
#endif
#ifdef ESP_PLATFORM
/// TF 卡上的 Anki 根，与书籍目录同卡。/ The Anki root on the TF card, same card as books.
#define ANKI_DIR_TF "/sdcard/anki"
#else
// 宿主：TF 根派生自 ANKI_DIR，双根去重/回落/清理在宿主测试里真实落地；
// ESP 上恒为 /sdcard/anki。/ Host: derive the TF root from ANKI_DIR so
// dual-root dedup/fallback/cleanup is exercised for real in host tests; on
// ESP this stays /sdcard/anki.
#define ANKI_DIR_TF ANKI_DIR "-tf"
#endif
#define ANKI_DECK_FILE_MAX (6U * 1024U * 1024U)
#define ANKI_SOURCE_PATH_MAX 256
#define ANKI_SESSION_MAX 50

/* .akd 既有字节布局：由 anki_store.c 的私有宏上提，值逐字不变，供
   anki_progress.c 只读复用，避免两处漂移。/ Lifted .akd byte layout from
   anki_store.c (values unchanged) so anki_progress.c can reuse it read-only. */
#define AKD_MAGIC "AKD1"
#define AKD_VERSION 1u
#define AKD_HEADER_BYTES 64
#define AKD_CARD_RECORD 52
#define AKD_OFF_MAGIC 0
#define AKD_OFF_VERSION 4
#define AKD_OFF_DECK_ID 8
#define AKD_OFF_DAY 16
#define AKD_OFF_LAST_RTC_DAY 24
#define AKD_OFF_CARD_COUNT 32
#define AKD_OFF_NOTE_COUNT 36
#define AKD_OFF_NOTETYPE_COUNT 40
#define AKD_OFF_NAME_LEN 44
#define AKD_OFF_SOURCE_LEN 48
#define AKD_OFF_TABLE_AT 52
#define AKD_OFF_HEADER_BYTES_FIELD 56
#define AKD_OFF_RESERVED 60
#define AKD_CARD_OFF_ID 0
#define AKD_CARD_OFF_NID 8
#define AKD_CARD_OFF_ORD 16
#define AKD_CARD_OFF_TYPE 20
#define AKD_CARD_OFF_QUEUE 24
#define AKD_CARD_OFF_DUE 28
#define AKD_CARD_OFF_IVL 32
#define AKD_CARD_OFF_FACTOR 36
#define AKD_CARD_OFF_REPS 40
#define AKD_CARD_OFF_LAPSES 44
#define AKD_CARD_OFF_RESERVED 48

/* 续学状态 sidecar .aks：定长 512 B、显式小端序列化、不写进 .akd、
   不升 AKD_VERSION。/ Resume sidecar: fixed 512 B, explicit little-endian
   serialization, never inside .akd, AKD_VERSION stays 1. */
#define ANKI_STATE_MAGIC "AKS1"
#define ANKI_STATE_VERSION 1u
#define ANKI_RESUME_BYTES 512u
#define ANKI_STATE_SUFFIX ".aks"
#define ANKI_ROOT_TAG_FLASH 0u
#define ANKI_ROOT_TAG_TF 1u
#define ANKI_FNV1A_BASIS 2166136261u

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
    /* 尾部追加：0 = 无有效续学；既有宿主测试不受影响。/ Appended at the
       tail: 0 means no valid resume state; existing host tests are unchanged. */
    int resume_pos;
    int resume_count;
} anki_deck_summary_t;

/// 已校验的续学状态快照（512 B .aks 的内存表示）。/ A validated resume snapshot.
typedef struct {
    int64_t deck_id;
    uint32_t version;
    uint32_t root_tag;
    int card_count;
    int session_count;
    int session_pos;
    int studied;
    int again;
    int good_or_better;
    int64_t day;
    uint32_t fingerprint;
    int64_t session_ids[ANKI_SESSION_MAX];
} anki_resume_t;

/// FNV-1a 32，store 与 progress 共用；头文件内联，零链接依赖。
/// FNV-1a 32 shared by store and progress; header-inline, no link dependency.
static inline uint32_t anki_fnv1a(const void* data, size_t len, uint32_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

/// 导入一个 .apkg / 裸集合：按卡组分别落盘。out_imported 收到成功卡组数。
/// 落盘根为当前活动根（TF 卡在位且可写 → /sdcard/anki，否则 /flash/anki）。
/// Import an .apkg or bare collection: one file per deck. out_imported
/// receives the number of decks written. Files land under the active root
/// (TF present and writable → /sdcard/anki, else /flash/anki).
esp_err_t anki_store_import(const char* path, int* out_imported);

/// 枚举已导入卡组；回调返回 false 提前结束。/ Enumerate imported decks; false stops early.
typedef bool (*anki_deck_visit_fn)(const anki_deck_summary_t* summary, void* ctx);
esp_err_t anki_store_list(anki_deck_visit_fn visit, void* ctx);

/// 双根解析纯函数：TF 就绪 → ANKI_DIR_TF，否则 ANKI_DIR。
/// Pure dual-root resolution: TF-ready → ANKI_DIR_TF, else ANKI_DIR.
/// 唯一输入是 tf_ready 标志，宿主测试直接验证两种映射。/ The only input is
/// the tf_ready flag; host tests verify both mappings directly.
const char* anki_root_from_tf_ready(bool tf_ready);

/// 当前活动根：TF 卡在位且可写 → ANKI_DIR_TF，否则 ANKI_DIR。
/// The active root: TF present and writable → ANKI_DIR_TF, else ANKI_DIR.
/// 宿主分支恒返回 ANKI_DIR（-DANKI_DIR 机制不变）。
/// The host branch always returns ANKI_DIR (the -DANKI_DIR mechanism is
/// unchanged).
const char* anki_store_active_root(void);

/// 整库载入；out 由 anki_store_unload 释放。先试活动根，找不到再试另一个根；
/// anki_deck_t.path 记录实际读到的路径（进度原地写回该文件）。
/// Load a whole deck; out is released by anki_store_unload. The active root
/// is tried first, then the other root; anki_deck_t.path records the path
/// actually read (progress patches that file in place).
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

/// 删除卡组文件：活动根与另一个 root 的同 id 文件都删，避免拔卡后幽灵卡组回来。
/// Delete a deck file: the same id under the active root and the other root
/// both go, so a removed card cannot leave a ghost deck behind.
esp_err_t anki_store_delete(int64_t deck_id);

/// 当前 Unix 日（UTC+8）；未对时返回 0。/ Current Unix day (UTC+8); 0 when time is unset.
int64_t anki_day_now(void);

/* ------------------------------------------------ 续学状态 .aks（新增 API） */

/// 读并校验当前 deck 的续学状态；无效/损坏/跨日/绑定不符 → 非 ESP_OK 且 out 清零。
/// Read and validate resume state; any binding mismatch leaves out zeroed.
esp_err_t anki_store_resume_load(const anki_deck_t* deck, anki_resume_t* out);

/// 由当前会话快照写 .aks（与 .akd 同根，512 B 整块一次写）；失败不致命。
/// Persist a session snapshot to .aks (one 512 B write); failure is non-fatal.
esp_err_t anki_store_resume_save(const anki_deck_t* deck,
                                 const int* session_indices, int session_count,
                                 int session_pos, int studied, int again,
                                 int good_or_better);

/// 清除两根下同 id 的 .aks；允许「无文件」；void，不阻塞。
/// Clear .aks under both roots; a missing file is fine; never blocks.
void anki_store_resume_clear(int64_t deck_id);

/// 把已校验的 card_id 队列映射回当前 deck 的卡片下标；返回有效张数，
/// 0 = 应重建（E8 静默降级）。/ Map the validated card_id queue back to deck
/// card indices; 0 means rebuild (E8 silent degradation).
int anki_store_resume_indices(const anki_deck_t* deck, const anki_resume_t* r,
                              int* out_indices, int max);

/// 轻量探测，仅供 anki_store_list 填 resume_pos/count；读失败静默置 0，
/// 绝不返回错误、绝不阻塞列表（I4）。/ Lightweight probe for the list row;
/// any failure silently yields 0 and never blocks the list (I4).
void anki_store_resume_peek(int64_t deck_id, const char* root,
                            int* out_pos, int* out_count);
