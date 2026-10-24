/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡组导入实现。.apkg 走 zip_reader 取集合库；裸库直接读文件。
 *       v2（col 内 JSON）与 v3（notetypes/templates 表）两种 schema 都解析。
 * English: Anki deck import. .apkg pulls the collection through zip_reader;
 *          a bare database is read directly. Both the v2 (JSON inside col) and
 *          v3 (notetypes/templates tables) schemas are parsed.
 */
#include "anki_apkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "anki_json.h"
#include "anki_sqlite.h"
#include "zip_reader.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define TAG "anki_apkg"

#define COLLECTION_MAX (8U * 1024U * 1024U)
#define FIELD_TEXT_MAX (48U * 1024U)
#define COL_JSON_MAX (2U * 1024U * 1024U)

// 大块走 PSRAM：解析中的集合与模型都放那边，内部 RAM 留给栈和 UI。
// Large blocks live in PSRAM; internal RAM stays free for stack and UI.
static void* anki_alloc(size_t n) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    // 宿主测试：填充 0xAA 模拟真机未初始化的 PSRAM。宿主与设备的这个差异
    // 曾让「只在扩容时清零」的槽位 bug 逃过全部宿主测试、却在真机闪退。
    // / Host tests: fill 0xAA to mimic the device's uninitialized PSRAM. This
    // host/device gap once let the zero-only-on-growth slot bug escape every
    // host test and crash on real hardware.
    void* p = malloc(n);
    if (p) memset(p, 0xAA, n);
    return p;
#endif
}

static char* copy_text(const char* src, size_t len) {
    if (len > FIELD_TEXT_MAX) return NULL;
    char* out = anki_alloc(len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, src, len);
    out[len] = 0;
    return out;
}

/// 定长数组增长；新槽由调用方在每次追加时清零。/ Grow a fixed-stride array;
/// the caller zeroes each appended slot.
static void* grow(void* data, int* cap, size_t item, int need) {
    if (need <= *cap) return data;
    int next = *cap ? *cap * 2 : 16;
    while (next < need) next *= 2;
    void* p = anki_alloc((size_t)next * item);
    if (!p) return NULL;
    if (data && *cap) memcpy(p, data, (size_t)(*cap) * item);
    *cap = next;
    return p;
}

static bool deck_known(const anki_import_t* imp, int64_t did) {
    for (int i = 0; i < imp->deck_count; ++i) {
        if (imp->decks[i].id == did) return true;
    }
    return false;
}

/// 取主键值：INTEGER PRIMARY KEY 在记录里是 NULL，真值是 rowid。
/// Read a primary key: INTEGER PRIMARY KEY stores NULL in the record; the
/// real value is the rowid.
static int64_t pk_value(const anki_sql_value_t* v, int64_t rowid) {
    return v->type == ANKI_SQL_INT ? v->i : rowid;
}

// ---------------------------------------------------------------- v2 JSON 模型

typedef struct {
    anki_import_t* imp;
    bool failed;
    int notetype_cap;
    int deck_cap;
} v2_build_t;

static bool v2_add_notetype(v2_build_t* b, int64_t mid, const char* name,
                            const anki_json_value_t* flds, const anki_json_value_t* tmpls) {
    if (b->imp->notetype_count == b->notetype_cap) {
        anki_notetype_t* grown = grow(b->imp->notetypes, &b->notetype_cap,
                                      sizeof(anki_notetype_t), b->imp->notetype_count + 1);
        if (!grown) {
            b->failed = true;
            return false;
        }
        b->imp->notetypes = grown;
    }
    // 每个新槽都必须清零：只清扩容那一格的话，同一扩容周期内的后续槽是
    // 未初始化的 PSRAM 垃圾，template_count/模板指针为随机值，释放时触发
    // heap_caps_free 断言。/ Every appended slot must be zeroed: zeroing only
    // the growth tick leaves later slots in the same growth window holding
    // uninitialized PSRAM garbage; random template_count/pointers then trip
    // the heap_caps_free assertion on release.
    anki_notetype_t* nt = &b->imp->notetypes[b->imp->notetype_count];
    memset(nt, 0, sizeof(*nt));
    nt->mid = mid;
    size_t name_len = strlen(name);
    if (name_len >= ANKI_TEMPLATE_NAME_MAX) name_len = ANKI_TEMPLATE_NAME_MAX - 1;
    memcpy(nt->name, name, name_len);
    nt->name[name_len] = 0;
    if (flds) {
        size_t n = anki_json_size(flds);
        for (size_t i = 0; i < n && nt->field_count < ANKI_MAX_FIELDS; ++i) {
            const anki_json_value_t* f = anki_json_at(flds, i);
            const anki_json_value_t* fname = f ? anki_json_get(f, "name") : NULL;
            size_t len = 0;
            const char* text = fname ? anki_json_string(fname, &len) : NULL;
            if (!text || !len) continue;
            if (len >= ANKI_FIELD_NAME_MAX) len = ANKI_FIELD_NAME_MAX - 1;
            memcpy(nt->fields[nt->field_count], text, len);
            nt->fields[nt->field_count][len] = 0;
            ++nt->field_count;
        }
    }
    if (tmpls) {
        size_t n = anki_json_size(tmpls);
        for (size_t i = 0; i < n && nt->template_count < ANKI_MAX_TEMPLATES; ++i) {
            const anki_json_value_t* t = anki_json_at(tmpls, i);
            if (!t) continue;
            const anki_json_value_t* tname = anki_json_get(t, "name");
            const anki_json_value_t* qfmt = anki_json_get(t, "qfmt");
            const anki_json_value_t* afmt = anki_json_get(t, "afmt");
            size_t len = 0;
            const char* text = tname ? anki_json_string(tname, &len) : NULL;
            anki_template_t* slot = &nt->templates[nt->template_count];
            if (text && len) {
                if (len >= ANKI_TEMPLATE_NAME_MAX) len = ANKI_TEMPLATE_NAME_MAX - 1;
                memcpy(slot->name, text, len);
                slot->name[len] = 0;
            } else {
                snprintf(slot->name, sizeof(slot->name), "Card %d", nt->template_count + 1);
            }
            size_t qlen = 0, alen = 0;
            const char* q = qfmt ? anki_json_string(qfmt, &qlen) : NULL;
            const char* a = afmt ? anki_json_string(afmt, &alen) : NULL;
            slot->qfmt = copy_text(q ? q : "", q ? qlen : 0);
            slot->afmt = copy_text(a ? a : "", a ? alen : 0);
            if (!slot->qfmt || !slot->afmt) {
                b->failed = true;
                return false;
            }
            ++nt->template_count;
        }
    }
    ++b->imp->notetype_count;
    return true;
}

static bool v2_add_deck(v2_build_t* b, int64_t did, const char* name, bool dynamic) {
    if (dynamic) return true; // 过滤/动态卡组不导入 / filtered decks are skipped
    if (b->imp->deck_count == b->deck_cap) {
        anki_deck_info_t* grown = grow(b->imp->decks, &b->deck_cap, sizeof(anki_deck_info_t),
                                       b->imp->deck_count + 1);
        if (!grown) {
            b->failed = true;
            return false;
        }
        b->imp->decks = grown;
    }
    // 同 notetype：每次追加都要清零。/ Same as notetypes: zero on every append.
    anki_deck_info_t* deck = &b->imp->decks[b->imp->deck_count];
    memset(deck, 0, sizeof(*deck));
    deck->id = did;
    size_t len = strlen(name);
    if (len >= ANKI_DECK_NAME_MAX) len = ANKI_DECK_NAME_MAX - 1;
    memcpy(deck->name, name, len);
    deck->name[len] = 0;
    ++b->imp->deck_count;
    return true;
}

/// 遍历 JSON 对象成员（v2 的 models/decks 都是数字键对象）。
/// Visit every object member (v2 models/decks are numeric-keyed objects).
typedef bool (*json_object_visit_fn)(const char* key, const anki_json_value_t* value, void* ctx);

static bool json_visit_object(const anki_json_value_t* obj, json_object_visit_fn fn, void* ctx) {
    if (!obj || anki_json_type(obj) != ANKI_JSON_OBJECT) return false;
    bool ok = true;
    size_t count = anki_json_member_count(obj);
    for (size_t i = 0; i < count && ok; ++i) {
        size_t key_len = 0;
        const char* key = anki_json_member_key(obj, i, &key_len);
        const anki_json_value_t* value = anki_json_member_value(obj, i);
        if (!key || !value) continue;
        ok = fn(key, value, ctx);
    }
    return ok;
}

typedef struct {
    v2_build_t* build;
    bool models;
} v2_visit_ctx_t;

static bool v2_visit_entry(const char* key, const anki_json_value_t* value, void* ctx) {
    v2_visit_ctx_t* visit = (v2_visit_ctx_t*)ctx;
    v2_build_t* b = visit->build;
    const anki_json_value_t* name = anki_json_get(value, "name");
    size_t len = 0;
    const char* text = name ? anki_json_string(name, &len) : NULL;
    if (!text) return true;
    char buffer[ANKI_DECK_NAME_MAX];
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, text, len);
    buffer[len] = 0;
    // v2 的 id 是 JSON 对象的数字键。/ v2 ids are the numeric object keys.
    int64_t id = key && *key ? strtoll(key, NULL, 10) : 0;
    if (visit->models) {
        return v2_add_notetype(b, id, buffer, anki_json_get(value, "flds"),
                               anki_json_get(value, "tmpls"));
    }
    return v2_add_deck(b, id, buffer, anki_json_bool(anki_json_get(value, "dyn")));
}

static bool v2_parse_col(v2_build_t* b, const char* models_json, size_t models_len,
                         const char* decks_json, size_t decks_len) {
    anki_json_value_t* models = anki_json_parse(models_json, models_len);
    if (!models) {
        ESP_LOGW(TAG, "col.models JSON invalid");
        b->failed = true;
        return false;
    }
    v2_visit_ctx_t visit = {.build = b, .models = true};
    json_visit_object(models, v2_visit_entry, &visit);
    anki_json_free(models);
    if (b->failed) return false;

    anki_json_value_t* decks = anki_json_parse(decks_json, decks_len);
    if (!decks) {
        ESP_LOGW(TAG, "col.decks JSON invalid");
        b->failed = true;
        return false;
    }
    visit.models = false;
    json_visit_object(decks, v2_visit_entry, &visit);
    anki_json_free(decks);
    return !b->failed;
}

/// col 行回调：把 models/decks 两列复制到暂存缓冲（借用值只在回调内有效），
/// 并取 crt 供 v1 调度器的 due 重映射。cap 为各缓冲的实际容量，越界即失败——
/// 容量来自第一遍测量的实测长度，不再写死 COL_JSON_MAX。
/// / col row callback: stage the models/decks columns (borrowed values die
/// with the call) and capture crt for the v1 scheduler due remap. Each cap is
/// the real capacity of its buffer and overflows fail — caps come from the
/// measured first pass instead of a hard-wired COL_JSON_MAX.
typedef struct {
    char* models;
    char* decks;
    size_t* models_size;
    size_t* decks_size;
    int64_t* crt;
    size_t models_cap, decks_cap;
    int models_at, decks_at, crt_at;
    bool failed;
} col_stage_t;

static bool on_col_row(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    (void)rowid;
    col_stage_t* stage = (col_stage_t*)ctx;
    const anki_sql_value_t* models = stage->models_at >= 0 && stage->models_at < count
                                         ? &v[stage->models_at]
                                         : NULL;
    const anki_sql_value_t* decks =
        stage->decks_at >= 0 && stage->decks_at < count ? &v[stage->decks_at] : NULL;
    const anki_sql_value_t* crt =
        stage->crt_at >= 0 && stage->crt_at < count ? &v[stage->crt_at] : NULL;
    if (models && models->type == ANKI_SQL_TEXT) {
        if (models->text_len >= stage->models_cap) {
            stage->failed = true;
            return false;
        }
        memcpy(stage->models, models->text, models->text_len);
        *stage->models_size = models->text_len;
    }
    if (decks && decks->type == ANKI_SQL_TEXT) {
        if (decks->text_len >= stage->decks_cap) {
            stage->failed = true;
            return false;
        }
        memcpy(stage->decks, decks->text, decks->text_len);
        *stage->decks_size = decks->text_len;
    }
    if (crt && crt->type == ANKI_SQL_INT) *stage->crt = crt->i;
    return false; // 只要一行 / one row is enough
}

/// 第一遍测量回调：只记录 models/decks 的实测长度与 crt，不分配任何缓冲。
/// col 恒只有一行，扫到即停。/ First-pass measure callback: records only the
/// measured models/decks lengths and crt, allocating nothing. col always holds
/// a single row, so scanning stops at the first one.
typedef struct {
    size_t models_len, decks_len;
    int64_t crt;
    int models_at, decks_at, crt_at;
} col_measure_t;

static bool on_col_measure(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    (void)rowid;
    col_measure_t* m = (col_measure_t*)ctx;
    const anki_sql_value_t* models =
        m->models_at >= 0 && m->models_at < count ? &v[m->models_at] : NULL;
    const anki_sql_value_t* decks =
        m->decks_at >= 0 && m->decks_at < count ? &v[m->decks_at] : NULL;
    const anki_sql_value_t* crt = m->crt_at >= 0 && m->crt_at < count ? &v[m->crt_at] : NULL;
    if (models && models->type == ANKI_SQL_TEXT) m->models_len = models->text_len;
    if (decks && decks->type == ANKI_SQL_TEXT) m->decks_len = decks->text_len;
    if (crt && crt->type == ANKI_SQL_INT) m->crt = crt->i;
    return false; // 只要一行 / one row is enough
}

// ---------------------------------------------------------------- v3 表模型

typedef struct {
    anki_import_t* imp;
    bool failed;
    int notetype_cap;
    int deck_cap;
} v3_build_t;

static anki_notetype_t* v3_notetype(v3_build_t* b, int64_t id, const char* name, size_t name_len) {
    for (int i = 0; i < b->imp->notetype_count; ++i) {
        if (b->imp->notetypes[i].mid == id) return &b->imp->notetypes[i];
    }
    if (b->imp->notetype_count == b->notetype_cap) {
        anki_notetype_t* grown = grow(b->imp->notetypes, &b->notetype_cap, sizeof(anki_notetype_t),
                                      b->imp->notetype_count + 1);
        if (!grown) {
            b->failed = true;
            return NULL;
        }
        b->imp->notetypes = grown;
    }
    // 每个新槽都必须清零，理由同 v2 路径。/ Every appended slot must be zeroed;
    // see the v2 path for why.
    anki_notetype_t* nt = &b->imp->notetypes[b->imp->notetype_count];
    memset(nt, 0, sizeof(*nt));
    nt->mid = id;
    if (name_len >= ANKI_TEMPLATE_NAME_MAX) name_len = ANKI_TEMPLATE_NAME_MAX - 1;
    memcpy(nt->name, name, name_len);
    nt->name[name_len] = 0;
    ++b->imp->notetype_count;
    return nt;
}

static anki_notetype_t* v3_find(const anki_import_t* imp, int64_t mid) {
    for (int i = 0; i < imp->notetype_count; ++i) {
        if (imp->notetypes[i].mid == mid) return &imp->notetypes[i];
    }
    return NULL;
}

static bool v3_on_notetype(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    v3_build_t* b = (v3_build_t*)ctx;
    if (count < 2 || v[1].type != ANKI_SQL_TEXT) return true;
    return v3_notetype(b, pk_value(&v[0], rowid), v[1].text, v[1].text_len) != NULL;
}

static bool v3_on_field(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    (void)rowid;
    v3_build_t* b = (v3_build_t*)ctx;
    if (count < 3 || v[0].type != ANKI_SQL_INT || v[2].type != ANKI_SQL_TEXT) return true;
    anki_notetype_t* nt = v3_find(b->imp, v[0].i);
    if (!nt || nt->field_count >= ANKI_MAX_FIELDS) return true;
    size_t len = v[2].text_len;
    if (len >= ANKI_FIELD_NAME_MAX) len = ANKI_FIELD_NAME_MAX - 1;
    memcpy(nt->fields[nt->field_count], v[2].text, len);
    nt->fields[nt->field_count][len] = 0;
    ++nt->field_count;
    return true;
}

static bool v3_on_template(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    (void)rowid;
    v3_build_t* b = (v3_build_t*)ctx;
    // 列序：notetype_id, ord, name, mtime_secs, usn, config
    // Column order: notetype_id, ord, name, mtime_secs, usn, config
    if (count < 6 || v[0].type != ANKI_SQL_INT || v[2].type != ANKI_SQL_TEXT) {
        return true;
    }
    // config 列亲和性为 blob，Anki 实际存 JSON 文本；两种类型都收。
    // The config column has blob affinity but Anki stores JSON text; accept both.
    if (v[5].type != ANKI_SQL_TEXT && v[5].type != ANKI_SQL_BLOB) return true;
    anki_notetype_t* nt = v3_find(b->imp, v[0].i);
    if (!nt || nt->template_count >= ANKI_MAX_TEMPLATES) return true;
    anki_template_t* slot = &nt->templates[nt->template_count];
    size_t len = v[2].text_len;
    if (len >= ANKI_TEMPLATE_NAME_MAX) len = ANKI_TEMPLATE_NAME_MAX - 1;
    memcpy(slot->name, v[2].text, len);
    slot->name[len] = 0;
    anki_json_value_t* config = anki_json_parse(v[5].text, v[5].text_len);
    if (config) {
        const anki_json_value_t* qfmt = anki_json_get(config, "qfmt");
        const anki_json_value_t* afmt = anki_json_get(config, "afmt");
        size_t qlen = 0, alen = 0;
        const char* q = qfmt ? anki_json_string(qfmt, &qlen) : NULL;
        const char* a = afmt ? anki_json_string(afmt, &alen) : NULL;
        slot->qfmt = copy_text(q ? q : "", q ? qlen : 0);
        slot->afmt = copy_text(a ? a : "", a ? alen : 0);
        anki_json_free(config);
    } else {
        slot->qfmt = copy_text("", 0);
        slot->afmt = copy_text("", 0);
    }
    if (!slot->qfmt || !slot->afmt) {
        b->failed = true;
        return false;
    }
    ++nt->template_count;
    return true;
}

static bool v3_on_deck(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    v3_build_t* b = (v3_build_t*)ctx;
    // 列序：id, name, mtime_secs, usn, common, kind
    // Column order: id, name, mtime_secs, usn, common, kind
    if (count < 6 || v[1].type != ANKI_SQL_TEXT) return true;
    int64_t kind = v[5].type == ANKI_SQL_INT ? v[5].i : 0;
    if (kind != 0) return true; // 过滤/动态卡组跳过 / filtered decks skipped
    if (b->imp->deck_count == b->deck_cap) {
        anki_deck_info_t* grown = grow(b->imp->decks, &b->deck_cap, sizeof(anki_deck_info_t),
                                       b->imp->deck_count + 1);
        if (!grown) {
            b->failed = true;
            return false;
        }
        b->imp->decks = grown;
    }
    // 同 v2 卡组：每次追加都要清零。/ Same as v2 decks: zero on every append.
    anki_deck_info_t* deck = &b->imp->decks[b->imp->deck_count];
    memset(deck, 0, sizeof(*deck));
    deck->id = pk_value(&v[0], rowid);
    size_t len = v[1].text_len;
    if (len >= ANKI_DECK_NAME_MAX) len = ANKI_DECK_NAME_MAX - 1;
    memcpy(deck->name, v[1].text, len);
    deck->name[len] = 0;
    ++b->imp->deck_count;
    return true;
}

// ---------------------------------------------------------------- notes/cards

typedef struct {
    anki_import_t* imp;
    bool failed;
    int note_cap;
    int card_cap;
} rows_build_t;

static bool on_note(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    rows_build_t* b = (rows_build_t*)ctx;
    // 列序：id, guid, mid, mod, usn, tags, flds, sfld, ...
    // Column order: id, guid, mid, mod, usn, tags, flds, sfld, ...
    if (count < 7 || v[2].type != ANKI_SQL_INT || v[6].type != ANKI_SQL_TEXT) {
        return true;
    }
    if (b->imp->note_count >= ANKI_IMPORT_NOTE_MAX) {
        b->failed = true;
        return false;
    }
    if (b->imp->note_count == b->note_cap) {
        anki_note_t* grown = grow(b->imp->notes, &b->note_cap, sizeof(anki_note_t),
                                  b->imp->note_count + 1);
        if (!grown) {
            b->failed = true;
            return false;
        }
        b->imp->notes = grown;
    }
    // 同 notetype：每次追加都要清零。/ Same as notetypes: zero on every append.
    anki_note_t* note = &b->imp->notes[b->imp->note_count];
    memset(note, 0, sizeof(*note));
    note->id = pk_value(&v[0], rowid);
    note->mid = v[2].i;
    note->flds = copy_text(v[6].text, v[6].text_len);
    if (!note->flds) {
        b->failed = true;
        return false;
    }
    ++b->imp->note_count;
    return true;
}

static bool on_card(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    rows_build_t* b = (rows_build_t*)ctx;
    // 列序：id, nid, did, ord, mod, usn, type, queue, due, ivl, factor, reps, lapses, ...
    // Column order: id, nid, did, ord, mod, usn, type, queue, due, ivl, factor, reps, lapses, ...
    if (count < 13 || v[1].type != ANKI_SQL_INT || v[2].type != ANKI_SQL_INT ||
        v[6].type != ANKI_SQL_INT || v[7].type != ANKI_SQL_INT) {
        return true;
    }
    int type = (int)v[6].i;
    int queue = (int)v[7].i;
    if (type < 0 || type > 2 || queue < 0) return true; // 挂起与过滤态不导入 / suspended/filtered skipped
    if (!deck_known(b->imp, v[2].i)) return true;
    if (b->imp->card_count >= ANKI_IMPORT_CARD_MAX) {
        b->failed = true;
        return false;
    }
    if (b->imp->card_count == b->card_cap) {
        anki_card_t* grown = grow(b->imp->cards, &b->card_cap, sizeof(anki_card_t),
                                  b->imp->card_count + 1);
        if (!grown) {
            b->failed = true;
            return false;
        }
        b->imp->cards = grown;
    }
    // 同 notetype：每次追加都要清零。/ Same as notetypes: zero on every append.
    anki_card_t* card = &b->imp->cards[b->imp->card_count];
    memset(card, 0, sizeof(*card));
    card->id = pk_value(&v[0], rowid);
    card->nid = v[1].i;
    card->did = v[2].i;
    card->ord = v[3].type == ANKI_SQL_INT ? (int)v[3].i : 0;
    card->type = type;
    card->queue = queue;
    card->due = v[8].type == ANKI_SQL_INT ? v[8].i : 0;
    card->ivl = v[9].type == ANKI_SQL_INT ? (int)v[9].i : 0;
    card->factor = v[10].type == ANKI_SQL_INT ? (int)v[10].i : 0;
    card->reps = v[11].type == ANKI_SQL_INT ? (int)v[11].i : 0;
    card->lapses = count > 12 && v[12].type == ANKI_SQL_INT ? (int)v[12].i : 0;
    ++b->imp->card_count;
    return true;
}

// ---------------------------------------------------------------- 入口

void anki_apkg_free(anki_import_t* result) {
    if (!result) return;
    for (int i = 0; i < result->notetype_count; ++i) {
        anki_notetype_t* nt = &result->notetypes[i];
        for (int t = 0; t < nt->template_count; ++t) {
            free(nt->templates[t].qfmt);
            free(nt->templates[t].afmt);
        }
    }
    for (int i = 0; i < result->note_count; ++i) free(result->notes[i].flds);
    free(result->notetypes);
    free(result->decks);
    free(result->notes);
    free(result->cards);
    memset(result, 0, sizeof(*result));
}

const anki_note_t* anki_apkg_note(const anki_import_t* imp, int64_t nid) {
    if (!imp) return NULL;
    for (int i = 0; i < imp->note_count; ++i) {
        if (imp->notes[i].id == nid) return &imp->notes[i];
    }
    return NULL;
}

const anki_notetype_t* anki_apkg_notetype(const anki_import_t* imp, int64_t mid) {
    if (!imp) return NULL;
    for (int i = 0; i < imp->notetype_count; ++i) {
        if (imp->notetypes[i].mid == mid) return &imp->notetypes[i];
    }
    return NULL;
}

/// 读取整个文件到内存（裸 SQLite 集合）。/ Read a whole file into memory (a bare
/// SQLite collection).
static esp_err_t read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > COLLECTION_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t* data = anki_alloc((size_t)st.st_size);
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        free(data);
        return ESP_FAIL;
    }
    *out = data;
    *out_len = got;
    return ESP_OK;
}

/// 从 .apkg ZIP 中取出集合库。/ Pull the collection database out of an .apkg ZIP.
static esp_err_t read_apkg(const char* path, uint8_t** out, size_t* out_len) {
    zip_reader_t* zip = NULL;
    esp_err_t err = zip_open(path, &zip);
    if (err != ESP_OK) return err;
    // 新版 Anki 导出会同时包含两个条目：collection.anki2 是给旧版 Anki 的
    // 占位存根（内容为"请升级 Anki"的提示卡），真库在 collection.anki21，
    // 必须优先取 anki21。/ Modern Anki exports ship both entries:
    // collection.anki2 is a placeholder stub for old Anki (a "please update
    // Anki" prompt card) while the real collection lives in collection.anki21;
    // anki21 must be preferred.
    int index = zip_find(zip, "collection.anki21");
    if (index < 0) index = zip_find(zip, "collection.anki2");
    if (index < 0) {
        zip_close(zip);
        return ESP_ERR_NOT_FOUND;
    }
    size_t size = zip_entry_size(zip, index);
    if (size == 0 || size > COLLECTION_MAX) {
        zip_close(zip);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t* data = anki_alloc(size + 1); // +1：文本列安全截断 / +1 for safe text truncation
    if (!data) {
        zip_close(zip);
        return ESP_ERR_NO_MEM;
    }
    err = zip_extract(zip, index, data, size);
    zip_close(zip);
    if (err != ESP_OK) {
        free(data);
        return err;
    }
    data[size] = 0;
    *out = data;
    *out_len = size;
    return ESP_OK;
}

esp_err_t anki_apkg_import(const char* path, anki_import_t* out) {
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    uint8_t* collection = NULL;
    size_t collection_len = 0;
    FILE* probe = fopen(path, "rb");
    if (!probe) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t magic[16] = {0};
    size_t got = fread(magic, 1, sizeof(magic), probe);
    fclose(probe);
    esp_err_t err;
    if (got >= 4 && memcmp(magic, "PK\x03\x04", 4) == 0) {
        err = read_apkg(path, &collection, &collection_len);
    } else {
        err = read_file(path, &collection, &collection_len);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "collection read failed: %s", esp_err_to_name(err));
        return err;
    }

    anki_sqlite_t* db = NULL;
    err = anki_sql_open(collection, collection_len, &db);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "collection open failed: %s", esp_err_to_name(err));
        free(collection);
        return err;
    }

    bool v3 = anki_sql_table_root(db, "notetypes") != 0;
    bool v2 = anki_sql_table_root(db, "col") != 0;
    if (!v3 && !v2) {
        ESP_LOGW(TAG, "no col or notetypes table");
        anki_sql_close(db);
        free(collection);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (v3) {
        out->due_epoch_day = 1; // anki21 只有 v2 调度器 / anki21 is v2 scheduler only
        v3_build_t build = {.imp = out, .failed = false};
        err = anki_sql_scan_table(db, "notetypes", v3_on_notetype, &build);
        if (err == ESP_OK && !build.failed)
            err = anki_sql_scan_table(db, "fields", v3_on_field, &build);
        if (err == ESP_OK && !build.failed)
            err = anki_sql_scan_table(db, "templates", v3_on_template, &build);
        if (err == ESP_OK && !build.failed)
            err = anki_sql_scan_table(db, "decks", v3_on_deck, &build);
        if (err != ESP_OK || build.failed) {
            ESP_LOGW(TAG, "v3 schema parse failed: %s", esp_err_to_name(err));
            goto fail;
        }
    } else {
        v2_build_t build = {.imp = out, .failed = false};
        const char* const* columns = NULL;
        int column_count = anki_sql_table_columns(db, "col", &columns);
        int models_at = -1, decks_at = -1, crt_at = -1;
        for (int i = 0; i < column_count; ++i) {
            if (strcasecmp(columns[i], "models") == 0) models_at = i;
            else if (strcasecmp(columns[i], "decks") == 0) decks_at = i;
            else if (strcasecmp(columns[i], "crt") == 0) crt_at = i;
        }
        // 回退到 v2 col 的固定列序。/ Fall back to the fixed v2 col order.
        if (models_at < 0) models_at = 9;
        if (decks_at < 0) decks_at = 10;
        if (crt_at < 0) crt_at = 1;
        // 两遍扫描：先测量再按需分配。col 恒只有一行，第一遍拿到 models/
        // decks 的实际长度后按实测值分配暂存缓冲——此前固定申请 2×2MB，加上
        // 集合体本身与解压工作区，1.1MB 真库的峰值 ~5.3MB，PSRAM 紧张时
        // decks_buf 分配失败且整条路径无日志。按需分配后峰值降到 ~1.3MB。
        // / Two-pass scan: measure first, then allocate on demand. col always
        // holds one row, so a first pass takes the real models/decks lengths
        // and the staging buffers are sized to match — the old fixed 2×2MB
        // staging peaked at ~5.3MB with a 1.1MB collection (collection plus
        // unzip workspace) and decks_buf failed silently under PSRAM
        // pressure. On-demand staging brings the peak down to ~1.3MB.
        col_measure_t measure = {0};
        measure.models_at = models_at;
        measure.decks_at = decks_at;
        measure.crt_at = crt_at;
        err = anki_sql_scan_table(db, "col", on_col_measure, &measure);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "col measure scan failed: %s", esp_err_to_name(err));
            goto fail;
        }
        if (measure.models_len >= COL_JSON_MAX || measure.decks_len >= COL_JSON_MAX) {
            ESP_LOGW(TAG, "col JSON too large: models=%zu decks=%zu (cap %u)",
                     measure.models_len, measure.decks_len, (unsigned)COL_JSON_MAX);
            err = ESP_ERR_INVALID_SIZE;
            goto fail;
        }
        char* models_buf = anki_alloc(measure.models_len + 1);
        char* decks_buf = anki_alloc(measure.decks_len + 1);
        if (!models_buf || !decks_buf) {
#ifdef ESP_PLATFORM
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
            size_t psram_free = 0; // 宿主没有 PSRAM 概念 / the host has no PSRAM notion
            (void)psram_free;      // 宿主 ESP_LOGW 为空宏，防未用告警 / host ESP_LOGW is a no-op
#endif
            // 分配失败必须有日志：真机上这条路径曾完全静默，UI 也看不到原因。
            // / Allocation failure must log: this path used to be fully silent
            // on the device, with the UI hiding the reason too.
            ESP_LOGW(TAG, "col staging alloc failed (%zu+%zu bytes, %zu PSRAM free)",
                     measure.models_len + 1, measure.decks_len + 1, psram_free);
            free(models_buf);
            free(decks_buf);
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        // 列为 NULL/缺失时保持合法空串（宿主分配器毒化 0xAA）。
        // / Keep a valid empty string when the column is NULL or missing (the
        // host allocator poisons with 0xAA).
        models_buf[0] = 0;
        decks_buf[0] = 0;
        size_t models_size = 0, decks_size = 0;
        int64_t crt = 0;
        col_stage_t stage = {
            .models = models_buf,
            .decks = decks_buf,
            .models_size = &models_size,
            .decks_size = &decks_size,
            .crt = &crt,
            .models_cap = measure.models_len + 1,
            .decks_cap = measure.decks_len + 1,
            .models_at = models_at,
            .decks_at = decks_at,
            .crt_at = crt_at,
            .failed = false,
        };
        err = anki_sql_scan_table(db, "col", on_col_row, &stage);
        if (err != ESP_OK || stage.failed) {
            ESP_LOGW(TAG, "col scan failed: %s", esp_err_to_name(err));
            free(models_buf);
            free(decks_buf);
            goto fail;
        }
        // v1 调度器（schema < 18）的复习 due 是相对集合创建日的天数。
        // The v1 scheduler (schema < 18) stores review due in days since crt.
        uint32_t schema = anki_sql_user_version(db);
        out->due_epoch_day = schema >= 18 ? 1 : 0;
        out->crt_day = crt > 0 ? crt / 86400 : 0;
        bool parsed = v2_parse_col(&build, models_buf, models_size, decks_buf, decks_size);
        free(models_buf);
        free(decks_buf);
        if (!parsed) {
            ESP_LOGW(TAG, "v2 col parse failed");
            goto fail;
        }
    }

    {
        rows_build_t build = {.imp = out, .failed = false};
        err = anki_sql_scan_table(db, "notes", on_note, &build);
        if (err == ESP_OK && !build.failed)
            err = anki_sql_scan_table(db, "cards", on_card, &build);
        if (err != ESP_OK || build.failed) {
            ESP_LOGW(TAG, "notes/cards parse failed: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    anki_sql_close(db);
    free(collection);
    if (out->deck_count == 0 || out->card_count == 0) {
        ESP_LOGW(TAG, "empty deck import");
        anki_apkg_free(out);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "imported %d decks, %d notetypes, %d notes, %d cards from %s",
             out->deck_count, out->notetype_count, out->note_count, out->card_count, path);
    return ESP_OK;

fail:
    anki_sql_close(db);
    free(collection);
    anki_apkg_free(out);
    return err != ESP_OK ? err : ESP_FAIL;
}
