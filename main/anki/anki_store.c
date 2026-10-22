/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：卡组存储实现：.akd 读写、列表计数、日计数同步、定长进度原地更新与
 *       简化 SM-2 调度。/ English: Deck storage: .akd read/write, list counts,
 *       day-counter sync, fixed-record progress patching and a simplified
 *       SM-2 scheduler.
 */
#include "anki_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "book_store.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#ifdef _WIN32
#include <direct.h>
// MinGW 的 mkdir 只收路径。/ MinGW mkdir takes a path only.
#define anki_mkdir(path) _mkdir(path)
#else
#define anki_mkdir(path) mkdir((path), 0777)
#endif

#define TAG "anki_store"

#ifndef ANKI_DIR
#define ANKI_DIR "/flash/anki"
#endif

#define AKD_MAGIC "AKD1"
#define AKD_VERSION 1u
#define CARD_RECORD 52
#define HEADER_BYTES 64

// 2024-01-01 之前的 RTC 视为未对时。/ An RTC before 2024-01-01 counts as unset.
#define ANKI_RTC_VALID_SECONDS 1704067200
// 界面时区与设备一致：UTC+8。/ The UI timezone matches the device: UTC+8.
#define ANKI_TZ_OFFSET (8 * 3600)

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    size_t at;
    bool overflow;
} cursor_t;

static void* anki_alloc(size_t n) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

/// 复制一段借来的文本为自持 NUL 结尾缓冲。必须走 PSRAM 分配：载入上千条
/// 笔记时每条 flds 都要一次小分配，普通 malloc 按
/// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 会优先吃内部 RAM，逐步耗尽后
/// 评分时的 fopen/fseek 触发 picolibc 为 FILE 初始化锁，FreeRTOS 互斥量
/// 只能在内部 RAM 分配，失败即 abort。宿主下 anki_alloc 即 malloc，语义
/// 不变。/ Copy a borrowed span into an owned NUL-terminated buffer. This
/// must go through the PSRAM allocator: loading a deck of a thousand notes
/// makes one small allocation per note's flds, and plain malloc (under
/// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384) prefers internal RAM until it
/// runs out; grading then fopen/fseek's the deck file, picolibc initializes
/// a per-FILE lock and the FreeRTOS mutex can only be allocated from
/// internal RAM — failure aborts. On the host anki_alloc is malloc, so the
/// semantics are unchanged.
static char* copy_owned(const char* src, size_t len) {
    char* out = anki_alloc(len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, src, len);
    out[len] = 0;
    return out;
}

static void cur_u32(cursor_t* c, uint32_t v) {
    if (c->overflow || c->at + 4 > c->cap) {
        c->overflow = true;
        return;
    }
    memcpy(c->data + c->at, &v, 4);
    c->at += 4;
}

static void cur_i64(cursor_t* c, int64_t v) {
    if (c->overflow || c->at + 8 > c->cap) {
        c->overflow = true;
        return;
    }
    memcpy(c->data + c->at, &v, 8);
    c->at += 8;
}

static void cur_bytes(cursor_t* c, const void* src, size_t len) {
    if (c->overflow || c->at + len > c->cap) {
        c->overflow = true;
        return;
    }
    if (len) memcpy(c->data + c->at, src, len);
    c->at += len;
}

static void cur_text(cursor_t* c, const char* text) {
    size_t len = text ? strlen(text) : 0;
    cur_u32(c, (uint32_t)len);
    cur_bytes(c, text ? text : "", len);
    cur_bytes(c, "", 1); // NUL
}

int64_t anki_day_now(void) {
    // 秒级精度足够按日比较；界面同样用 time()。/ Second precision is enough for
    // day comparison; the UI uses time() too.
    time_t now = time(NULL);
    if (now < (time_t)ANKI_RTC_VALID_SECONDS) return 0;
    return (int64_t)((now + ANKI_TZ_OFFSET) / 86400);
}

static void deck_path(int64_t deck_id, char out[64]) {
    snprintf(out, 64, "%s/%lld.akd", ANKI_DIR, (long long)deck_id);
}

static esp_err_t ensure_dir(void) {
    if (anki_mkdir(ANKI_DIR) == 0) return ESP_OK;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(ANKI_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return ESP_OK;
    }
    ESP_LOGW(TAG, "anki dir unavailable: %s (%d)", ANKI_DIR, errno);
    return ESP_FAIL;
}

// ---------------------------------------------------------------- 写盘

/// 收集某卡组引用的笔记与笔记类型。/ Collect the notes and notetypes a deck references.
typedef struct {
    const anki_import_t* imp;
    int64_t deck_id;
    int* note_slots; // 笔记下标或 -1 / note index or -1
    int* notetype_slots;
    bool failed;
} subset_t;

static void build_subset(subset_t* sub) {
    const anki_import_t* imp = sub->imp;
    // 槽位表同样走 PSRAM：1375 项的两张 int 表约 11KB，留给内部 RAM 给
    // libc 锁等小而关键的分配。ESP-IDF 的 free 对 PSRAM 与内部 RAM 统一，
    // 释放处无需改动。/ Slot tables also go to PSRAM: two int tables of
    // 1375 entries are ~11KB; keep internal RAM for small but critical
    // allocations such as libc locks. ESP-IDF free() covers PSRAM and
    // internal RAM alike, so the free sites stay untouched.
    sub->note_slots =
        anki_alloc(sizeof(int) * (size_t)(imp->note_count > 0 ? imp->note_count : 1));
    sub->notetype_slots =
        anki_alloc(sizeof(int) * (size_t)(imp->notetype_count > 0 ? imp->notetype_count : 1));
    if (!sub->note_slots || !sub->notetype_slots) {
        sub->failed = true;
        return;
    }
    for (int i = 0; i < imp->note_count; ++i) sub->note_slots[i] = -1;
    for (int i = 0; i < imp->notetype_count; ++i) sub->notetype_slots[i] = -1;
    for (int i = 0; i < imp->card_count; ++i) {
        if (imp->cards[i].did != sub->deck_id) continue;
        for (int n = 0; n < imp->note_count; ++n) {
            if (imp->notes[n].id != imp->cards[i].nid) continue;
            if (sub->note_slots[n] < 0) {
                sub->note_slots[n] = 1;
                for (int t = 0; t < imp->notetype_count; ++t) {
                    if (imp->notetypes[t].mid == imp->notes[n].mid) sub->notetype_slots[t] = 1;
                }
            }
        }
    }
}

static void write_notetype(cursor_t* c, const anki_notetype_t* nt) {
    cur_i64(c, nt->mid);
    cur_text(c, nt->name);
    cur_u32(c, (uint32_t)nt->field_count);
    for (int i = 0; i < nt->field_count; ++i) cur_text(c, nt->fields[i]);
    cur_u32(c, (uint32_t)nt->template_count);
    for (int i = 0; i < nt->template_count; ++i) {
        cur_text(c, nt->templates[i].name);
        cur_text(c, nt->templates[i].qfmt);
        cur_text(c, nt->templates[i].afmt);
    }
}

static esp_err_t write_deck_file(const anki_import_t* imp, const anki_deck_info_t* deck,
                                 const char* source, const subset_t* sub, int64_t day,
                                 int64_t last_rtc_day) {
    // 先量尺寸再落盘：一次分配，避免半边文件。/ Measure first, then write once.
    size_t need = HEADER_BYTES + strlen(deck->name) + 1 + strlen(source ? source : "") + 1;
    int note_total = 0, notetype_total = 0;
    for (int i = 0; i < imp->note_count; ++i) {
        if (sub->note_slots[i] < 0) continue;
        ++note_total;
        // 16（id+mid）+ 4（长度前缀）+ 字段字节 + NUL。
        // 16 (id+mid) + 4 (length prefix) + field bytes + NUL.
        need += 16 + 4 + strlen(imp->notes[i].flds) + 1;
    }
    for (int i = 0; i < imp->notetype_count; ++i) {
        if (sub->notetype_slots[i] < 0) continue;
        ++notetype_total;
        const anki_notetype_t* nt = &imp->notetypes[i];
        need += 8 + 4 + strlen(nt->name) + 1 + 4;
        for (int f = 0; f < nt->field_count; ++f) need += 4 + strlen(nt->fields[f]) + 1;
        need += 4;
        for (int t = 0; t < nt->template_count; ++t) {
            need += 4 + strlen(nt->templates[t].name) + 1;
            need += 4 + strlen(nt->templates[t].qfmt) + 1;
            need += 4 + strlen(nt->templates[t].afmt) + 1;
        }
    }
    int card_total = 0;
    for (int i = 0; i < imp->card_count; ++i) {
        if (imp->cards[i].did == deck->id) ++card_total;
    }
    need += (size_t)card_total * CARD_RECORD;
    if (need > ANKI_DECK_FILE_MAX) {
        ESP_LOGW(TAG, "deck %s too large: %zu bytes", deck->name, need);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t* buffer = anki_alloc(need);
    if (!buffer) return ESP_ERR_NO_MEM;
    cursor_t c = {.data = buffer, .len = 0, .cap = need, .at = 0, .overflow = false};
    memcpy(c.data, AKD_MAGIC, 4);
    c.at = 4; // memcpy 不推进游标，手动前进 / memcpy does not advance the cursor
    cur_u32(&c, AKD_VERSION);
    cur_i64(&c, deck->id);
    cur_i64(&c, day);
    cur_i64(&c, last_rtc_day);
    cur_u32(&c, (uint32_t)card_total);
    cur_u32(&c, (uint32_t)note_total);
    cur_u32(&c, (uint32_t)notetype_total);
    cur_u32(&c, (uint32_t)strlen(deck->name));
    cur_u32(&c, (uint32_t)strlen(source ? source : ""));
    size_t card_offset = 0; // 稍后回填 / backfilled below
    (void)card_offset;
    cur_u32(&c, 0);          // card_offset 占位 / placeholder
    cur_u32(&c, HEADER_BYTES);
    cur_u32(&c, 0); // 保留 / reserved
    cur_bytes(&c, deck->name, strlen(deck->name) + 1);
    cur_bytes(&c, source ? source : "", strlen(source ? source : "") + 1);
    size_t header_end = c.at;
    for (int i = 0; i < imp->notetype_count && !c.overflow; ++i) {
        if (sub->notetype_slots[i] < 0) continue;
        write_notetype(&c, &imp->notetypes[i]);
    }
    for (int i = 0; i < imp->note_count && !c.overflow; ++i) {
        if (sub->note_slots[i] < 0) continue;
        cur_i64(&c, imp->notes[i].id);
        cur_i64(&c, imp->notes[i].mid);
        cur_text(&c, imp->notes[i].flds);
    }
    // 卡片表紧随其后；回填 offset。/ The card table follows; backfill its offset.
    uint32_t table_at = (uint32_t)c.at;
    memcpy(buffer + 52, &table_at, 4);
    for (int i = 0; i < imp->card_count && !c.overflow; ++i) {
        const anki_card_t* card = &imp->cards[i];
        if (card->did != deck->id) continue;
        cur_i64(&c, card->id);
        cur_i64(&c, card->nid);
        cur_u32(&c, (uint32_t)card->ord);
        cur_u32(&c, (uint32_t)card->type);
        cur_u32(&c, (uint32_t)card->queue);
        cur_u32(&c, (uint32_t)card->due);
        cur_u32(&c, (uint32_t)card->ivl);
        cur_u32(&c, (uint32_t)card->factor);
        cur_u32(&c, (uint32_t)card->reps);
        cur_u32(&c, (uint32_t)card->lapses);
        cur_u32(&c, 0); // 保留字段，凑满 CARD_RECORD / reserved, pads to CARD_RECORD
    }
    if (c.overflow) {
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }
    (void)header_end;

    char path[64];
    deck_path(deck->id, path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "write open %s failed (%d)", path, errno);
        free(buffer);
        return ESP_FAIL;
    }
    size_t wrote = fwrite(buffer, 1, c.at, f);
    bool closed = fclose(f) == 0;
    free(buffer);
    if (wrote != c.at || !closed) {
        ESP_LOGW(TAG, "write %s truncated", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/// 把导入集合的 due 重映射到设备日空间。/ Remap an import's due values into device day space.
static void remap_due(anki_import_t* imp, int64_t rtc_day, int64_t device_day) {
    for (int i = 0; i < imp->card_count; ++i) {
        anki_card_t* card = &imp->cards[i];
        if (card->queue == 2 || card->queue == 3) {
            // 复习卡：due 是天数。/ Review cards: due is a day count.
            int64_t epoch_day = card->due;
            if (!imp->due_epoch_day) epoch_day = imp->crt_day + card->due;
            if (rtc_day > 0) {
                card->due = epoch_day - rtc_day + device_day;
            } else {
                card->due = device_day; // 无 RTC：全部视为今日到期 / no RTC: all due today
            }
        } else if (card->queue == 1 && card->due > 100000000) {
            // 学习卡：due 是 Unix 秒。/ Learning cards: due is Unix seconds.
            int64_t epoch_day = (card->due + ANKI_TZ_OFFSET) / 86400;
            card->due = rtc_day > 0 ? epoch_day - rtc_day + device_day : device_day;
        } else {
            card->due = 0; // 新卡与位置值：立即可学 / new and positional: available now
        }
        if (card->due < 0) card->due = 0;
    }
}

esp_err_t anki_store_import(const char* path, int* out_imported) {
    if (!path) return ESP_ERR_INVALID_ARG;
    if (out_imported) *out_imported = 0;
    esp_err_t err = book_store_ensure_flash();
    if (err != ESP_OK) return err;
    err = ensure_dir();
    if (err != ESP_OK) return err;

    anki_import_t imp = {0};
    err = anki_apkg_import(path, &imp);
    if (err != ESP_OK) return err;

    int64_t rtc_day = anki_day_now();
    int imported = 0;
    for (int d = 0; d < imp.deck_count; ++d) {
        subset_t sub = {.imp = &imp, .deck_id = imp.decks[d].id, .failed = false};
        build_subset(&sub);
        if (sub.failed) {
            free(sub.note_slots);
            free(sub.notetype_slots);
            continue;
        }
        // 重映射按整集合做一次，随后各卡组只用本组卡片。/ Remap once for the
        // whole collection; each deck then writes only its own cards.
        if (d == 0) remap_due(&imp, rtc_day, rtc_day > 0 ? rtc_day : 0);
        err = write_deck_file(&imp, &imp.decks[d], path, &sub, rtc_day > 0 ? rtc_day : 0,
                              rtc_day);
        free(sub.note_slots);
        free(sub.notetype_slots);
        if (err == ESP_OK) {
            ++imported;
        } else {
            ESP_LOGW(TAG, "deck %s write failed: %s", imp.decks[d].name, esp_err_to_name(err));
        }
    }
    anki_apkg_free(&imp);
    if (out_imported) *out_imported = imported;
    return imported > 0 ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------- 读盘

/// 读整个文件；调用方 free。/ Read a whole file; caller frees.
static uint8_t* read_whole_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > ANKI_DECK_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    uint8_t* data = malloc((size_t)st.st_size);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        free(data);
        return NULL;
    }
    *out_len = got;
    return data;
}

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t at;
    bool overflow;
} reader_t;

static uint32_t r_u32(reader_t* r) {
    if (r->overflow || r->at + 4 > r->len) {
        r->overflow = true;
        return 0;
    }
    uint32_t v;
    memcpy(&v, r->p + r->at, 4);
    r->at += 4;
    return v;
}

static int64_t r_i64(reader_t* r) {
    if (r->overflow || r->at + 8 > r->len) {
        r->overflow = true;
        return 0;
    }
    int64_t v;
    memcpy(&v, r->p + r->at, 8);
    r->at += 8;
    return v;
}

/// 读一个长度前缀文本（NUL 结尾）； Borrowed until the buffer is released.
static const char* r_text(reader_t* r, uint32_t* out_len) {
    uint32_t len = r_u32(r);
    if (r->overflow || r->at + len + 1 > r->len) {
        r->overflow = true;
        return NULL;
    }
    const char* text = (const char*)(r->p + r->at);
    r->at += len + 1;
    if (out_len) *out_len = len;
    return text;
}

/// 校验文件头；成功时填充摘要字段。/ Validate the header; fills summary fields.
static bool header_ok(const uint8_t* data, size_t len, anki_deck_summary_t* summary,
                      uint32_t* card_offset, int64_t* day, int64_t* last_rtc_day) {
    if (len < HEADER_BYTES || memcmp(data, AKD_MAGIC, 4) != 0) return false;
    reader_t r = {.p = data, .len = len, .at = 4, .overflow = false};
    uint32_t version = r_u32(&r);
    if (version != AKD_VERSION) return false;
    int64_t deck_id = r_i64(&r);
    int64_t day_value = r_i64(&r);
    int64_t last_rtc = r_i64(&r);
    uint32_t card_count = r_u32(&r);
    uint32_t note_count = r_u32(&r);
    uint32_t notetype_count = r_u32(&r);
    uint32_t name_len = r_u32(&r);
    uint32_t source_len = r_u32(&r);
    uint32_t table_at = r_u32(&r);
    r_u32(&r); // header_bytes
    r_u32(&r); // reserved
    if (r.overflow) return false;
    if (name_len >= ANKI_DECK_NAME_MAX) return false;
    if (HEADER_BYTES + (size_t)name_len + 1 + (size_t)source_len + 1 > len) return false;
    // 空卡片表的 table_at 可以正好等于文件长度（表在文尾）。
    // An empty card table may place table_at exactly at end-of-file.
    size_t table_end = (size_t)table_at + (size_t)card_count * CARD_RECORD;
    if (table_at < HEADER_BYTES || table_end > len) return false;
    if (summary) {
        summary->deck_id = deck_id;
        memcpy(summary->name, data + HEADER_BYTES, name_len);
        summary->name[name_len] = 0;
        summary->total = (int)card_count;
        summary->due = 0;
        summary->fresh = 0;
    }
    if (card_offset) *card_offset = table_at;
    if (day) *day = day_value;
    if (last_rtc_day) *last_rtc_day = last_rtc;
    (void)note_count;
    (void)notetype_count;
    return true;
}

static void free_deck_arrays(anki_deck_t* deck) {
    if (deck->notetypes) {
        for (int i = 0; i < deck->notetype_count; ++i) {
            anki_notetype_t* nt = &deck->notetypes[i];
            for (int t = 0; t < nt->template_count; ++t) {
                free(nt->templates[t].qfmt);
                free(nt->templates[t].afmt);
            }
        }
        free(deck->notetypes);
    }
    for (int i = 0; i < deck->note_count; ++i) free(deck->notes[i].flds);
    free(deck->notes);
    free(deck->cards);
    memset(deck, 0, sizeof(*deck));
}

void anki_store_unload(anki_deck_t* deck) {
    if (!deck) return;
    free_deck_arrays(deck);
}

esp_err_t anki_store_load(int64_t deck_id, anki_deck_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    char path[64];
    deck_path(deck_id, path);
    size_t len = 0;
    uint8_t* data = read_whole_file(path, &len);
    if (!data) return ESP_ERR_NOT_FOUND;

    anki_deck_summary_t summary = {0};
    uint32_t card_offset = 0;
    int64_t day = 0, last_rtc = 0;
    if (!header_ok(data, len, &summary, &card_offset, &day, &last_rtc)) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    out->deck_id = deck_id;
    snprintf(out->name, sizeof(out->name), "%s", summary.name);
    out->day = day;
    out->last_rtc_day = last_rtc;
    out->card_offset = card_offset;
    snprintf(out->path, sizeof(out->path), "%s", path);

    // 字符串紧随 64 字节固定头；name/source 的长度也在固定头里（44/48）。
    // The strings follow the 64-byte fixed header; their lengths live in the
    // header at 44/48.
    reader_t r = {.p = data, .len = len, .at = HEADER_BYTES, .overflow = false};
    uint32_t name_len = 0, source_len = 0;
    {
        reader_t h = {.p = data, .len = len, .at = 44, .overflow = false};
        name_len = r_u32(&h);
        source_len = r_u32(&h);
    }
    if (HEADER_BYTES + (size_t)name_len + 1 + (size_t)source_len + 1 > len) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    r.at = HEADER_BYTES + name_len + 1 + source_len + 1;
    // 段计数在固定文件头里（32/36/40），字符串之后直接就是 notetype 段。
    // Section counts live in the fixed header (32/36/40); the notetype section
    // follows the strings directly.
    uint32_t notetype_count = 0, note_count = 0;
    {
        reader_t h = {.p = data, .len = len, .at = 36, .overflow = false};
        note_count = r_u32(&h);
        notetype_count = r_u32(&h);
    }
    if (r.overflow) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    // 结构数组保持 calloc：notes≈22KB、cards≈71KB（千卡卡组）超过
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 会自动落 PSRAM；notetypes
    // 数量少，占用可忽略。/ Keep calloc for the struct arrays: notes≈22KB
    // and cards≈71KB for a thousand-card deck exceed 16384 and land in
    // PSRAM automatically; notetypes are few and negligible.
    out->notetypes = calloc((size_t)notetype_count + 1, sizeof(anki_notetype_t));
    out->notes = calloc((size_t)note_count + 1, sizeof(anki_note_t));
    out->cards = calloc((size_t)summary.total + 1, sizeof(anki_card_t));
    if (!out->notetypes || !out->notes || !out->cards) {
        free(data);
        anki_store_unload(out);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < notetype_count && !r.overflow; ++i) {
        anki_notetype_t* nt = &out->notetypes[i];
        nt->mid = r_i64(&r);
        uint32_t len2 = 0;
        const char* name = r_text(&r, &len2);
        if (name) {
            if (len2 >= ANKI_TEMPLATE_NAME_MAX) len2 = ANKI_TEMPLATE_NAME_MAX - 1;
            memcpy(nt->name, name, len2);
            nt->name[len2] = 0;
        }
        uint32_t fields = r_u32(&r);
        for (uint32_t f = 0; f < fields && !r.overflow; ++f) {
            const char* field = r_text(&r, &len2);
            if (!field) break;
            // 超容量的字段仍要消费以保持读取同步。/ Over-capacity fields are
            // still consumed to keep the reader in sync.
            if (nt->field_count < ANKI_MAX_FIELDS) {
                if (len2 >= ANKI_FIELD_NAME_MAX) len2 = ANKI_FIELD_NAME_MAX - 1;
                memcpy(nt->fields[nt->field_count], field, len2);
                nt->fields[nt->field_count][len2] = 0;
                ++nt->field_count;
            }
        }
        uint32_t templates = r_u32(&r);
        for (uint32_t t = 0; t < templates && !r.overflow; ++t) {
            uint32_t tname_len = 0, qlen = 0, alen = 0;
            const char* tname = r_text(&r, &tname_len);
            const char* q = r_text(&r, &qlen);
            const char* a = r_text(&r, &alen);
            if (!tname || !q || !a) {
                r.overflow = true;
                break;
            }
            if (nt->template_count < ANKI_MAX_TEMPLATES) {
                anki_template_t* slot = &nt->templates[nt->template_count];
                if (tname_len >= ANKI_TEMPLATE_NAME_MAX) tname_len = ANKI_TEMPLATE_NAME_MAX - 1;
                memcpy(slot->name, tname, tname_len);
                slot->name[tname_len] = 0;
                slot->qfmt = copy_owned(q, qlen);
                slot->afmt = copy_owned(a, alen);
                if (!slot->qfmt || !slot->afmt) {
                    r.overflow = true;
                    break;
                }
                ++nt->template_count;
            }
        }
        ++out->notetype_count;
    }
    for (uint32_t i = 0; i < note_count && !r.overflow; ++i) {
        anki_note_t* note = &out->notes[i];
        note->id = r_i64(&r);
        note->mid = r_i64(&r);
        uint32_t flen = 0;
        const char* flds = r_text(&r, &flen);
        note->flds = flds ? copy_owned(flds, flen) : NULL;
        if (!note->flds) {
            r.overflow = true;
            break;
        }
        ++out->note_count;
    }
    // 卡片表：定长记录，按文件偏移直接读。/ Card table: fixed records by file offset.
    for (int i = 0; i < summary.total; ++i) {
        const uint8_t* rec = data + card_offset + (size_t)i * CARD_RECORD;
        reader_t cr = {.p = rec, .len = (size_t)CARD_RECORD, .at = 0, .overflow = false};
        anki_card_t* card = &out->cards[i];
        card->id = r_i64(&cr);
        card->nid = r_i64(&cr);
        card->ord = (int)r_u32(&cr);
        card->type = (int)r_u32(&cr);
        card->queue = (int)r_u32(&cr);
        card->due = (int)r_u32(&cr);
        card->ivl = (int)r_u32(&cr);
        card->factor = (int)r_u32(&cr);
        card->reps = (int)r_u32(&cr);
        card->lapses = (int)r_u32(&cr);
        ++out->card_count;
    }
    free(data);
    if (r.overflow || out->card_count != summary.total || out->note_count != (int)note_count ||
        out->notetype_count != (int)notetype_count) {
        ESP_LOGW(TAG, "deck %lld load inconsistent", (long long)deck_id);
        anki_store_unload(out);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 列表与日计数

esp_err_t anki_store_list(anki_deck_visit_fn visit, void* ctx) {
    if (!visit) return ESP_ERR_INVALID_ARG;
    esp_err_t err = book_store_ensure_flash();
    if (err != ESP_OK) return err;
    DIR* dir = opendir(ANKI_DIR);
    if (!dir) return ESP_OK; // 还没有卡组目录即空列表 / no directory yet means empty
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcasecmp(dot, ".akd") != 0) continue;
        char path[320];
        if (snprintf(path, sizeof(path), "%s/%s", ANKI_DIR, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        size_t len = 0;
        uint8_t* data = read_whole_file(path, &len);
        if (!data) continue;
        anki_deck_summary_t summary = {0};
        uint32_t card_offset = 0;
        int64_t day = 0, last_rtc = 0;
        bool ok = header_ok(data, len, &summary, &card_offset, &day, &last_rtc);
        if (ok) {
            // 日计数按 RTC 推进后再数到期。/ Advance the day counter before counting due.
            int64_t today = anki_day_now();
            if (today > last_rtc && last_rtc > 0) day += today - last_rtc;
            else if (last_rtc == 0 && today > 0) day = today;
            for (int i = 0; i < summary.total; ++i) {
                const uint8_t* rec = data + card_offset + (size_t)i * CARD_RECORD;
                // 记录布局：id(8) nid(8) ord(4) type(4) queue(4) due(4) ...
                // Record layout: id(8) nid(8) ord(4) type(4) queue(4) due(4) ...
                reader_t cr = {.p = rec, .len = (size_t)CARD_RECORD, .at = 16, .overflow = false};
                r_u32(&cr); // ord
                r_u32(&cr); // type
                uint32_t queue = r_u32(&cr);
                uint32_t due = r_u32(&cr);
                if (queue == 0) ++summary.fresh;
                else if (queue >= 1 && queue <= 3 && (int64_t)due <= day) ++summary.due;
            }
        }
        free(data);
        if (!ok) continue;
        if (!visit(&summary, ctx)) break;
    }
    closedir(dir);
    return ESP_OK;
}

int64_t anki_store_sync_day(anki_deck_t* deck) {
    if (!deck) return 0;
    int64_t today = anki_day_now();
    if (today <= 0) return deck->day; // 未对时：保持 / unset: hold
    if (deck->last_rtc_day <= 0) {
        deck->day = today; // 首次对齐 / first alignment
    } else if (today > deck->last_rtc_day) {
        deck->day += today - deck->last_rtc_day; // 离线多日后追赶 / catch up after offline days
    }
    deck->last_rtc_day = today;
    return deck->day;
}

// ---------------------------------------------------------------- 会话与调度

int anki_store_session(anki_deck_t* deck, int* out_indices, int max) {
    if (!deck || !out_indices || max <= 0) return 0;
    int64_t day = anki_store_sync_day(deck);
    int count = 0;
    // 先新卡，再到期复习；组内按 id 升序，保证确定性。
    // New cards first, then due reviews; ascending id keeps it deterministic.
    for (int i = 0; i < deck->card_count && count < max; ++i) {
        if (deck->cards[i].queue == 0) out_indices[count++] = i;
    }
    for (int i = 0; i < deck->card_count && count < max; ++i) {
        const anki_card_t* card = &deck->cards[i];
        if (card->queue >= 1 && card->queue <= 3 && card->due <= day) out_indices[count++] = i;
    }
    return count;
}

static int clamp_factor(int factor) {
    if (factor <= 0) factor = 2500; // Anki 新卡默认 2.5x / Anki new-card default
    if (factor < 1300) factor = 1300;
    if (factor > 6500) factor = 6500;
    return factor;
}

/// 简化 SM-2：Again 回到学习列，Good/Easy 按系数推间隔，Hard 轻微延长。
/// Simplified SM-2: Again returns to learning, Good/Easy scale by ease, Hard nudges.
void anki_schedule(anki_deck_t* deck, int card_index, anki_grade_t grade) {
    if (!deck || card_index < 0 || card_index >= deck->card_count) return;
    anki_card_t* card = &deck->cards[card_index];
    int64_t day = anki_store_sync_day(deck);
    card->factor = clamp_factor(card->factor);
    ++card->reps;
    if (grade == ANKI_GRADE_AGAIN) {
        if (card->type == 2) ++card->lapses; // 复习卡遗忘计流失 / a lapsed review counts
        card->type = 1;
        card->queue = 1;
        card->ivl = 0;
        card->due = day; // 今日内重来 / relearn within today
        card->factor -= 200;
        card->factor = clamp_factor(card->factor);
        return;
    }
    if (grade == ANKI_GRADE_HARD) {
        if (card->type == 2) {
            int ivl = card->ivl > 0 ? card->ivl : 1;
            ivl = ivl + ivl / 5; // +20%
            if (ivl <= card->ivl) ivl = card->ivl + 1;
            card->ivl = ivl;
            card->due = day + ivl;
            card->factor -= 150;
            card->factor = clamp_factor(card->factor);
        } else {
            // 新卡/学习卡：留在学习列今日再见。/ New/learning: stay in learning today.
            card->type = 1;
            card->queue = 1;
            card->due = day;
        }
        return;
    }
    if (grade == ANKI_GRADE_GOOD) {
        if (card->type == 2) {
            int ivl = card->ivl > 0 ? card->ivl : 1;
            int grown = (int)((int64_t)ivl * card->factor / 1000);
            card->ivl = grown > ivl ? grown : ivl + 1;
        } else if (card->type == 1) {
            int ivl = card->ivl > 0 ? card->ivl : 1; // 学习卡毕业带当前步数 / graduating keeps the step
            card->ivl = ivl;
        } else {
            card->ivl = 1;
        }
        card->type = 2;
        card->queue = 2;
        card->due = day + card->ivl;
        return;
    }
    // Easy
    if (grade == ANKI_GRADE_EASY) {
        if (card->type == 2) {
            int ivl = card->ivl > 0 ? card->ivl : 1;
            int grown = (int)((int64_t)ivl * card->factor / 1000);
            grown = grown + grown / 4; // 再 +25% / an extra 25%
            card->ivl = grown > ivl ? grown : ivl + 2;
        } else if (card->type == 1) {
            int ivl = card->ivl > 0 ? card->ivl : 1;
            card->ivl = ivl > 4 ? ivl : 4;
        } else {
            card->ivl = 4;
        }
        card->type = 2;
        card->queue = 2;
        card->due = day + card->ivl;
        card->factor += 150;
        card->factor = clamp_factor(card->factor);
        return;
    }
}

esp_err_t anki_store_save_card(anki_deck_t* deck, int card_index) {
    if (!deck || card_index < 0 || card_index >= deck->card_count) return ESP_ERR_INVALID_ARG;
    const anki_card_t* card = &deck->cards[card_index];
    uint8_t record[CARD_RECORD];
    memcpy(record + 0, &card->id, 8);
    memcpy(record + 8, &card->nid, 8);
    uint32_t ord = (uint32_t)card->ord, type = (uint32_t)card->type, queue = (uint32_t)card->queue,
             due = (uint32_t)card->due, ivl = (uint32_t)card->ivl, factor = (uint32_t)card->factor,
             reps = (uint32_t)card->reps, lapses = (uint32_t)card->lapses;
    memcpy(record + 16, &ord, 4);
    memcpy(record + 20, &type, 4);
    memcpy(record + 24, &queue, 4);
    memcpy(record + 28, &due, 4);
    memcpy(record + 32, &ivl, 4);
    memcpy(record + 36, &factor, 4);
    memcpy(record + 40, &reps, 4);
    memcpy(record + 44, &lapses, 4);
    memset(record + 48, 0, 4); // 保留 / reserved

    // 卡片表偏移在载入时已从文件头取到，直接原地写 52 字节。
    // The table offset was captured at load time; patch 52 bytes in place.
    FILE* f = fopen(deck->path, "r+b");
    if (!f) return ESP_FAIL;
    bool good = fseek(f, (long)(deck->card_offset + (size_t)card_index * CARD_RECORD), SEEK_SET) == 0;
    if (good) good = fwrite(record, 1, CARD_RECORD, f) == CARD_RECORD;
    if (fclose(f) != 0) good = false;
    if (!good) {
        ESP_LOGW(TAG, "progress patch failed for %s card %d", deck->path, card_index);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t anki_store_delete(int64_t deck_id) {
    char path[64];
    deck_path(deck_id, path);
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "delete %s failed (%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}
