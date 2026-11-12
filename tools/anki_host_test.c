/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 导入、渲染与存储的宿主测试。夹具由 gen_anki_fixtures.py 生成。
 * English: Host tests for Anki import, rendering and storage. Fixtures come
 * from gen_anki_fixtures.py.
 *
 * 冻结：只验证解析与调度逻辑；不替代真机刷屏验证。
 * Frozen: verifies parsing and scheduling logic only; no substitute for
 * on-device display validation.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anki_apkg.h"
#include "anki_json.h"
#include "anki_progress.h"
#include "anki_render.h"
#include "anki_sqlite.h"
#include "anki_store.h"
#include "book_store.h"

// 存储模块依赖 book_store 的挂载助手；宿主测试恒成功。
// The store depends on book_store's mount helper; host tests always succeed.
esp_err_t book_store_ensure_flash(void) { return ESP_OK; }

#ifdef _WIN32
#include <direct.h>
#define test_mkdir(path) _mkdir(path)
#else
#include <sys/types.h>
#define test_mkdir(path) mkdir((path), 0777)
#endif

#define FIX "build/anki-fixtures"

static int failures;
static int checks;

#define CHECK(cond)                                                                               \
    do {                                                                                          \
        ++checks;                                                                                 \
        if (!(cond)) {                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
            ++failures;                                                                           \
        }                                                                                         \
    } while (0)

static void clean_dir(const char* root) {
    // 宿主目录树可能不存在，先逐级建好。/ The host tree may not exist yet; build it.
    char path[320];
    snprintf(path, sizeof(path), "%s", root);
    for (char* p = path + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            test_mkdir(path);
            *p = '/';
        }
    }
    test_mkdir(path);
    DIR* dir = opendir(root);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcasecmp(dot, ".akd") != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        unlink(path);
    }
    closedir(dir);
}

static void clean_deck_dir(void) { clean_dir(ANKI_DIR); }

// ---------------------------------------------------------------- JSON

static void test_json(void) {
    printf("== json ==\n");
    anki_json_value_t* root = anki_json_parse("{\"a\": 1, \"b\": \"x\\ty\", \"c\": [true, null, 2.5]}", strlen("{\"a\": 1, \"b\": \"x\\ty\", \"c\": [true, null, 2.5]}"));
    CHECK(root != NULL);
    if (root) {
        CHECK(anki_json_type(root) == ANKI_JSON_OBJECT);
        CHECK(anki_json_member_count(root) == 3);
        CHECK(anki_json_number(anki_json_get(root, "a")) == 1);
        const char* b = anki_json_string(anki_json_get(root, "b"), NULL);
        CHECK(b && strcmp(b, "x\ty") == 0);
        const anki_json_value_t* arr = anki_json_get(root, "c");
        CHECK(anki_json_size(arr) == 3);
        CHECK(anki_json_bool(anki_json_at(arr, 0)) == true);
        CHECK(anki_json_type(anki_json_at(arr, 1)) == ANKI_JSON_NULL);
        CHECK(anki_json_number(anki_json_at(arr, 2)) == 2.5);
        anki_json_free(root);
    }
    // 代理对应成 UTF-8。/ Surrogate pairs become UTF-8.
    root = anki_json_parse("{\"k\": \"\\ud83d\\ude00\"}", strlen("{\"k\": \"\\ud83d\\ude00\"}"));
    CHECK(root != NULL);
    if (root) {
        const char* k = anki_json_string(anki_json_get(root, "k"), NULL);
        CHECK(k && strcmp(k, "\xf0\x9f\x98\x80") == 0);
        anki_json_free(root);
    }
    // 数字键对象迭代（v2 col.models 形状）。/ Numeric-keyed iteration (v2 col.models shape).
    root = anki_json_parse("{\"1700000000000\": {\"name\": \"基础\"}, \"42\": {\"name\": \"其他\"}}",
                           strlen("{\"1700000000000\": {\"name\": \"基础\"}, \"42\": {\"name\": \"其他\"}}"));
    CHECK(root != NULL);
    if (root) {
        CHECK(anki_json_member_count(root) == 2);
        const char* key = anki_json_member_key(root, 0, NULL);
        CHECK(key && strcmp(key, "1700000000000") == 0);
        const anki_json_value_t* first = anki_json_member_value(root, 0);
        const char* name = anki_json_string(anki_json_get(first, "name"), NULL);
        CHECK(name && strcmp(name, "基础") == 0);
        anki_json_free(root);
    }
    // 非法输入全部拒绝。/ Every malformed input is rejected.
    CHECK(anki_json_parse("", 0) == NULL);
    CHECK(anki_json_parse("{", 1) == NULL);
    CHECK(anki_json_parse("{\"a\":}", 6) == NULL);
    CHECK(anki_json_parse("[1,]", 4) == NULL);
    CHECK(anki_json_parse("{} trailing", 10) == NULL);
    CHECK(anki_json_parse("\"unterminated", 13) == NULL);
    CHECK(anki_json_parse("{\"a\": \"\\q\"}", 10) == NULL);
    CHECK(anki_json_parse("{\"a\": \"\\ud800\"}", 14) == NULL); // 孤立高代理 / lone high surrogate
    CHECK(anki_json_parse("{\"a\": 1}{\"b\": 2}", 15) == NULL);
}

// ---------------------------------------------------------------- SQLite

static int note_rows_seen;
static int64_t first_note_id;

static bool count_notes(int64_t rowid, const anki_sql_value_t* v, int count, void* ctx) {
    (void)ctx;
    if (count < 7) return true;
    // INTEGER PRIMARY KEY 存 NULL，真值是 rowid。/ INTEGER PRIMARY KEY stores NULL; the rowid is the value.
    if (note_rows_seen == 0) first_note_id = v[0].type == ANKI_SQL_INT ? v[0].i : rowid;
    ++note_rows_seen;
    return true;
}

static void test_sqlite(void) {
    printf("== sqlite ==\n");
    FILE* f = fopen(FIX "/basic.anki2", "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc((size_t)size);
    CHECK(fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    anki_sqlite_t* db = NULL;
    CHECK(anki_sql_open(data, (size_t)size, &db) == ESP_OK);
    if (db) {
        CHECK(anki_sql_table_root(db, "col") != 0);
        CHECK(anki_sql_table_root(db, "notes") != 0);
        CHECK(anki_sql_table_root(db, "cards") != 0);
        CHECK(anki_sql_table_root(db, "revlog") != 0);
        CHECK(anki_sql_table_root(db, "graves") == 0);
        const char* const* columns = NULL;
        int n = anki_sql_table_columns(db, "col", &columns);
        CHECK(n == 13);
        if (n == 13) {
            CHECK(strcmp(columns[9], "models") == 0);
            CHECK(strcmp(columns[10], "decks") == 0);
        }
        n = anki_sql_table_columns(db, "notes", &columns);
        CHECK(n == 11);
        note_rows_seen = 0;
        CHECK(anki_sql_scan_table(db, "notes", count_notes, NULL) == ESP_OK);
        CHECK(note_rows_seen == 3);
        CHECK(first_note_id == 1700000000001);
        CHECK(anki_sql_scan_table(db, "missing", count_notes, NULL) == ESP_ERR_NOT_FOUND);
        anki_sql_close(db);
    }
    // 坏文件拒绝。/ Bad files are rejected.
    anki_sqlite_t* bad = NULL;
    CHECK(anki_sql_open((const uint8_t*)"not a database at all........", 31, &bad) != ESP_OK);
    CHECK(anki_sql_open(data, 40, &bad) == ESP_ERR_INVALID_SIZE);
    free(data);

    // 大集合：溢出页 + 多页 b-树。/ Big collection: overflow pages and a multi-page b-tree.
    f = fopen(FIX "/big.anki2", "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)size);
    CHECK(fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    db = NULL;
    CHECK(anki_sql_open(data, (size_t)size, &db) == ESP_OK);
    if (db) {
        note_rows_seen = 0;
        CHECK(anki_sql_scan_table(db, "notes", count_notes, NULL) == ESP_OK);
        CHECK(note_rows_seen == 120);
        anki_sql_close(db);
    }
    free(data);

    // WAL 模式拒绝。/ WAL mode is rejected.
    f = fopen(FIX "/wal.anki2", "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)size);
    CHECK(fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    db = NULL;
    CHECK(anki_sql_open(data, (size_t)size, &db) == ESP_ERR_NOT_SUPPORTED);
    CHECK(db == NULL);
    free(data);
}

// ---------------------------------------------------------------- 导入

static void test_import_v2(void) {
    printf("== import v2 ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/basic-v2.apkg", &imp) == ESP_OK);
    CHECK(imp.deck_count == 2); // Default + 英语单词
    CHECK(imp.notetype_count == 1);
    CHECK(imp.note_count == 3);
    CHECK(imp.card_count == 3);
    CHECK(imp.due_epoch_day == 1);
    if (imp.deck_count == 2) {
        CHECK(strcmp(imp.decks[0].name, "Default") == 0);
        CHECK(strcmp(imp.decks[1].name, "英语单词") == 0);
    }
    if (imp.notetype_count == 1) {
        CHECK(imp.notetypes[0].field_count == 3);
        CHECK(strcmp(imp.notetypes[0].fields[0], "正面") == 0);
        CHECK(imp.notetypes[0].template_count == 1);
        CHECK(strcmp(imp.notetypes[0].templates[0].qfmt, "<div class=question>{{正面}}</div>") == 0);
    }
    if (imp.card_count == 3) {
        CHECK(imp.cards[0].type == 0 && imp.cards[0].queue == 0);
        CHECK(imp.cards[1].type == 1 && imp.cards[1].queue == 1);
        CHECK(imp.cards[2].type == 2 && imp.cards[2].queue == 2);
        CHECK(imp.cards[2].due == 20000);
        CHECK(imp.cards[2].ivl == 10);
        CHECK(imp.cards[2].reps == 3);
    }
    anki_apkg_free(&imp);
}

static void test_import_v1(void) {
    printf("== import v1 scheduler ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/v1sched-v2.apkg", &imp) == ESP_OK);
    CHECK(imp.due_epoch_day == 0);
    CHECK(imp.crt_day == 19723); // 1704067200 / 86400
    CHECK(imp.card_count == 1);
    if (imp.card_count == 1) CHECK(imp.cards[0].due == 30);
    anki_apkg_free(&imp);
}

static void test_import_v3(void) {
    printf("== import v3 ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/multi-v3.apkg", &imp) == ESP_OK);
    CHECK(imp.deck_count == 2); // 过滤卡组被跳过 / the filtered deck is skipped
    if (imp.deck_count == 2) {
        CHECK(strcmp(imp.decks[0].name, "中文卡组") == 0);
        CHECK(strcmp(imp.decks[1].name, "英文卡组") == 0);
    }
    CHECK(imp.card_count == 3); // 过滤卡组的卡片不导入 / filtered-deck cards are skipped
    CHECK(imp.notetype_count == 2);
    if (imp.notetype_count == 2) {
        CHECK(strcmp(imp.notetypes[0].name, "基础") == 0);
        CHECK(imp.notetypes[0].field_count == 2);
        CHECK(imp.notetypes[0].template_count == 1);
        if (imp.notetypes[0].template_count == 1) {
            CHECK(strcmp(imp.notetypes[0].templates[0].qfmt, "<div>{{正面}}</div>") == 0);
        }
        CHECK(strcmp(imp.notetypes[1].name, "进阶") == 0);
        CHECK(imp.notetypes[1].field_count == 1);
        CHECK(imp.notetypes[1].template_count == 1);
        if (imp.notetypes[1].template_count == 1) {
            CHECK(strcmp(imp.notetypes[1].templates[0].qfmt, "<div>{{例句}}</div>") == 0);
        }
    }
    anki_apkg_free(&imp);
}

static void test_import_big(void) {
    printf("== import big ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/big-v2.apkg", &imp) == ESP_OK);
    CHECK(imp.card_count == 120);
    CHECK(imp.note_count == 120);
    CHECK(imp.notetype_count == 1);
    if (imp.notetype_count == 1) {
        // 长模板经溢出页完整取出。/ The long template survives overflow pages.
        CHECK(strlen(imp.notetypes[0].templates[0].qfmt) > 10000);
    }
    anki_apkg_free(&imp);
}

static void test_import_bare(void) {
    printf("== import bare sqlite ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/bare.anki2", &imp) == ESP_OK);
    CHECK(imp.deck_count == 1);
    CHECK(imp.card_count == 1);
    anki_apkg_free(&imp);
}

// 双笔记类型回归：anki_alloc 在宿主上填充 0xAA（模拟真机未初始化的 PSRAM）。
// 若追加槽未清零，template_count/模板指针是 0xAA…，导入或释放即崩。
// / Two-notetype regression: anki_alloc poisons host allocations with 0xAA
// (mimicking the device's uninitialized PSRAM). With unzeroed slots the
// counts/pointers are 0xAA… and import or release crashes.
static void test_unzeroed_slots(void) {
    printf("== import two-notetype decks with poisoned allocations ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/dual-v2.apkg", &imp) == ESP_OK);
    CHECK(imp.notetype_count == 2);
    for (int i = 0; i < imp.notetype_count; ++i) {
        CHECK(imp.notetypes[i].template_count == 1);
        CHECK(imp.notetypes[i].templates[0].qfmt != NULL);
        CHECK(imp.notetypes[i].templates[0].afmt != NULL);
    }
    CHECK(imp.card_count == 2);
    anki_apkg_free(&imp);

    anki_import_t v3 = {0};
    CHECK(anki_apkg_import(FIX "/multi-v3.apkg", &v3) == ESP_OK);
    CHECK(v3.notetype_count == 2);
    for (int i = 0; i < v3.notetype_count; ++i) {
        CHECK(v3.notetypes[i].template_count == 1);
        CHECK(v3.notetypes[i].templates[0].qfmt != NULL);
        CHECK(v3.notetypes[i].templates[0].afmt != NULL);
    }
    anki_apkg_free(&v3);
}

// 现代导出回归：新版 Anki（23.10+）导出的 apkg 同时带 collection.anki2 存根
// 与 collection.anki21 真库，导入器必须选 anki21——此前误选存根导致真卡组
// 只剩 Default。/ Modern-export regression: a modern Anki (23.10+) .apkg
// carries both the collection.anki2 stub and the collection.anki21 real
// collection; the importer must pick anki21 — it used to pick the stub,
// reducing the import to a lone Default deck.
static void test_modern_export(void) {
    printf("== import modern dual-entry export ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/modern-dual.apkg", &imp) == ESP_OK);
    CHECK(imp.deck_count == 1); // 存根的 Default 不算 / the stub's Default does not count
    if (imp.deck_count == 1) {
        CHECK(strcmp(imp.decks[0].name, "无线电考试") == 0);
    }
    CHECK(imp.card_count == 2);
    CHECK(imp.note_count == 2);
    CHECK(imp.notetype_count == 1);
    if (imp.notetype_count == 1) {
        CHECK(strcmp(imp.notetypes[0].name, "无线电问答") == 0);
    }
    // 两遍扫描回归：col 暂存改为「先测量再按需分配」，crt_day 仍须逐位来自
    // col.crt（1704067200 / 86400），models/decks 解析结果不变。
    // / Two-pass regression: col staging now measures first and allocates on
    // demand, yet crt_day must still match col.crt bit for bit
    // (1704067200 / 86400) and the models/decks parse stays unchanged.
    CHECK(imp.crt_day == 19723);
    anki_apkg_free(&imp);
}

static void test_rejects(void) {
    printf("== rejects ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/garbage.apkg", &imp) != ESP_OK);
    CHECK(anki_apkg_import(FIX "/wal-v2.apkg", &imp) != ESP_OK);
    CHECK(anki_apkg_import(FIX "/nocollection.apkg", &imp) != ESP_OK);
    CHECK(anki_apkg_import(FIX "/missing-file.apkg", &imp) == ESP_ERR_NOT_FOUND);
}

// ---------------------------------------------------------------- 渲染

static void test_render(void) {
    printf("== render ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/basic-v2.apkg", &imp) == ESP_OK);
    anki_collection_t col;
    anki_collection_from_import(&imp, &col);
    static char front[ANKI_CARD_TEXT_MAX], back[ANKI_CARD_TEXT_MAX];
    if (imp.card_count == 3) {
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strcmp(front, "apple") == 0);
        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        // 背面含 FrontSide 与答案。/ The back carries FrontSide and the answer.
        CHECK(strstr(back, "apple") != NULL);
        CHECK(strstr(back, "苹果") != NULL);
        CHECK(strstr(back, "&nbsp;") == NULL); // 实体已解码 / entities decoded
    }
    anki_apkg_free(&imp);

    // 条件段：有例句才显示。/ Conditional: the example shows only when present.
    imp = (anki_import_t){0};
    CHECK(anki_apkg_import(FIX "/conditional-v2.apkg", &imp) == ESP_OK);
    if (imp.card_count == 2) {
        anki_collection_from_import(&imp, &col);
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strstr(front, "This is an example.") != NULL);
        CHECK(anki_render_front(&col, &imp.cards[1], front, sizeof(front)) == ESP_OK);
        CHECK(strcmp(front, "只有问题") == 0);
        CHECK(strstr(front, "example") == NULL);
    }
    anki_apkg_free(&imp);

    // 完形：ord=N 考的是 c(N+1)——正面把它挖空，背面揭示它，其余序号保留原文。
    // Cloze: ord=N asks about c(N+1) — blanked on the front, revealed on the
    // back, every other index keeps its source text.
    imp = (anki_import_t){0};
    CHECK(anki_apkg_import(FIX "/cloze-v2.apkg", &imp) == ESP_OK);
    if (imp.card_count == 2) {
        anki_collection_from_import(&imp, &col);
        // 卡片 0 考 c1 / card 0 asks c1
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strstr(front, "法国") == NULL);
        CHECK(strstr(front, "欧洲") != NULL);
        CHECK(strstr(front, "[…]") != NULL);
        // 卡片 1 考 c2 / card 1 asks c2
        CHECK(anki_render_front(&col, &imp.cards[1], front, sizeof(front)) == ESP_OK);
        CHECK(strstr(front, "欧洲") == NULL);
        CHECK(strstr(front, "法国") != NULL);
        CHECK(strstr(front, "[…]") != NULL);
        // 背面揭示这张卡要考的答案 / the back reveals the answer it asks for
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        CHECK(strstr(back, "补充说明") != NULL);
        CHECK(strstr(back, "法国") != NULL);
    }
    anki_apkg_free(&imp);
}

// 选择题版面：正面与背面同构。正面 = 元信息行（模板里的 I / J / P）+ 
// `【题型】题干` + 空行 + 字母选项；背面再加错项反色标记、答案与备注。
// 模板里只写在 id 属性上的字母与 `<li class="hidden">placeholder</li>` 都不进正文。
// Choice layout: the front mirrors the back. Front = the meta line (I / J / P
// from the template) plus `【type】question`, a blank line and lettered
// options; the back adds the inverted wrong options, the answer and the note.
// Letters that only lived in id attributes and hidden placeholder items
// (`<li class="hidden">placeholder</li>`) never reach the text.
static void test_choice_render(void) {
    printf("== choice render ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/choice-v2.apkg", &imp) == ESP_OK);
    anki_collection_t col;
    anki_collection_from_import(&imp, &col);
    static char front[ANKI_CARD_TEXT_MAX], back[ANKI_CARD_TEXT_MAX];
    if (imp.card_count == 2) {
        // 单选卡：T="A"，正面四个选项带字母，错误项 B/C/D 在背面反色。
        // Single-choice card: T="A"; the front carries lettered options and
        // the wrong ones B/C/D are inverted on the back.
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strcmp(front,
                     "MC1-0001 / LY0001 / 1.1.1\n"
                     "【单选】我国依法负责对业余无线电台实施监督管理的机构是：\n"
                     "\n"
                     "A. 国家无线电管理机构和省、自治区、直辖市无线电管理机构\n"
                     "B. 在国家或地方民政部门注册的业余无线电协会\n"
                     "C. 国家体育管理机构和地方体育管理机构\n"
                     "D. 国家和地方公安部门\n") == 0);
        CHECK(strstr(front, "placeholder") == NULL);  // 隐藏占位条目不泄漏 / no placeholder leak
        CHECK(strstr(front, "\x01") == NULL);         // 正面无强调标记 / no marker on the front
        CHECK(strstr(front, "【单选】") != NULL);       // 题型前缀 / type prefix
        CHECK(strstr(front, "\nA. ") != NULL);
        CHECK(strstr(front, "\nB. ") != NULL);
        CHECK(strstr(front, "\nC. ") != NULL);
        CHECK(strstr(front, "\nD. ") != NULL);

        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        // 整篇背面逐行钉死：题干（带题型前缀）、空行、选项（错误项行首带
        // \x01）、空行、答案行（不带标记），注释为空故无备注行。
        // The whole back is pinned line by line: the question with the type
        // prefix, a blank line, options (wrong ones lead with \x01), a blank
        // line and the answer (no marker); the empty note means no note line.
        CHECK(strcmp(back,
                     "【单选】我国依法负责对业余无线电台实施监督管理的机构是：\n"
                     "\n"
                     "A. 国家无线电管理机构和省、自治区、直辖市无线电管理机构\n"
                     "\x01"
                     "B. 在国家或地方民政部门注册的业余无线电协会\n"
                     "\x01"
                     "C. 国家体育管理机构和地方体育管理机构\n"
                     "\x01"
                     "D. 国家和地方公安部门\n"
                     "\n"
                     "答案：A\n") == 0);
        CHECK(strstr(back, "\nA. ") != NULL);        // 正确项常规 / correct option plain
        CHECK(strstr(back, "\n\x01" "A. ") == NULL);  // 正确项不反色 / not inverted
        CHECK(strstr(back, "\n\x01" "B. ") != NULL);  // 错误项反色 / wrong option inverted
        CHECK(strstr(back, "\n\x01" "C. ") != NULL);
        CHECK(strstr(back, "\n\x01" "D. ") != NULL);
        CHECK(strstr(back, "\n答案：A") != NULL);      // 答案行常规 / answer line plain
        CHECK(strstr(back, "\n\x01" "答案") == NULL);  // 答案行不反色 / answer not inverted
        CHECK(strstr(back, "：\n\nA. ") != NULL);      // 题干后有空行 / blank line after question
        CHECK(strstr(back, "门\n\n答案：A") != NULL);  // 选项后有空行 / blank line after options
        CHECK(strstr(back, "备注：") == NULL);          // 注释为空无备注行 / no note line

        // 多选卡：T="AC"，正面/背面第 1 行都是【多选】，A/C 常规，B/D 反色。
        // Multiple-choice card: T="AC"; both first lines read 【多选】, A and C
        // stay plain while B and D are inverted.
        CHECK(anki_render_front(&col, &imp.cards[1], front, sizeof(front)) == ESP_OK);
        CHECK(strcmp(front,
                     "MC2-0002 / LY0002 / 1.1.1\n"
                     "【多选】我国专门针对无线电管理的行政法规及其制定机构是：\n"
                     "\n"
                     "A. 《中华人民共和国无线电管理条例》\n"
                     "B. 《中华人民共和国无线电管理办法》\n"
                     "C. 国务院和中央军委\n"
                     "D. 工业和信息化部\n") == 0);
        CHECK(strstr(front, "placeholder") == NULL);
        CHECK(strstr(front, "\x01") == NULL);
        CHECK(strstr(front, "【多选】") != NULL);

        CHECK(anki_render_back(&col, &imp.cards[1], front, back, sizeof(back)) == ESP_OK);
        CHECK(strcmp(back,
                     "【多选】我国专门针对无线电管理的行政法规及其制定机构是：\n"
                     "\n"
                     "A. 《中华人民共和国无线电管理条例》\n"
                     "\x01"
                     "B. 《中华人民共和国无线电管理办法》\n"
                     "C. 国务院和中央军委\n"
                     "\x01"
                     "D. 工业和信息化部\n"
                     "\n"
                     "答案：AC\n"
                     "备注：参见《条例》第一章\n") == 0);
        CHECK(strstr(back, "\nA. ") != NULL);
        CHECK(strstr(back, "\nC. ") != NULL);
        CHECK(strstr(back, "\n\x01" "A. ") == NULL);
        CHECK(strstr(back, "\n\x01" "C. ") == NULL);
        CHECK(strstr(back, "\n\x01" "B. ") != NULL);
        CHECK(strstr(back, "\n\x01" "D. ") != NULL);
        CHECK(strstr(back, "\n答案：AC") != NULL);
        CHECK(strstr(back, "\n\x01" "答案") == NULL);
        CHECK(strstr(back, "\n备注：参见《条例》第一章") != NULL);
    }
    anki_apkg_free(&imp);

    // 回归保护：普通（非选择题）夹具正面与背面都不含强调标记，也不加题型前缀。
    // Regression guard: an ordinary (non-choice) fixture carries no emphasis
    // marker on either side and gets no type prefix.
    imp = (anki_import_t){0};
    CHECK(anki_apkg_import(FIX "/basic-v2.apkg", &imp) == ESP_OK);
    anki_collection_from_import(&imp, &col);
    if (imp.card_count == 3) {
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strcmp(front, "apple") == 0);  // 普通卡走模板路径，行为不变 / template path unchanged
        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        CHECK(strstr(back, "\x01") == NULL);
        CHECK(strstr(back, "【") == NULL);
    }
    anki_apkg_free(&imp);
}

// ---------------------------------------------------------------- 存储

static int list_seen;
static anki_deck_summary_t list_first;

static bool visit_summary(const anki_deck_summary_t* summary, void* ctx) {
    (void)ctx;
    if (list_seen == 0) list_first = *summary;
    ++list_seen;
    return true;
}

static void test_store(void) {
    printf("== store ==\n");
    clean_deck_dir();
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    CHECK(imported == 2); // Default 与英语单词都落盘 / both decks land on disk

    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 2);
    if (list_seen == 2) {
        // Default 无卡（0），英语单词三张。/ Default holds no cards (0); the word deck holds three.
        CHECK(list_first.total == 0 || list_first.total == 3);
    }

    // 英语单词卡组：载入、会话、评分、保存、重载。
    // The word deck: load, session, grade, save, reload.
    int64_t target = 0;
    for (int i = 0; i < list_seen; ++i) {
        (void)i;
    }
    // 找到有三张卡的那个。/ Find the deck with three cards.
    list_seen = 0;
    anki_store_list(visit_summary, NULL);
    // 英语单词是第二个写入的；直接按名找。/ The word deck is written second; find it by name.
    // 通过遍历拿 id。/ Walk again for the id.
    struct finder {
        int64_t id;
        bool found;
    } finder = {0, false};
    (void)finder;
    // 简单方式：两个 id 都试，取载入后 card_count==3 的。/ Try both ids and keep the one with three cards.
    int64_t ids[2] = {0, 0};
    {
        DIR* dir = opendir(ANKI_DIR);
        CHECK(dir != NULL);
        int at = 0;
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL && at < 2) {
                const char* dot = strrchr(entry->d_name, '.');
                if (!dot || strcasecmp(dot, ".akd") != 0) continue;
                ids[at++] = strtoll(entry->d_name, NULL, 10);
            }
            closedir(dir);
        }
        CHECK(at == 2);
    }
    anki_deck_t deck = {0};
    for (int i = 0; i < 2; ++i) {
        if (ids[i] == 0) continue;
        if (anki_store_load(ids[i], &deck) == ESP_OK && deck.card_count == 3) {
            target = ids[i];
            break;
        }
        anki_store_unload(&deck);
    }
    CHECK(target != 0);
    if (target == 0) return;

    CHECK(strcmp(deck.name, "英语单词") == 0);
    CHECK(deck.notetype_count == 1);
    CHECK(deck.note_count == 3);
    int64_t day = anki_store_sync_day(&deck);
    CHECK(day == anki_day_now() || anki_day_now() == 0);

    int indices[ANKI_SESSION_MAX];
    int count = anki_store_session(&deck, indices, ANKI_SESSION_MAX);
    CHECK(count == 3); // 1 新 + 1 学习（今日）+ 1 到期复习 / 1 new + 1 learning + 1 due review

    // 新卡评 Good：转复习、隔 1 天。/ Good on the new card: review, 1 day out.
    int new_index = -1;
    for (int i = 0; i < count; ++i) {
        if (deck.cards[indices[i]].queue == 0) new_index = indices[i];
    }
    CHECK(new_index >= 0);
    if (new_index >= 0) {
        anki_schedule(&deck, new_index, ANKI_GRADE_GOOD);
        CHECK(deck.cards[new_index].type == 2);
        CHECK(deck.cards[new_index].queue == 2);
        CHECK(deck.cards[new_index].ivl == 1);
        CHECK(deck.cards[new_index].due == day + 1);
        CHECK(deck.cards[new_index].reps == 1);
        CHECK(anki_store_save_card(&deck, new_index) == ESP_OK);
    }

    // 复习卡评 Good：间隔按 2.5 系数放大。/ Good on the review card: 2.5x ease growth.
    int review_index = -1;
    for (int i = 0; i < deck.card_count; ++i) {
        if (deck.cards[i].type == 2 && deck.cards[i].ivl == 10) review_index = i;
    }
    CHECK(review_index >= 0);
    if (review_index >= 0) {
        anki_schedule(&deck, review_index, ANKI_GRADE_GOOD);
        CHECK(deck.cards[review_index].ivl == 25);
        CHECK(deck.cards[review_index].due == day + 25);
        CHECK(anki_store_save_card(&deck, review_index) == ESP_OK);
    }

    // 学习卡评 Again：回学习列、记一次。/ Again on the learning card: back to learning.
    int learn_index = -1;
    for (int i = 0; i < deck.card_count; ++i) {
        if (deck.cards[i].queue == 1) learn_index = i;
    }
    CHECK(learn_index >= 0);
    if (learn_index >= 0) {
        anki_schedule(&deck, learn_index, ANKI_GRADE_AGAIN);
        CHECK(deck.cards[learn_index].type == 1);
        CHECK(deck.cards[learn_index].ivl == 0);
        CHECK(anki_store_save_card(&deck, learn_index) == ESP_OK);
    }

    // 重载验证进度落盘。/ Reload to confirm the progress persisted.
    anki_deck_t reloaded = {0};
    CHECK(anki_store_load(target, &reloaded) == ESP_OK);
    CHECK(reloaded.card_count == 3);
    if (reloaded.card_count == 3 && new_index >= 0) {
        bool found_new = false;
        for (int i = 0; i < reloaded.card_count; ++i) {
            if (reloaded.cards[i].id == deck.cards[new_index].id) {
                found_new = true;
                CHECK(reloaded.cards[i].type == 2);
                CHECK(reloaded.cards[i].ivl == 1);
                CHECK(reloaded.cards[i].reps == 1);
            }
        }
        CHECK(found_new);
    }
    anki_store_unload(&reloaded);

    // 再次会话：评过 Good 的新卡今天不再出现。/ The graded new card leaves today's session.
    count = anki_store_session(&deck, indices, ANKI_SESSION_MAX);
    bool new_still_due = false;
    for (int i = 0; i < count; ++i) {
        if (deck.cards[indices[i]].queue == 0) new_still_due = true;
    }
    CHECK(!new_still_due || deck.cards[new_index].due <= day);

    anki_store_unload(&deck);

    // 删除后列表为空。/ The list is empty after deletion.
    for (int i = 0; i < 2; ++i) {
        if (ids[i] != 0) CHECK(anki_store_delete(ids[i]) == ESP_OK);
    }
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 0);
}

// ---------------------------------------------------------------- 双根

static void test_roots(void) {
    printf("== roots ==\n");
    // 纯映射：TF 就绪 → TF 根，否则内部闪存根。/ Pure mapping: TF-ready picks
    // the TF root, anything else the internal-flash root.
    CHECK(strcmp(anki_root_from_tf_ready(false), ANKI_DIR) == 0);
    CHECK(strcmp(anki_root_from_tf_ready(true), ANKI_DIR_TF) == 0);
    CHECK(strcmp(anki_root_from_tf_ready(true), ANKI_DIR) != 0);
    // 宿主不探测 TF：活动根恒为 ANKI_DIR（-DANKI_DIR 机制不变）。
    // / The host probes no TF, so the active root is always ANKI_DIR (the
    // -DANKI_DIR mechanism is unchanged).
    CHECK(strcmp(anki_store_active_root(), ANKI_DIR) == 0);
}

/// 收集某根下 .akd 的卡组 id。/ Collect deck ids from a root's .akd files.
static int collect_deck_ids(const char* root, int64_t* ids, int max) {
    DIR* dir = opendir(root);
    if (!dir) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && count < max) {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcasecmp(dot, ".akd") != 0) continue;
        ids[count++] = strtoll(entry->d_name, NULL, 10);
    }
    closedir(dir);
    return count;
}

/// 整文件复制，供双根测试搭建“同 id 两份”。/ Whole-file copy so the dual-root
/// test can stage two copies of one id.
static bool copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[4096];
    size_t got;
    bool ok = true;
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    ok = fclose(out) == 0 && ok;
    fclose(in);
    return ok;
}

/// 把文件截断到前 keep 字节（移植式重写，不依赖 ftruncate）。/ Truncate a
/// file to its first keep bytes by rewriting it, so no ftruncate is needed.
static bool truncate_file(const char* path, long keep) {
    if (keep <= 0) return false;
    FILE* in = fopen(path, "rb");
    if (!in) return false;
    char buffer[4096];
    size_t want = (size_t)keep < sizeof(buffer) ? (size_t)keep : sizeof(buffer);
    size_t got = fread(buffer, 1, want, in);
    fclose(in);
    if (got == 0) return false;
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    bool ok = fwrite(buffer, 1, got, out) == got;
    return fclose(out) == 0 && ok;
}

static void test_dual_root(void) {
    printf("== dual root ==\n");
    clean_dir(ANKI_DIR);
    clean_dir(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    CHECK(imported == 2);

    // 宿主活动根是 ANKI_DIR：两个卡组都落在那里，TF 根为空。
    // / The host's active root is ANKI_DIR, so both decks land there and the
    // TF root stays empty.
    int64_t ids[4] = {0, 0, 0, 0};
    int id_count = collect_deck_ids(ANKI_DIR, ids, 4);
    CHECK(id_count == 2);
    CHECK(collect_deck_ids(ANKI_DIR_TF, ids, 4) == 0);
    if (id_count != 2) return;

    char flash_path[320], tf_path[320];
    snprintf(flash_path, sizeof(flash_path), "%s/%lld.akd", ANKI_DIR, (long long)ids[0]);
    snprintf(tf_path, sizeof(tf_path), "%s/%lld.akd", ANKI_DIR_TF, (long long)ids[0]);
    // 同 id 再放一份到 TF 根。/ Stage a second copy of the same id on the TF root.
    CHECK(copy_file(flash_path, tf_path));

    // 列表按 id 去重：仍只有两个卡组。/ The list dedupes by id: still two decks.
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 2);

    // 载入走活动根（ANKI_DIR）。/ Load prefers the active root (ANKI_DIR).
    anki_deck_t deck = {0};
    CHECK(anki_store_load(ids[0], &deck) == ESP_OK);
    CHECK(strncmp(deck.path, ANKI_DIR, strlen(ANKI_DIR)) == 0);
    anki_store_unload(&deck);

    // 删掉活动根那份，TF 根仍在：载入回落到 TF 根，path 记录实际读到的位置。
    // / Remove the active-root copy; the TF copy remains and load falls back
    // to it, path recording where it was actually read from.
    CHECK(unlink(flash_path) == 0);
    CHECK(anki_store_load(ids[0], &deck) == ESP_OK);
    CHECK(strncmp(deck.path, ANKI_DIR_TF, strlen(ANKI_DIR_TF)) == 0);
    anki_store_unload(&deck);

    // 损坏文件遮蔽修复：活动根放截断的同 id .akd，另一根保留完好副本。
    // / Corrupted-file shadowing fix: stage a truncated same-id .akd on the
    // active root while the other root keeps a healthy copy.
    CHECK(copy_file(tf_path, flash_path));
    // 先记下完好副本的卡片数作参照（卡组可能 0 卡，不能硬编码非零）。
    // / Record the healthy copy's card count as reference (a deck may hold
    // zero cards, so a hardcoded non-zero expectation would be wrong).
    CHECK(anki_store_load(ids[0], &deck) == ESP_OK);
    int healthy_cards = deck.card_count;
    anki_store_unload(&deck);
    CHECK(truncate_file(flash_path, 32)); // 少于 64 字节头，header_ok 必败 / under the 64-byte header, header_ok must fail

    // 先收掉 ids[1]，让随后列表里只有 ids[0]，可对汇总逐项断言。
    // / Drop ids[1] first so the following list holds ids[0] alone and the
    // summary can be asserted field by field.
    CHECK(anki_store_delete(ids[1]) == ESP_OK);

    // 列表仍显示该卡组（去重表只收头校验通过的 id），且汇总与完好副本一致。
    // / The list still shows the deck (the dedup table only keeps ids whose
    // header checks out), and the summary matches the healthy copy.
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 1);
    if (list_seen == 1) {
        CHECK(list_first.deck_id == ids[0]);
        CHECK(list_first.total == healthy_cards);
    }

    // 载入跳过活动根的损坏副本，从 TF 根读到完好副本。
    // / Load skips the damaged active-root copy and reads the healthy copy
    // from the TF root.
    CHECK(anki_store_load(ids[0], &deck) == ESP_OK);
    CHECK(strncmp(deck.path, ANKI_DIR_TF, strlen(ANKI_DIR_TF)) == 0);
    CHECK(deck.card_count == healthy_cards); // 读到的是完好副本 / the healthy copy was read
    anki_store_unload(&deck);

    // 删除同时清理两个根：幽灵卡组不回来。/ Delete clears both roots: no ghost.
    CHECK(anki_store_delete(ids[0]) == ESP_OK);
    struct stat st;
    CHECK(stat(tf_path, &st) != 0);
    CHECK(stat(flash_path, &st) != 0);

    // 收尾：删掉另一个卡组并清空两个根，避免影响后续测试。
    // / Wrap up: delete the other deck and empty both roots so later tests
    // stay unaffected.
    CHECK(anki_store_delete(ids[1]) == ESP_OK);
    clean_dir(ANKI_DIR);
    clean_dir(ANKI_DIR_TF);
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 0);
}

// ---------------------------------------------------------------- 阶段 4 QA 辅助
// Stage-4 QA helpers: byte-level .aks / .akb assertions. Only the test harness
// lives here; no production source is touched by these checks.

/* 彻底清理两根下的所有 Anki 产物（.akd/.aks/.akb/.tmp/.bak 与空的同名目录），
   让每个新测试彼此独立。/ Remove every Anki artifact under both roots so each
   new test starts from a clean, independent state. */
static void qa_clean_all(const char* root) {
    char path[320];
    snprintf(path, sizeof(path), "%s", root);
    for (char* p = path + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            test_mkdir(path);
            *p = '/';
        }
    }
    test_mkdir(path);
    DIR* dir = opendir(root);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", root, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            (void)rmdir(full);
            continue;
        }
        (void)remove(full);
    }
    closedir(dir);
}

static uint32_t qa_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t qa_i64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

static void qa_wu32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void qa_wi64(uint8_t* p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((u >> (8 * i)) & 0xFFu);
}

static uint8_t* qa_slurp(const char* path, size_t* out_len) {
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t* b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return b;
}

static bool qa_write(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static bool qa_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void qa_path(const char* root, int64_t id, const char* suffix,
                    char* out, size_t cap) {
    snprintf(out, cap, "%.200s/%lld%.16s", root, (long long)id, suffix);
}

static void qa_akd_path(const char* root, int64_t id, char* out, size_t cap) {
    qa_path(root, id, ".akd", out, cap);
}

static void qa_aks_path(const char* root, int64_t id, char* out, size_t cap) {
    qa_path(root, id, ANKI_STATE_SUFFIX, out, cap);
}

static void qa_akb_path(const char* root, int64_t id, char* out, size_t cap) {
    qa_path(root, id, ANKI_BACKUP_SUFFIX, out, cap);
}

static void qa_tmp_path(const char* root, int64_t id, char* out, size_t cap) {
    qa_path(root, id, ANKI_DECK_TMP_SUFFIX, out, cap);
}

static void qa_bak_path(const char* root, int64_t id, char* out, size_t cap) {
    qa_path(root, id, ANKI_DECK_BAK_SUFFIX, out, cap);
}

/* 重算 .aks 校验和：篡改某字段后仍能精确命中它自己的校验分支。
   / Recompute the .aks checksum so a mutated field reaches its own check. */
static void qa_aks_reseal(uint8_t* b) {
    qa_wu32(b + 12, anki_fnv1a(b + 16, ANKI_RESUME_BYTES - 16, ANKI_FNV1A_BASIS));
}

static void qa_aks_mutate(const char* path, size_t off, const void* data, size_t n,
                          bool reseal) {
    size_t len = 0;
    uint8_t* b = qa_slurp(path, &len);
    if (!b || len != ANKI_RESUME_BYTES) {
        free(b);
        return;
    }
    memcpy(b + off, data, n);
    if (reseal) qa_aks_reseal(b);
    (void)qa_write(path, b, len);
    free(b);
}

static void qa_aks_set_u32(const char* path, size_t off, uint32_t v, bool reseal) {
    uint8_t b[4];
    qa_wu32(b, v);
    qa_aks_mutate(path, off, b, 4, reseal);
}

static void qa_aks_set_i64(const char* path, size_t off, int64_t v, bool reseal) {
    uint8_t b[8];
    qa_wi64(b, v);
    qa_aks_mutate(path, off, b, 8, reseal);
}

typedef struct {
    int want;
    int64_t id;
} qa_find_t;

static bool qa_visit_find(const anki_deck_summary_t* s, void* ctx) {
    qa_find_t* f = (qa_find_t*)ctx;
    if (s->total == f->want) {
        f->id = s->deck_id;
        return false;
    }
    return true;
}

static int64_t qa_deck_with_cards(int cards) {
    qa_find_t f = {cards, 0};
    if (anki_store_list(qa_visit_find, &f) != ESP_OK) return 0;
    return f.id;
}

/* 每个用例前重建：清两根 → 导入 → 载入 → 同步日 → 建会话 → 写一份有效 .aks。
   / Rebuild a fresh, valid resume state for each case. */
static bool qa_prepare(int64_t* out_id, anki_deck_t* deck, int* idx, int* cnt) {
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    if (anki_store_import(FIX "/basic-v2.apkg", &imported) != ESP_OK || imported != 2) return false;
    int64_t id = qa_deck_with_cards(3);
    if (!id) return false;
    memset(deck, 0, sizeof(*deck));
    if (anki_store_load(id, deck) != ESP_OK) return false;
    anki_store_sync_day(deck);
    *cnt = anki_store_session(deck, idx, ANKI_SESSION_MAX);
    if (*cnt != 3) return false;
    if (anki_store_resume_save(deck, idx, *cnt, 1, 4, 1, 3) != ESP_OK) return false;
    *out_id = id;
    return true;
}

/* I1：除头 day/last_rtc_day(@16..@31) 与每卡 7 个调度字段(@20..@47) 外，
   整文件逐字节不变。/ I1: every byte outside those allowed ranges is identical. */
static size_t qa_i1_diff(const uint8_t* a, const uint8_t* b, size_t len,
                         uint32_t table_at, uint32_t card_count) {
    size_t diff = 0;
    for (size_t k = 0; k < len; ++k) {
        if (k >= (size_t)AKD_OFF_DAY && k < (size_t)AKD_OFF_DAY + 16) continue;
        bool allowed = false;
        if (k >= table_at) {
            size_t rel = k - table_at;
            size_t rec = rel / AKD_CARD_RECORD;
            size_t off = rel % AKD_CARD_RECORD;
            if (rec < card_count && off >= (size_t)AKD_CARD_OFF_TYPE &&
                off < (size_t)AKD_CARD_OFF_RESERVED) {
                allowed = true;
            }
        }
        if (allowed) continue;
        if (a[k] != b[k]) ++diff;
    }
    return diff;
}

static int qa_count_suffix(const char* root, const char* suffix) {
    DIR* dir = opendir(root);
    if (!dir) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        size_t nl = strlen(e->d_name), sl = strlen(suffix);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0) ++n;
    }
    closedir(dir);
    return n;
}

// ---------------------------------------------------------------- QA A. 断点续学 .aks

/* C14 回归契约：.akd 头/卡表偏移常量、格式版本与 FNV-1a 标准向量。
   / Regression contract: .akd offsets, format versions and FNV-1a vectors. */
static void test_qa_contract(void) {
    printf("== qa contract / 常量与格式契约 ==\n");
    CHECK(AKD_HEADER_BYTES == 64);
    CHECK(AKD_CARD_RECORD == 52);
    CHECK(strcmp(AKD_MAGIC, "AKD1") == 0 && AKD_VERSION == 1u);
    CHECK(AKD_OFF_MAGIC == 0 && AKD_OFF_VERSION == 4 && AKD_OFF_DECK_ID == 8);
    /* 关键事实：day @16、last_rtc_day @24，deck_id 才是 @8。
       / Key fact: day is @16, last_rtc_day @24; deck_id is @8. */
    CHECK(AKD_OFF_DAY == 16 && AKD_OFF_LAST_RTC_DAY == 24);
    CHECK(AKD_OFF_CARD_COUNT == 32 && AKD_OFF_TABLE_AT == 52);
    CHECK(AKD_CARD_OFF_ID == 0 && AKD_CARD_OFF_NID == 8 && AKD_CARD_OFF_ORD == 16);
    CHECK(AKD_CARD_OFF_TYPE == 20 && AKD_CARD_OFF_QUEUE == 24 && AKD_CARD_OFF_DUE == 28);
    CHECK(AKD_CARD_OFF_IVL == 32 && AKD_CARD_OFF_FACTOR == 36);
    CHECK(AKD_CARD_OFF_REPS == 40 && AKD_CARD_OFF_LAPSES == 44);
    CHECK(AKD_CARD_OFF_RESERVED == 48);
    CHECK(ANKI_STATE_VERSION == 1u && ANKI_RESUME_BYTES == 512u);
    CHECK(strcmp(ANKI_STATE_MAGIC, "AKS1") == 0);
    CHECK(strcmp(ANKI_STATE_SUFFIX, ".aks") == 0);
    CHECK(ANKI_BACKUP_FORMAT_VERSION == 1 && strcmp(ANKI_BACKUP_SUFFIX, ".akb") == 0);
    CHECK(ANKI_BACKUP_LINE_MAX == 192 && ANKI_RESTORE_CARD_MAX == 20000);
    CHECK(ANKI_SESSION_MAX == 50);
    CHECK(strcmp(ANKI_DECK_TMP_SUFFIX, ".akd.tmp") == 0);
    CHECK(strcmp(ANKI_DECK_BAK_SUFFIX, ".akd.bak") == 0);
    /* store 与 progress 共用的内联 FNV-1a 必须与标准向量一致。
       / The header-inline FNV-1a shared by store and progress matches vectors. */
    CHECK(anki_fnv1a("", 0, ANKI_FNV1A_BASIS) == 2166136261u);
    CHECK(anki_fnv1a("a", 1, ANKI_FNV1A_BASIS) == 0xe40c292cu);
    CHECK(anki_fnv1a("foobar", 6, ANKI_FNV1A_BASIS) == 0xbf9cf968u);
    /* 摘要尾部两个新字段默认 0（既有 ABI 不受影响）。
       / The two appended summary fields default to 0. */
    anki_deck_summary_t s;
    memset(&s, 0, sizeof(s));
    CHECK(s.resume_pos == 0 && s.resume_count == 0);
}

/* A1 往返、A2 单会话/离开路径、A6 0xAA 未初始化槽。
   / A1 roundtrip, A2 single-session & leave paths, A6 poisoned slots. */
static void test_qa_resume_roundtrip(void) {
    printf("== qa resume roundtrip / 续学往返 ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    CHECK(imported == 2);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;

    anki_deck_t deck = {0};
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    CHECK(deck.card_count == 3);
    anki_store_sync_day(&deck);
    int idx[ANKI_SESSION_MAX] = {0};
    int cnt = anki_store_session(&deck, idx, ANKI_SESSION_MAX);
    CHECK(cnt == 3);
    if (cnt != 3) {
        anki_store_unload(&deck);
        return;
    }

    char aks[320], akd[320], aks_tf[320];
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    qa_aks_path(ANKI_DIR_TF, id, aks_tf, sizeof(aks_tf));

    /* A1：保存 → 载入，还原队列（按 card_id）、pos 与三个计数。
       / A1: save then load restores the card_id queue, pos and all counters. */
    CHECK(anki_store_resume_save(&deck, idx, cnt, 1, 7, 2, 5) == ESP_OK);
    CHECK(qa_exists(aks));
    size_t sb_len = 0;
    uint8_t* sb = qa_slurp(aks, &sb_len);
    CHECK(sb != NULL && sb_len == ANKI_RESUME_BYTES);
    if (sb && sb_len == ANKI_RESUME_BYTES) {
        CHECK(memcmp(sb, ANKI_STATE_MAGIC, 4) == 0);
        CHECK(qa_u32(sb + 4) == ANKI_STATE_VERSION);
        CHECK(qa_u32(sb + 8) == ANKI_RESUME_BYTES);
        CHECK(qa_u32(sb + 12) ==
              anki_fnv1a(sb + 16, ANKI_RESUME_BYTES - 16, ANKI_FNV1A_BASIS));
        CHECK(qa_i64(sb + 16) == id);
        CHECK(qa_u32(sb + 28) == 3u);  // card_count
        CHECK(qa_u32(sb + 32) == 3u);  // session_count
        CHECK(qa_u32(sb + 36) == 1u);  // session_pos
        CHECK(qa_u32(sb + 40) == 7u && qa_u32(sb + 44) == 2u && qa_u32(sb + 48) == 5u);
        CHECK(qa_i64(sb + 52) == deck.day);
        /* A6：session_ids[50] 未用槽与 reserved[12] 必须显式为 0，
           绝不能出现 0xAA 毒化值。/ Unused slots and reserved must be zero. */
        bool tail_zero = true;
        for (size_t k = 64 + 3 * 8; k < ANKI_RESUME_BYTES; ++k) {
            if (sb[k] != 0) {
                tail_zero = false;
                break;
            }
        }
        CHECK(tail_zero);
        free(sb);
    }

    anki_resume_t r = {0};
    CHECK(anki_store_resume_load(&deck, &r) == ESP_OK);
    CHECK(r.deck_id == id && r.version == ANKI_STATE_VERSION);
    CHECK(r.card_count == 3 && r.session_count == 3 && r.session_pos == 1);
    CHECK(r.studied == 7 && r.again == 2 && r.good_or_better == 5);
    CHECK(r.day == deck.day);
    CHECK(r.root_tag == ANKI_ROOT_TAG_FLASH);
    for (int i = 0; i < cnt; ++i) CHECK(r.session_ids[i] == deck.cards[idx[i]].id);
    for (int i = cnt; i < ANKI_SESSION_MAX; ++i) CHECK(r.session_ids[i] == 0);
    CHECK(r.fingerprint != 0);
    uint32_t fp = ANKI_FNV1A_BASIS;
    for (int i = 0; i < deck.card_count; ++i) {
        int64_t cid = deck.cards[i].id;
        fp = anki_fnv1a(&cid, sizeof(cid), fp);
    }
    CHECK(r.fingerprint == fp);

    /* A1：resume_indices 把 card_id 队列映射回当前 deck 下标；未用槽清零。
       / A1: resume_indices maps the card_id queue back to deck indices. */
    int mapped[ANKI_SESSION_MAX];
    memset(mapped, 0xAA, sizeof(mapped));
    CHECK(anki_store_resume_indices(&deck, &r, mapped, ANKI_SESSION_MAX) == cnt);
    for (int i = 0; i < cnt; ++i) CHECK(mapped[i] == idx[i]);
    for (int i = cnt; i < ANKI_SESSION_MAX; ++i) CHECK(mapped[i] == 0);

    /* A2：只保留一个未结束会话 —— 再写一次即整体覆盖，文件仍是一份 512 B。
       / A2: only one open session; a second save overwrites, still one 512 B file. */
    CHECK(anki_store_resume_save(&deck, idx, cnt, 2, 9, 3, 6) == ESP_OK);
    size_t sb2_len = 0;
    uint8_t* sb2 = qa_slurp(aks, &sb2_len);
    CHECK(sb2 != NULL && sb2_len == ANKI_RESUME_BYTES);
    free(sb2);
    anki_resume_t r2 = {0};
    CHECK(anki_store_resume_load(&deck, &r2) == ESP_OK);
    CHECK(r2.session_pos == 2 && r2.studied == 9 && r2.again == 3 &&
          r2.good_or_better == 6);
    CHECK(r2.session_count == 3);
    CHECK(qa_count_suffix(ANKI_DIR, ANKI_STATE_SUFFIX) == 1);

    /* A2 / K1：unload + 重新 load（模拟 session_release 的 store 层路径）
       不得清除 .aks；重进仍能续学。/ Unload/reload must preserve .aks. */
    anki_store_unload(&deck);
    CHECK(qa_exists(aks));
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    anki_resume_t r3 = {0};
    CHECK(anki_store_resume_load(&deck, &r3) == ESP_OK);
    CHECK(r3.session_pos == 2 && r3.session_count == 3);
    CHECK(anki_store_resume_indices(&deck, &r3, mapped, ANKI_SESSION_MAX) == 3);

    /* A2 / E3：走到小结 clear —— 两根 .aks 都不存在，重进静默重建。
       / E3: reaching the summary clears both roots; next entry rebuilds. */
    anki_store_resume_clear(id);
    CHECK(!qa_exists(aks));
    CHECK(!qa_exists(aks_tf));
    anki_resume_t cleared;
    memset(&cleared, 0xAA, sizeof(cleared));
    CHECK(anki_store_resume_load(&deck, &cleared) != ESP_OK);
    bool zeroed = true;
    const uint8_t* cp = (const uint8_t*)&cleared;
    for (size_t k = 0; k < sizeof(cleared); ++k) {
        if (cp[k] != 0) {
            zeroed = false;
            break;
        }
    }
    CHECK(zeroed);
    int rebuilt[ANKI_SESSION_MAX];
    int n = anki_store_session(&deck, rebuilt, ANKI_SESSION_MAX);
    CHECK(n > 0 && n <= ANKI_SESSION_MAX);

    /* A5：无 .aks 时 peek 静默置 0，列表不阻塞、卡组不消失。
       / A5: with no .aks the list still enumerates both decks. */
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 2);

    anki_store_unload(&deck);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* A3 失效条件：跨日 / 重导入 / 删除 / root_tag / card_count /
   fingerprint / 队列 card_id 找不到 —— 一律静默降级重建、不展示错卡。
   / A3 invalidation matrix: every mismatch silently degrades to rebuild. */
static void test_qa_resume_invalidation(void) {
    printf("== qa resume invalidation / 失效条件 ==\n");
    char aks[320], akd[320], aks_tf[320];
    int64_t id = 0;
    anki_deck_t deck;
    int idx[ANKI_SESSION_MAX];
    int cnt = 0;
    anki_resume_t r;

    /* E4 跨日：推进 .akd 头 day 后重新载入，绑定日不匹配 → 失效。
       / E4: advance the header day; the resume day binding no longer matches. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    {
        size_t len = 0;
        uint8_t* b = qa_slurp(akd, &len);
        CHECK(b != NULL && len >= AKD_HEADER_BYTES);
        if (b) {
            qa_wi64(b + AKD_OFF_DAY, qa_i64(b + AKD_OFF_DAY) + 3);
            CHECK(qa_write(akd, b, len));
            free(b);
        }
    }
    anki_store_unload(&deck);
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0 && r.session_pos == 0);
    CHECK(anki_store_session(&deck, idx, ANKI_SESSION_MAX) > 0);
    anki_store_unload(&deck);

    /* E5 重导入同 id：import 内 clear，旧 .aks 不残留。
       / E5: re-import of the same id clears the stale resume state. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    CHECK(qa_exists(aks));
    {
        int imported2 = 0;
        CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported2) == ESP_OK);
        CHECK(imported2 == 2);
    }
    CHECK(!qa_exists(aks));
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) != ESP_OK);
    CHECK(r.session_count == 0);
    anki_store_unload(&deck);

    /* E6 删除：两根同 id 的 .aks 一并清除，不产生孤儿记录。
       / E6: delete clears .aks under both roots. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_path(ANKI_DIR_TF, id, aks_tf, sizeof(aks_tf));
    CHECK(copy_file(aks, aks_tf));
    CHECK(qa_exists(aks) && qa_exists(aks_tf));
    CHECK(anki_store_delete(id) == ESP_OK);
    CHECK(!qa_exists(aks) && !qa_exists(aks_tf));
    anki_store_unload(&deck);

    /* E9 双根绑定：翻转 .aks 的 root_tag（校验和同步重算）→ 失效。
       / E9: flipping the root_tag binding invalidates the state. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_set_u32(aks, 24, ANKI_ROOT_TAG_TF, true);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0);
    anki_store_unload(&deck);

    /* E8 卡数不符。/ E8: card_count mismatch. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_set_u32(aks, 28, 4u, true);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0);
    anki_store_unload(&deck);

    /* E8 内容指纹不符：直接篡改 .aks 的 fingerprint。
       / E8: fingerprint mismatch. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_set_u32(aks, 60, 0x12345678u, true);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0);
    anki_store_unload(&deck);

    /* E8 真实场景：卡 id 集合变化（改 .akd 卡片表里的一个 id）→ 失效。
       / E8 real case: changing a card id in the deck changes the fingerprint. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    {
        size_t len = 0;
        uint8_t* b = qa_slurp(akd, &len);
        CHECK(b != NULL);
        if (b) {
            uint32_t table_at = qa_u32(b + AKD_OFF_TABLE_AT);
            qa_wi64(b + table_at + AKD_CARD_OFF_ID, 888888888888LL);
            CHECK(qa_write(akd, b, len));
            free(b);
        }
    }
    anki_store_unload(&deck);
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0);
    anki_store_unload(&deck);

    /* E8 队列里 card_id 找不到：改 session_ids[0] → resume_load 失效；
       resume_indices 也必须返回 0 且不写出任何错卡下标。
       / E8: a card_id missing from the deck invalidates load and indices. */
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_set_i64(aks, 64, 777777777777LL, true);
    memset(&r, 0xAA, sizeof(r));
    CHECK(anki_store_resume_load(&deck, &r) == ESP_ERR_NOT_FOUND);
    CHECK(r.session_count == 0);
    anki_resume_t fake = {0};
    fake.deck_id = id;
    fake.card_count = deck.card_count;
    fake.session_count = 3;
    fake.session_pos = 1;
    fake.session_ids[0] = deck.cards[idx[0]].id;
    fake.session_ids[1] = 999999999999LL;
    fake.session_ids[2] = deck.cards[idx[2]].id;
    int out[ANKI_SESSION_MAX];
    memset(out, 0xAA, sizeof(out));
    CHECK(anki_store_resume_indices(&deck, &fake, out, ANKI_SESSION_MAX) == 0);
    bool all_zero = true;
    for (int i = 0; i < ANKI_SESSION_MAX; ++i) {
        if (out[i] != 0) all_zero = false;
    }
    CHECK(all_zero);
    fake.session_ids[1] = deck.cards[idx[1]].id;
    CHECK(anki_store_resume_indices(&deck, &fake, out, ANKI_SESSION_MAX) == 3);
    CHECK(out[0] == idx[0] && out[1] == idx[1] && out[2] == idx[2]);
    anki_store_unload(&deck);

    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* A4 损坏拒绝：截断 / magic / version / len / checksum / pos / count 越界，
   每项都断言「拒绝、out 清零、且 .akd 零改动」。
   / A4 corruption matrix: reject, zero the out struct, leave .akd untouched. */
static void qa_corrupt_aks(const char* path, int kind) {
    size_t len = 0;
    uint8_t* b = qa_slurp(path, &len);
    if (!b || len != ANKI_RESUME_BYTES) {
        free(b);
        return;
    }
    switch (kind) {
        case 0: {  // 文件截断 / truncated file
            FILE* f = fopen(path, "wb");
            if (f) {
                (void)fwrite(b, 1, 100, f);
                fclose(f);
            }
            break;
        }
        case 1:  // magic 篡改 / magic tampered
            b[0] = 'X';
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        case 2:  // version 不识别 / unknown version
            qa_wu32(b + 4, 2u);
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        case 3:  // len != 512
            qa_wu32(b + 8, 511u);
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        case 4:  // checksum 篡改（不重算，故意不通过）/ checksum tampered
            b[12] ^= 1u;
            (void)qa_write(path, b, len);
            break;
        case 5:  // pos 越界（pos == count）/ pos out of range
            qa_wu32(b + 36, 3u);
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        case 6:  // count 越界（> ANKI_SESSION_MAX）/ count over cap
            qa_wu32(b + 32, ANKI_SESSION_MAX + 1u);
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        case 7:  // count = 0 / zero count
            qa_wu32(b + 32, 0u);
            qa_aks_reseal(b);
            (void)qa_write(path, b, len);
            break;
        default:
            break;
    }
    free(b);
}

static void test_qa_resume_corruption(void) {
    printf("== qa resume corruption / 损坏拒绝 ==\n");
    struct {
        const char* label;
        int kind;
        esp_err_t want;
    } cases[] = {
        {"truncated", 0, ESP_ERR_INVALID_SIZE},
        {"magic", 1, ESP_ERR_INVALID_SIZE},
        {"version", 2, ESP_ERR_INVALID_VERSION},
        {"len!=512", 3, ESP_ERR_INVALID_SIZE},
        {"checksum", 4, ESP_ERR_INVALID_SIZE},
        {"pos out of range", 5, ESP_ERR_NOT_FOUND},
        {"count > 50", 6, ESP_ERR_NOT_FOUND},
        {"count = 0", 7, ESP_ERR_NOT_FOUND},
    };
    char aks[320], akd[320];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int64_t id = 0;
        anki_deck_t deck;
        int idx[ANKI_SESSION_MAX];
        int cnt = 0;
        CHECK(qa_prepare(&id, &deck, idx, &cnt));
        qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
        qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
        size_t snap_len = 0;
        uint8_t* snap = qa_slurp(akd, &snap_len);
        CHECK(snap != NULL && snap_len > 0);
        qa_corrupt_aks(aks, cases[i].kind);
        anki_resume_t r;
        memset(&r, 0xAA, sizeof(r));
        esp_err_t got = anki_store_resume_load(&deck, &r);
        if (got != cases[i].want) {
            printf("  corruption[%s]: got 0x%x, want 0x%x\n", cases[i].label,
                   (unsigned)got, (unsigned)cases[i].want);
        }
        CHECK(got == cases[i].want);
        CHECK(r.deck_id == 0 && r.session_count == 0 && r.session_pos == 0);
        /* 关键：损坏拒绝不得改动 .akd 一个字节。/ .akd stays byte-identical. */
        size_t now_len = 0;
        uint8_t* now = qa_slurp(akd, &now_len);
        CHECK(now != NULL && now_len == snap_len);
        if (now && snap && now_len == snap_len) CHECK(memcmp(now, snap, snap_len) == 0);
        free(snap);
        free(now);
        /* 静默降级重建，不展示错卡。/ Silent rebuild, never a wrong card. */
        int rebuilt[ANKI_SESSION_MAX];
        CHECK(anki_store_session(&deck, rebuilt, ANKI_SESSION_MAX) == cnt);
        anki_store_unload(&deck);
    }
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* A5 resume_peek：有效状态填摘要；任何读失败 / 绑定不符都静默置 0，
   绝不阻塞 anki_store_list。/ A5: peek failures silently yield 0. */
typedef struct {
    int64_t id;
    int pos;
    int count;
    bool found;
    int decks;
} qa_peek_t;

static bool qa_visit_peek(const anki_deck_summary_t* s, void* ctx) {
    qa_peek_t* p = (qa_peek_t*)ctx;
    ++p->decks;
    if (s->deck_id == p->id) {
        p->pos = s->resume_pos;
        p->count = s->resume_count;
        p->found = true;
    }
    return true;
}

static qa_peek_t qa_peek_list(int64_t id) {
    qa_peek_t p;
    memset(&p, 0, sizeof(p));
    p.id = id;
    CHECK(anki_store_list(qa_visit_peek, &p) == ESP_OK);
    return p;
}

static void test_qa_resume_peek(void) {
    printf("== qa resume peek / 列表轻量探测 ==\n");
    int64_t id = 0;
    anki_deck_t deck;
    int idx[ANKI_SESSION_MAX];
    int cnt = 0;
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    char aks[320], akd[320];
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));

    /* 有效 .aks → 列表摘要带出续学位置。/ A valid .aks fills the summary. */
    qa_peek_t ok = qa_peek_list(id);
    CHECK(ok.decks == 2 && ok.found);
    CHECK(ok.pos == 1 && ok.count == 3);

    /* 直接 API：有效值、NULL 参数、不存在文件都安全。
       / Direct API: valid, NULL args and a missing file are all safe. */
    int p = -1, c = -1;
    anki_store_resume_peek(id, ANKI_DIR, &p, &c);
    CHECK(p == 1 && c == 3);
    anki_store_resume_peek(id, ANKI_DIR, NULL, &c);
    anki_store_resume_peek(id, ANKI_DIR, &p, NULL);
    anki_store_resume_peek(id, ANKI_DIR, NULL, NULL);
    anki_store_resume_peek(id, NULL, &p, &c);
    CHECK(p == 0 && c == 0);
    p = -1;
    c = -1;
    anki_store_resume_peek(123456789123LL, ANKI_DIR, &p, &c);
    CHECK(p == 0 && c == 0);

    /* 损坏 .aks（截断）→ 静默置 0，列表仍两个卡组、不弹错。
       / A truncated .aks yields 0 and never blocks the list. */
    {
        size_t len = 0;
        uint8_t* b = qa_slurp(aks, &len);
        CHECK(b != NULL);
        if (b) {
            CHECK(qa_write(aks, b, 40));
            free(b);
        }
    }
    qa_peek_t bad = qa_peek_list(id);
    CHECK(bad.decks == 2 && bad.found);
    CHECK(bad.pos == 0 && bad.count == 0);

    /* 绑定不符（card_count / 跨日）→ peek 静默置 0。
       / Binding mismatches (card_count / day) also yield 0. */
    /* qa_prepare 会重新载入整库：先释放上一次的 PSRAM 数组，否则 ASan 报泄漏。
       / qa_prepare reloads the whole deck; release the previous arrays first. */
    anki_store_unload(&deck);
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    qa_aks_set_u32(aks, 28, 4u, true);
    qa_peek_t bad2 = qa_peek_list(id);
    CHECK(bad2.decks == 2 && bad2.pos == 0 && bad2.count == 0);

    anki_store_unload(&deck);
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    {
        size_t len = 0;
        uint8_t* b = qa_slurp(akd, &len);
        CHECK(b != NULL);
        if (b) {
            qa_wi64(b + AKD_OFF_DAY, qa_i64(b + AKD_OFF_DAY) + 5);
            CHECK(qa_write(akd, b, len));
            free(b);
        }
    }
    qa_peek_t bad3 = qa_peek_list(id);
    CHECK(bad3.decks == 2 && bad3.pos == 0 && bad3.count == 0);

    anki_store_unload(&deck);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

// ---------------------------------------------------------------- QA B. 进度导出 / 恢复 .akb

/* 行式 .akb 构造器：SUM 只覆盖 C 行内容字节（不含换行），与导出端和解析端
   使用同一规则；未知键与未知行前缀由被测解析器忽略。
   / Line-oriented .akb builder; SUM covers C-line bytes without the newline,
   matching both the exporter and the parser. */
typedef struct {
    char* b;
    size_t cap;
    size_t at;
    uint32_t sum;
} qa_akb_t;

static void qa_akb_init(qa_akb_t* a, size_t cap) {
    a->b = malloc(cap);
    a->cap = cap;
    a->at = 0;
    a->sum = ANKI_FNV1A_BASIS;
    if (a->b) a->b[0] = 0;
}

static void qa_akb_free(qa_akb_t* a) {
    free(a->b);
    a->b = NULL;
    a->cap = 0;
    a->at = 0;
}

static void qa_akb_put(qa_akb_t* a, const char* fmt, ...) {
    if (!a->b || a->at + 1 >= a->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(a->b + a->at, a->cap - a->at, fmt, ap);
    va_end(ap);
    if (n > 0) a->at += (size_t)n;
    if (a->at >= a->cap) a->at = a->cap - 1;
    a->b[a->at] = 0;
}

static void qa_akb_c(qa_akb_t* a, const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(line)) return;
    a->sum = anki_fnv1a(line, (size_t)n, a->sum);
    qa_akb_put(a, "%s\n", line);
}

/* omit 位掩码用于构造「缺必需键」的坏备份。
   / omit bitmask builds backups with a missing required key. */
static void qa_akb_head_ex(qa_akb_t* a, int version, int64_t deck_id, int64_t day,
                           int64_t rtc, int count, int omit) {
    qa_akb_put(a, "# qa progress backup (UTF-8)\n");
    if (!(omit & 1)) qa_akb_put(a, "akb_version=%d\n", version);
    if (!(omit & 2)) qa_akb_put(a, "deck_id=%lld\n", (long long)deck_id);
    qa_akb_put(a, "deck_name=qa\nsource=qa.apkg\n");
    if (!(omit & 4)) qa_akb_put(a, "day=%lld\n", (long long)day);
    if (!(omit & 8)) qa_akb_put(a, "last_rtc_day=%lld\n", (long long)rtc);
    if (!(omit & 16)) qa_akb_put(a, "card_count=%d\n", count);
    qa_akb_put(a, "ext1=0\next2=0\n");
}

static void qa_akb_head(qa_akb_t* a, int version, int64_t deck_id, int64_t day,
                        int64_t rtc, int count) {
    qa_akb_head_ex(a, version, deck_id, day, rtc, count, 0);
}

static void qa_akb_end_ex(qa_akb_t* a, bool with_end, bool with_sum) {
    if (with_end) qa_akb_put(a, "END\n");
    if (with_sum) qa_akb_put(a, "SUM,%08x\n", (unsigned)a->sum);
}

static void qa_akb_end(qa_akb_t* a, bool with_end) {
    qa_akb_end_ex(a, with_end, true);
}

/* 拒绝用例统一断言：期望错误码、结果结构体清零、.akd 字节原样、
   且不留 .akd.tmp / .akd.bak。/ One helper for the whole reject matrix. */
static void qa_reject(const char* label, const char* text, size_t len, esp_err_t want,
                      const char* path, const char* akd_path, const uint8_t* snap,
                      size_t snap_len, const char* tmp_path, const char* bak_path) {
    CHECK(qa_write(path, text, len));
    anki_progress_result_t r;
    memset(&r, 0xAA, sizeof(r));
    esp_err_t got = anki_progress_restore(path, &r);
    if (got != want) {
        printf("  reject[%s]: got 0x%x, want 0x%x\n", label, (unsigned)got,
               (unsigned)want);
    }
    CHECK(got == want);
    CHECK(r.deck_id == 0 && r.covered == 0 && r.skipped == 0 && r.name[0] == 0);
    size_t now_len = 0;
    uint8_t* now = qa_slurp(akd_path, &now_len);
    CHECK(now != NULL && now_len == snap_len);
    if (now && snap && now_len == snap_len) CHECK(memcmp(now, snap, snap_len) == 0);
    free(now);
    CHECK(!qa_exists(tmp_path) && !qa_exists(bak_path));
}

/* B7 往返 / B8 R-1 日基准 / I1 字节不变 / B12 文件名与单份覆盖。
   / B7 roundtrip, B8 R-1 day base, I1 byte invariance, B12 naming. */
static void test_qa_progress_roundtrip(void) {
    printf("== qa progress roundtrip (R-1 / I1) ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;
    anki_deck_t deck = {0};
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    anki_store_sync_day(&deck);
    /* 逐卡评分，造出非零且有区分度的 7 个调度字段。
       / Grade every card so all seven scheduling fields carry distinct values. */
    for (int i = 0; i < deck.card_count; ++i) {
        anki_schedule(&deck, i, (anki_grade_t)(i % 4));
        CHECK(anki_store_save_card(&deck, i) == ESP_OK);
    }
    char akd[320], akb[320], tmp[320], bak[320];
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    qa_akb_path(ANKI_DIR, id, akb, sizeof(akb));
    qa_tmp_path(ANKI_DIR, id, tmp, sizeof(tmp));
    qa_bak_path(ANKI_DIR, id, bak, sizeof(bak));

    size_t orig_len = 0;
    uint8_t* orig = qa_slurp(akd, &orig_len);
    CHECK(orig != NULL && orig_len >= AKD_HEADER_BYTES);
    if (!orig) {
        anki_store_unload(&deck);
        return;
    }
    uint32_t table_at = qa_u32(orig + AKD_OFF_TABLE_AT);
    uint32_t card_count = qa_u32(orig + AKD_OFF_CARD_COUNT);
    int64_t backup_day = qa_i64(orig + AKD_OFF_DAY);
    int64_t backup_rtc = qa_i64(orig + AKD_OFF_LAST_RTC_DAY);
    CHECK(card_count == 3 && table_at >= AKD_HEADER_BYTES);
    uint32_t orig7[3][7];
    int64_t orig_ids[3];
    for (uint32_t k = 0; k < card_count; ++k) {
        const uint8_t* rec = orig + table_at + (size_t)k * AKD_CARD_RECORD;
        orig_ids[k] = qa_i64(rec + AKD_CARD_OFF_ID);
        for (int j = 0; j < 7; ++j) orig7[k][j] = qa_u32(rec + 20 + (size_t)j * 4);
    }

    /* B7 导出：文件名、无 TF 卡分支（宿主活动根即非 TF 根）。
       / B7: export path and the no-TF-card branch (host active root is flash). */
    char out[320] = {0};
    CHECK(anki_progress_export(id, out, sizeof(out)) == ESP_OK);
    CHECK(strcmp(out, akb) == 0);
    CHECK(strcmp(anki_store_active_root(), ANKI_DIR) == 0);
    CHECK(strcmp(anki_store_active_root(), ANKI_DIR_TF) != 0);
    char expect_name[64];
    snprintf(expect_name, sizeof(expect_name), "%lld.akb", (long long)id);
    const char* base = strrchr(out, '/');
    CHECK(base != NULL && strcmp(base + 1, expect_name) == 0);  // 不带时间戳
    CHECK(qa_exists(out));

    /* B7 文本结构与 SUM；不含卡片正文。
       / B7: text structure, correct SUM, and no card content. */
    size_t bk_len = 0;
    uint8_t* bk = qa_slurp(out, &bk_len);
    CHECK(bk != NULL && bk_len > 0 && bk_len <= ANKI_BACKUP_FILE_MAX);
    uint32_t sum = ANKI_FNV1A_BASIS;
    int c_rows = 0, end_rows = 0, sum_rows = 0;
    bool has_version = false, has_day = false, has_rtc = false, has_count = false;
    uint32_t sum_decl = 0;
    {
        FILE* fp = fopen(out, "r");
        char line[512];
        while (fp && fgets(line, sizeof(line), fp)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (strncmp(line, "C,", 2) == 0) {
                ++c_rows;
                sum = anki_fnv1a(line, n, sum);
            } else if (strcmp(line, "END") == 0) ++end_rows;
            else if (strncmp(line, "SUM,", 4) == 0) {
                ++sum_rows;
                sum_decl = (uint32_t)strtoul(line + 4, NULL, 16);
            } else if (strncmp(line, "akb_version=", 12) == 0) has_version = true;
            else if (strncmp(line, "day=", 4) == 0) has_day = true;
            else if (strncmp(line, "last_rtc_day=", 13) == 0) has_rtc = true;
            else if (strncmp(line, "card_count=", 11) == 0) has_count = true;
        }
        if (fp) fclose(fp);
    }
    CHECK(has_version && has_day && has_rtc && has_count);
    CHECK(c_rows == (int)card_count);
    CHECK(end_rows == 1 && sum_rows == 1);
    CHECK(sum_decl == sum);
    if (bk) {
        char* text = malloc(bk_len + 1);
        CHECK(text != NULL);
        if (text) {
            memcpy(text, bk, bk_len);
            text[bk_len] = 0;
            CHECK(strstr(text, "apple") == NULL);
            CHECK(strstr(text, "苹果") == NULL);
            CHECK(strstr(text, "<div") == NULL);
            CHECK(strstr(text, "FrontSide") == NULL);
            CHECK(strstr(text, "正面") == NULL);
            CHECK(strstr(text, "collection.anki") == NULL);
            free(text);
        }
        free(bk);
    }
    /* 单份覆盖：再导出一次仍只有一份 .akb，路径不变。
       / Single overwrite: a second export keeps exactly one .akb. */
    char out2[320] = {0};
    CHECK(anki_progress_export(id, out2, sizeof(out2)) == ESP_OK);
    CHECK(strcmp(out, out2) == 0);
    CHECK(qa_count_suffix(ANKI_DIR, ANKI_BACKUP_SUFFIX) == 1);

    /* R-1：模拟「换机后新设备 day=0」并破坏卡片调度字段。
       / R-1: simulate a fresh device (day=0) and corrupt the card fields. */
    size_t dev_len = 0;
    uint8_t* dev = qa_slurp(akd, &dev_len);
    CHECK(dev != NULL && dev_len == orig_len);
    if (dev) {
        qa_wi64(dev + AKD_OFF_DAY, 0);
        qa_wi64(dev + AKD_OFF_LAST_RTC_DAY, 0);
        for (uint32_t k = 0; k < card_count; ++k) {
            uint8_t* rec = dev + table_at + (size_t)k * AKD_CARD_RECORD;
            for (int j = 0; j < 7; ++j) {
                qa_wu32(rec + 20 + (size_t)j * 4, 0xDEAD0000u + (uint32_t)j);
            }
        }
        CHECK(qa_write(akd, dev, dev_len));
        free(dev);
    }
    anki_store_unload(&deck);
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    CHECK(deck.day == 0 && deck.last_rtc_day == 0);
    /* 失败模式复现：日基准丢失时所有复习卡都不到期。
       / Reproduce the failure: without the day base no review card is due. */
    bool none_due = true;
    for (int i = 0; i < deck.card_count; ++i) {
        const anki_card_t* c = &deck.cards[i];
        if (c->queue >= 1 && c->queue <= 3 && c->due <= deck.day) none_due = false;
    }
    CHECK(none_due);
    anki_store_unload(&deck);

    /* 恢复 / Restore. */
    anki_progress_result_t res = {0};
    CHECK(anki_progress_restore(out, &res) == ESP_OK);
    CHECK(res.deck_id == id);
    CHECK(res.covered == (int)card_count && res.skipped == 0);
    CHECK(strlen(res.name) > 0);
    CHECK(!qa_exists(tmp) && !qa_exists(bak));  // 提交后无中间态残留

    size_t after_len = 0;
    uint8_t* after = qa_slurp(akd, &after_len);
    CHECK(after != NULL && after_len == orig_len);
    if (after && after_len == orig_len) {
        /* R-1 硬断言：day @16 / last_rtc_day @24 实际写回备份值。
           / R-1 hard assertion: the header day base is actually written back. */
        CHECK(qa_i64(after + AKD_OFF_DAY) == backup_day);
        CHECK(qa_i64(after + AKD_OFF_LAST_RTC_DAY) == backup_rtc);
        /* B7：每卡 7 个调度字段逐字段一致，card_id 不变。
           / B7: all seven fields match, card ids untouched. */
        for (uint32_t k = 0; k < card_count; ++k) {
            const uint8_t* rec = after + table_at + (size_t)k * AKD_CARD_RECORD;
            CHECK(qa_i64(rec + AKD_CARD_OFF_ID) == orig_ids[k]);
            for (int j = 0; j < 7; ++j) {
                CHECK(qa_u32(rec + 20 + (size_t)j * 4) == orig7[k][j]);
            }
        }
        /* I1：排除允许区间后整文件逐字节不变（此处也恰好全等）。
           / I1: byte-identical outside the allowed ranges. */
        CHECK(qa_i1_diff(orig, after, orig_len, table_at, card_count) == 0);
        CHECK(memcmp(orig, after, orig_len) == 0);
        free(after);
    }

    /* R-1 端到端：重载后日基准已写回，到期复习卡重新进入会话。
       / R-1 E2E: after reload the due review cards are selectable again. */
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    CHECK(deck.day == backup_day && deck.last_rtc_day == backup_rtc);
    int due_by_base = 0;
    for (int i = 0; i < deck.card_count; ++i) {
        const anki_card_t* c = &deck.cards[i];
        if (c->queue >= 1 && c->queue <= 3 && c->due <= deck.day) ++due_by_base;
    }
    CHECK(due_by_base > 0);
    int sess[ANKI_SESSION_MAX];
    int sn = anki_store_session(&deck, sess, ANKI_SESSION_MAX);
    CHECK(sn > 0);
    bool review_in_session = false;
    for (int i = 0; i < sn; ++i) {
        const anki_card_t* c = &deck.cards[sess[i]];
        if (c->queue >= 1 && c->queue <= 3 && c->due <= deck.day) review_in_session = true;
    }
    CHECK(review_in_session);
    anki_store_unload(&deck);

    /* B8-I1 严格变体：备份 7 字段与设备当前值不同时，允许区间外仍逐字节不变。
       / Strict I1: with different backup values only the allowed ranges change. */
    {
        qa_akb_t a;
        qa_akb_init(&a, 4096);
        CHECK(a.b != NULL);
        if (a.b) {
            qa_akb_head(&a, ANKI_BACKUP_FORMAT_VERSION, id, backup_day, backup_rtc,
                        (int)card_count);
            qa_akb_c(&a, "C,%lld,2,3,12345,7,2600,9,2", (long long)orig_ids[0]);
            for (uint32_t k = 1; k < card_count; ++k) {
                qa_akb_c(&a, "C,%lld,%u,%u,%u,%u,%u,%u,%u", (long long)orig_ids[k],
                         (unsigned)orig7[k][0], (unsigned)orig7[k][1], (unsigned)orig7[k][2],
                         (unsigned)orig7[k][3], (unsigned)orig7[k][4], (unsigned)orig7[k][5],
                         (unsigned)orig7[k][6]);
            }
            qa_akb_end(&a, true);
            char custom[320];
            snprintf(custom, sizeof(custom), "%.200s/qa-i1.akb", ANKI_DIR);
            CHECK(qa_write(custom, a.b, a.at));
            qa_akb_free(&a);
            anki_progress_result_t r2 = {0};
            CHECK(anki_progress_restore(custom, &r2) == ESP_OK);
            CHECK(r2.covered == (int)card_count && r2.skipped == 0);
            size_t v_len = 0;
            uint8_t* v = qa_slurp(akd, &v_len);
            CHECK(v != NULL && v_len == orig_len);
            if (v && v_len == orig_len) {
                const uint8_t* rec = v + table_at;
                CHECK(qa_u32(rec + 20) == 2u && qa_u32(rec + 24) == 3u);
                CHECK(qa_u32(rec + 28) == 12345u && qa_u32(rec + 32) == 7u);
                CHECK(qa_u32(rec + 36) == 2600u && qa_u32(rec + 40) == 9u);
                CHECK(qa_u32(rec + 44) == 2u);
                CHECK(qa_i64(v + AKD_OFF_DAY) == backup_day);
                /* 允许区间内确实变了，排除区间外零差异 → helper 不是恒真。
                   / Allowed ranges changed; outside them zero diff. */
                CHECK(memcmp(v, orig, orig_len) != 0);
                CHECK(qa_i1_diff(orig, v, orig_len, table_at, card_count) == 0);
                /* 反向自检：正文段改一个字节必须被 I1 检出。
                   / Sanity check: a byte outside the card table is detected. */
                v[AKD_HEADER_BYTES] ^= 0x20u;
                CHECK(qa_i1_diff(orig, v, orig_len, table_at, card_count) > 0);
                free(v);
            }
        } else {
            qa_akb_free(&a);
        }
    }

    free(orig);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* B9 拒绝矩阵：每类都断言设备进度原样不变（.akd 字节快照比对）。
   / B9 reject matrix: every case leaves the on-device .akd byte-identical. */
static void test_qa_progress_rejects(void) {
    printf("== qa progress reject matrix / 拒绝矩阵 ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;
    char akd[320], custom[320], tmp[320], bak[320];
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    qa_tmp_path(ANKI_DIR, id, tmp, sizeof(tmp));
    qa_bak_path(ANKI_DIR, id, bak, sizeof(bak));
    snprintf(custom, sizeof(custom), "%.200s/qa-reject.akb", ANKI_DIR);
    size_t orig_len = 0;
    uint8_t* orig = qa_slurp(akd, &orig_len);
    CHECK(orig != NULL && orig_len > 0);
    if (!orig) return;
    int64_t day = qa_i64(orig + AKD_OFF_DAY);
    int64_t rtc = qa_i64(orig + AKD_OFF_LAST_RTC_DAY);
    uint32_t table_at = qa_u32(orig + AKD_OFF_TABLE_AT);
    int64_t ids[3];
    for (int k = 0; k < 3; ++k) {
        ids[k] = qa_i64(orig + table_at + (size_t)k * AKD_CARD_RECORD);
    }

    struct {
        const char* label;
        int kind;
        esp_err_t want;
    } cases[] = {
        {"unknown deck", 0, ESP_ERR_NOT_FOUND},
        {"older version", 1, ESP_ERR_INVALID_VERSION},
        {"newer version", 2, ESP_ERR_NOT_SUPPORTED},
        {"missing END", 3, ESP_ERR_INVALID_SIZE},
        {"SUM tampered", 4, ESP_ERR_INVALID_SIZE},
        {"C rows != card_count", 5, ESP_ERR_INVALID_SIZE},
        {"day out of range", 6, ESP_ERR_INVALID_SIZE},
        {"card cap in header", 7, ESP_ERR_NOT_SUPPORTED},
        {"C id <= 0", 8, ESP_ERR_INVALID_SIZE},
        {"C short row", 9, ESP_ERR_INVALID_SIZE},
        {"C negative field", 10, ESP_ERR_INVALID_SIZE},
        {"missing akb_version", 11, ESP_ERR_INVALID_SIZE},
        {"missing deck_id", 12, ESP_ERR_INVALID_SIZE},
        {"missing SUM", 13, ESP_ERR_INVALID_SIZE},
        {"half file", 14, ESP_ERR_INVALID_SIZE},
        {"overlong line", 15, ESP_ERR_INVALID_SIZE},
        {"Q rows > 50", 16, ESP_ERR_INVALID_SIZE},
        {"R count > 50", 17, ESP_ERR_INVALID_SIZE},
        {"C rows > 20000", 18, ESP_ERR_NOT_SUPPORTED},
        {"empty file", 19, ESP_ERR_INVALID_SIZE},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t cap = cases[i].kind == 18 ? (2U * 1024U * 1024U + 4096U) : 8192U;
        qa_akb_t a;
        qa_akb_init(&a, cap);
        CHECK(a.b != NULL);
        if (!a.b) {
            qa_akb_free(&a);
            break;
        }
        int version = 1, count = 3, omit = 0;
        int64_t did = id, dday = day, drtc = rtc;
        bool with_end = true;
        switch (cases[i].kind) {
            case 0: did = id + 12345; break;  // 设备上不存在的卡组
            case 1: version = 0; break;
            case 2: version = 2; break;
            case 3: with_end = false; break;
            case 6: dday = 999999999999LL; break;
            case 7: count = ANKI_RESTORE_CARD_MAX + 1; break;
            case 11: omit = 1; break;
            case 12: omit = 2; break;
            case 15: {
                char big[320];
                memset(big, 'x', sizeof(big) - 1);
                big[sizeof(big) - 1] = 0;
                qa_akb_put(&a, "# %s\n", big);  // > ANKI_BACKUP_LINE_MAX
                break;
            }
            default: break;
        }
        qa_akb_head_ex(&a, version, did, dday, drtc, count, omit);

        int n_rows = 3;
        if (cases[i].kind == 5) n_rows = 2;                       // 声明 3 只写 2
        if (cases[i].kind == 18) n_rows = ANKI_RESTORE_CARD_MAX + 1;
        if (cases[i].kind == 19) n_rows = 0;
        for (int k = 0; k < n_rows; ++k) {
            int64_t cid = k < 3 ? ids[k] : 900000000000LL + (int64_t)k;
            if (cases[i].kind == 8 && k == 0) {
                qa_akb_c(&a, "C,0,0,0,0,0,0,0,0");
            } else if (cases[i].kind == 9 && k == 0) {
                qa_akb_c(&a, "C,%lld,2,2,19748,25,2500,4", (long long)cid);  // 少一字段
            } else if (cases[i].kind == 10 && k == 0) {
                qa_akb_c(&a, "C,%lld,2,2,-1,25,2500,4,1", (long long)cid);   // 负字段
            } else {
                qa_akb_c(&a, "C,%lld,2,2,19748,25,2500,4,1", (long long)cid);
            }
        }
        if (cases[i].kind == 16) {
            for (int q = 0; q <= ANKI_SESSION_MAX; ++q) {
                qa_akb_put(&a, "Q,%lld\n", (long long)(800000000000LL + q));
            }
        }
        if (cases[i].kind == 17) {
            qa_akb_put(&a, "R,51,0,0,0,0,%lld,0,0\n", (long long)day);
        }
        if (cases[i].kind == 4) {
            qa_akb_put(&a, "END\nSUM,deadbeef\n");
        } else if (cases[i].kind == 13) {
            qa_akb_put(&a, "END\n");
        } else {
            qa_akb_end_ex(&a, with_end, true);
        }
        size_t use_len = a.at;
        if (cases[i].kind == 14) use_len = 60 < a.at ? 60 : a.at;  // 半截文件
        if (cases[i].kind == 19) use_len = 0;                      // 空文件
        qa_reject(cases[i].label, a.b, use_len, cases[i].want, custom, akd, orig,
                  orig_len, tmp, bak);
        qa_akb_free(&a);
    }

    /* 参数与路径边界；仍不得改动设备进度。
       / Argument and path boundaries; the device stays untouched. */
    anki_progress_result_t r0;
    memset(&r0, 0xAA, sizeof(r0));
    CHECK(anki_progress_restore("build/anki-tests/definitely-missing.akb", &r0) ==
          ESP_ERR_NOT_FOUND);
    CHECK(anki_progress_restore(NULL, &r0) == ESP_ERR_INVALID_ARG);
    CHECK(anki_progress_restore(custom, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(anki_progress_list_backups(NULL, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(anki_progress_export(0, NULL, 0) == ESP_ERR_INVALID_ARG);
    CHECK(anki_progress_export(-1, NULL, 0) == ESP_ERR_INVALID_ARG);
    {
        size_t now_len = 0;
        uint8_t* now = qa_slurp(akd, &now_len);
        CHECK(now != NULL && now_len == orig_len && memcmp(now, orig, orig_len) == 0);
        free(now);
    }

    free(orig);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* B10 原子性与自愈：模拟掉电中间态，recover 后 .akd 必须是完整旧或
   完整新，绝不混合；recover 幂等；tmp 写失败时原 .akd 未动。
   / B10: power-cut intermediate states recover to a whole old or whole new
   .akd, never a mix; recover is idempotent; a tmp write failure leaves .akd. */
static void test_qa_progress_atomic(void) {
    printf("== qa progress atomic + recover / 原子性与自愈 ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;
    char akd[320], tmp[320], bak[320], out[320];
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    qa_tmp_path(ANKI_DIR, id, tmp, sizeof(tmp));
    qa_bak_path(ANKI_DIR, id, bak, sizeof(bak));
    CHECK(anki_progress_export(id, out, sizeof(out)) == ESP_OK);

    size_t len = 0;
    uint8_t* old = qa_slurp(akd, &len);
    CHECK(old != NULL && len > 0);
    if (!old) return;
    /* 「新」是旧的一份完整副本，只改头 day 与首卡一个调度字段，
       仍是 header_ok 可接受的完整 .akd。/ "new" is a whole valid .akd. */
    uint8_t* fresh = malloc(len);
    CHECK(fresh != NULL);
    if (!fresh) {
        free(old);
        return;
    }
    memcpy(fresh, old, len);
    qa_wi64(fresh + AKD_OFF_DAY, qa_i64(fresh + AKD_OFF_DAY) + 7);
    uint32_t table_at = qa_u32(fresh + AKD_OFF_TABLE_AT);
    qa_wu32(fresh + table_at + AKD_CARD_OFF_DUE, 424242u);

    struct {
        const char* label;
        bool has_akd;
        bool has_bak;
        bool has_tmp;
        bool tmp_valid;
        bool akd_is_new;
        bool expect_new;
    } scenes[] = {
        {"after step 4 (.akd+tmp)", true, false, true, true, false, false},
        {"after step 5 (bak+valid tmp)", false, true, true, true, false, true},
        {"after step 5 (bak+corrupt tmp)", false, true, true, false, false, false},
        {"after step 6 (new akd + old bak)", true, true, false, false, true, true},
        {"only bak (no akd/tmp)", false, true, false, false, false, false},
    };
    for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
        qa_clean_all(ANKI_DIR);
        if (scenes[i].has_akd) {
            CHECK(qa_write(akd, scenes[i].akd_is_new ? fresh : old, len));
        }
        if (scenes[i].has_bak) CHECK(qa_write(bak, old, len));
        if (scenes[i].has_tmp) {
            if (scenes[i].tmp_valid) {
                CHECK(qa_write(tmp, fresh, len));
            } else {
                const char* junk = "not a complete akd";
                CHECK(qa_write(tmp, junk, strlen(junk)));
            }
        }
        anki_progress_recover();
        size_t r1_len = 0;
        uint8_t* r1 = qa_slurp(akd, &r1_len);
        CHECK(r1 != NULL && r1_len == len);
        if (r1 && r1_len == len) {
            const uint8_t* want = scenes[i].expect_new ? fresh : old;
            CHECK(memcmp(r1, want, len) == 0);
            /* 绝无混合态：与「完整旧」或「完整新」之一逐字节全等。
               / Never a mix: byte-identical to one whole version. */
            bool whole = memcmp(r1, old, len) == 0 || memcmp(r1, fresh, len) == 0;
            CHECK(whole);
        }
        /* 幂等：再跑一次 recover，字节不变、中间态清空。
           / Idempotent: a second recover changes nothing. */
        anki_progress_recover();
        size_t r2_len = 0;
        uint8_t* r2 = qa_slurp(akd, &r2_len);
        CHECK(r2 != NULL && r2_len == len);
        if (r1 && r2 && r1_len == len && r2_len == len) CHECK(memcmp(r1, r2, len) == 0);
        CHECK(!qa_exists(tmp) && !qa_exists(bak));
        free(r1);
        free(r2);
    }

    /* 孤儿 .aks 与 .akb.tmp 清理；有对应卡组的 .aks 必须保留。
       / Orphan .aks and stale .akb.tmp are removed; a live .aks is kept. */
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    CHECK(qa_write(akd, old, len));
    char orphan_aks[320], valid_aks[320], stale_bk[320];
    qa_aks_path(ANKI_DIR, 555555555555LL, orphan_aks, sizeof(orphan_aks));
    qa_aks_path(ANKI_DIR, id, valid_aks, sizeof(valid_aks));
    snprintf(stale_bk, sizeof(stale_bk), "%.200s/stale.akb.tmp", ANKI_DIR);
    {
        uint8_t dummy[ANKI_RESUME_BYTES];
        memset(dummy, 0x5A, sizeof(dummy));
        memcpy(dummy, ANKI_STATE_MAGIC, 4);
        CHECK(qa_write(orphan_aks, dummy, sizeof(dummy)));
        CHECK(qa_write(valid_aks, dummy, sizeof(dummy)));
    }
    CHECK(qa_write(stale_bk, "half", 4));
    anki_progress_recover();
    CHECK(!qa_exists(orphan_aks));
    CHECK(qa_exists(valid_aks));
    CHECK(!qa_exists(stale_bk));
    /* 中间态后缀不被误枚举为卡组。/ Intermediates are never listed as decks. */
    CHECK(qa_write(tmp, old, len));
    CHECK(qa_write(bak, old, len));
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 1);
    anki_progress_recover();
    CHECK(!qa_exists(tmp) && !qa_exists(bak));

    /* tmp 写失败注入：造一个与 .akd.tmp 同名的目录，使 fopen("wb") 必败。
       原 .akd 必须一个字节不动，且不留 .akd.bak。
       / Inject a tmp write failure; the original .akd must stay untouched. */
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    CHECK(qa_write(akd, old, len));
    CHECK(anki_progress_export(id, out, sizeof(out)) == ESP_OK);
    size_t snap_len = 0;
    uint8_t* snap = qa_slurp(akd, &snap_len);
    CHECK(snap != NULL && snap_len == len);
    CHECK(test_mkdir(tmp) == 0 || errno == EEXIST);
    anki_progress_result_t rr;
    memset(&rr, 0xAA, sizeof(rr));
    esp_err_t we = anki_progress_restore(out, &rr);
    printf("  tmp-write-failure -> 0x%x (expect ESP_FAIL=%d)\n", (unsigned)we, ESP_FAIL);
    CHECK(we == ESP_FAIL);
    size_t after_len = 0;
    uint8_t* after = qa_slurp(akd, &after_len);
    CHECK(after != NULL && after_len == snap_len);
    if (after && snap && after_len == snap_len) CHECK(memcmp(after, snap, snap_len) == 0);
    CHECK(!qa_exists(bak));
    free(snap);
    free(after);
    (void)rmdir(tmp);

    free(old);
    free(fresh);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* B11 Q8 宽松匹配 / U4 skipped 双侧 / 拒绝整库盲覆盖。
   / B11: loose card_id matching, U4 two-sided skipped, no blind overwrite. */
static void qa_check7(const uint8_t* rec, const uint32_t want[7]) {
    for (int j = 0; j < 7; ++j) CHECK(qa_u32(rec + 20 + (size_t)j * 4) == want[j]);
}

static void test_qa_progress_loose(void) {
    printf("== qa progress loose match (Q8 / U4) ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;
    anki_deck_t deck = {0};
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    anki_store_sync_day(&deck);
    for (int i = 0; i < deck.card_count; ++i) {
        anki_schedule(&deck, i, (anki_grade_t)(i % 4));
        CHECK(anki_store_save_card(&deck, i) == ESP_OK);
    }
    anki_store_unload(&deck);

    char akd[320];
    qa_akd_path(ANKI_DIR, id, akd, sizeof(akd));
    size_t len = 0;
    uint8_t* b = qa_slurp(akd, &len);
    CHECK(b != NULL && len > 0);
    if (!b) return;
    uint32_t table_at = qa_u32(b + AKD_OFF_TABLE_AT);
    uint32_t cards = qa_u32(b + AKD_OFF_CARD_COUNT);
    int64_t day = qa_i64(b + AKD_OFF_DAY);
    int64_t rtc = qa_i64(b + AKD_OFF_LAST_RTC_DAY);
    CHECK(cards == 3);
    int64_t ids[3];
    for (uint32_t k = 0; k < cards; ++k) {
        ids[k] = qa_i64(b + table_at + (size_t)k * AKD_CARD_RECORD);
    }
    /* 把设备 7 字段统一改成哨兵值，作为「恢复前」快照：任何未被覆盖的卡
       都会保留哨兵，任何盲覆盖都会暴露。/ Sentinel makes coverage visible. */
    const uint32_t sentinel[7] = {11u, 12u, 13u, 14u, 15u, 16u, 17u};
    for (uint32_t k = 0; k < cards; ++k) {
        uint8_t* rec = b + table_at + (size_t)k * AKD_CARD_RECORD;
        for (int j = 0; j < 7; ++j) qa_wu32(rec + 20 + (size_t)j * 4, sentinel[j]);
    }
    CHECK(qa_write(akd, b, len));
    uint8_t* before = malloc(len);
    CHECK(before != NULL);
    if (!before) {
        free(b);
        return;
    }
    memcpy(before, b, len);
    free(b);

    char custom[320];
    snprintf(custom, sizeof(custom), "%.200s/qa-loose.akb", ANKI_DIR);
    const uint32_t va[7] = {2u, 2u, 19748u, 25u, 2500u, 4u, 1u};  // 行 A：ids[1]
    const uint32_t vb[7] = {1u, 1u, 19723u, 3u, 2600u, 2u, 0u};   // 行 B：ids[0]
    const uint32_t vc[7] = {0u, 0u, 0u, 0u, 0u, 0u, 0u};          // 行 C：陌生 id

    /* 用例 1：C 行错位（ids[1], ids[0]）+ 一条设备没有的 id。
       covered=2；skipped=2（设备 ids[2] 没有备份行 + 备份陌生 id 设备没有）。
       / Case 1: shuffled rows plus an unknown backup id; U4 two-sided skipped. */
    {
        CHECK(qa_write(akd, before, len));
        qa_akb_t a;
        qa_akb_init(&a, 4096);
        qa_akb_head(&a, 1, id, day, rtc, 3);
        qa_akb_c(&a, "C,%lld,%u,%u,%u,%u,%u,%u,%u", (long long)ids[1],
                 (unsigned)va[0], (unsigned)va[1], (unsigned)va[2], (unsigned)va[3],
                 (unsigned)va[4], (unsigned)va[5], (unsigned)va[6]);
        qa_akb_c(&a, "C,%lld,%u,%u,%u,%u,%u,%u,%u", (long long)ids[0],
                 (unsigned)vb[0], (unsigned)vb[1], (unsigned)vb[2], (unsigned)vb[3],
                 (unsigned)vb[4], (unsigned)vb[5], (unsigned)vb[6]);
        qa_akb_c(&a, "C,999999999999,%u,%u,%u,%u,%u,%u,%u", (unsigned)vc[0],
                 (unsigned)vc[1], (unsigned)vc[2], (unsigned)vc[3], (unsigned)vc[4],
                 (unsigned)vc[5], (unsigned)vc[6]);
        qa_akb_end(&a, true);
        CHECK(qa_write(custom, a.b, a.at));
        qa_akb_free(&a);
        anki_progress_result_t r = {0};
        CHECK(anki_progress_restore(custom, &r) == ESP_OK);
        CHECK(r.covered == 2 && r.skipped == 2);
        size_t after_len = 0;
        uint8_t* after = qa_slurp(akd, &after_len);
        CHECK(after != NULL && after_len == len);
        if (after && after_len == len) {
            /* 按 id 覆盖：卡 0 拿到的是 B（不是第 0 行的 A）→ 拒绝盲覆盖。
               / Matched by id: card 0 gets B, not the first row A. */
            qa_check7(after + table_at + 0 * AKD_CARD_RECORD, vb);
            qa_check7(after + table_at + 1 * AKD_CARD_RECORD, va);
            qa_check7(after + table_at + 2 * AKD_CARD_RECORD, sentinel);
            CHECK(qa_i1_diff(before, after, len, table_at, cards) == 0);
            free(after);
        }
    }

    /* 用例 2：备份只有两行（ids[0], ids[2]）→ covered=2、skipped=1
       （设备 ids[1] 没有对应备份行）。/ Case 2: backup misses one device id. */
    {
        CHECK(qa_write(akd, before, len));
        qa_akb_t a;
        qa_akb_init(&a, 4096);
        qa_akb_head(&a, 1, id, day, rtc, 2);
        qa_akb_c(&a, "C,%lld,%u,%u,%u,%u,%u,%u,%u", (long long)ids[0],
                 (unsigned)vb[0], (unsigned)vb[1], (unsigned)vb[2], (unsigned)vb[3],
                 (unsigned)vb[4], (unsigned)vb[5], (unsigned)vb[6]);
        qa_akb_c(&a, "C,%lld,%u,%u,%u,%u,%u,%u,%u", (long long)ids[2],
                 (unsigned)va[0], (unsigned)va[1], (unsigned)va[2], (unsigned)va[3],
                 (unsigned)va[4], (unsigned)va[5], (unsigned)va[6]);
        qa_akb_end(&a, true);
        CHECK(qa_write(custom, a.b, a.at));
        qa_akb_free(&a);
        anki_progress_result_t r = {0};
        CHECK(anki_progress_restore(custom, &r) == ESP_OK);
        CHECK(r.covered == 2 && r.skipped == 1);
        size_t after_len = 0;
        uint8_t* after = qa_slurp(akd, &after_len);
        CHECK(after != NULL && after_len == len);
        if (after && after_len == len) {
            qa_check7(after + table_at + 0 * AKD_CARD_RECORD, vb);
            qa_check7(after + table_at + 1 * AKD_CARD_RECORD, sentinel);
            qa_check7(after + table_at + 2 * AKD_CARD_RECORD, va);
            CHECK(qa_i1_diff(before, after, len, table_at, cards) == 0);
            free(after);
        }
    }

    /* 用例 3：备份 C 行全是设备没有的 id → covered=0、skipped=6，
       设备 7 字段一个都不许变（拒绝整库盲覆盖）。
       / Case 3: no id matches → nothing on the device may change. */
    {
        CHECK(qa_write(akd, before, len));
        qa_akb_t a;
        qa_akb_init(&a, 4096);
        qa_akb_head(&a, 1, id, day, rtc, 3);
        for (int k = 0; k < 3; ++k) {
            qa_akb_c(&a, "C,%lld,2,2,19748,25,2500,4,1", 700000000000LL + k);
        }
        qa_akb_end(&a, true);
        CHECK(qa_write(custom, a.b, a.at));
        qa_akb_free(&a);
        anki_progress_result_t r = {0};
        CHECK(anki_progress_restore(custom, &r) == ESP_OK);
        CHECK(r.covered == 0 && r.skipped == 6);
        size_t after_len = 0;
        uint8_t* after = qa_slurp(akd, &after_len);
        CHECK(after != NULL && after_len == len);
        if (after && after_len == len) {
            for (uint32_t k = 0; k < cards; ++k) {
                qa_check7(after + table_at + (size_t)k * AKD_CARD_RECORD, sentinel);
            }
            CHECK(memcmp(after, before, len) == 0 ||
                  qa_i1_diff(before, after, len, table_at, cards) == 0);
            free(after);
        }
    }

    free(before);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* B12 双根备份枚举：同名只列一次且活动根优先；无 TF 卡导出/恢复仍可用。
   / B12: dual-root backup enumeration, active-root-first dedup, no TF gating. */
typedef struct {
    int n;
    char paths[16][320];
    char names[16][96];
} qa_bk_list_t;

static bool qa_visit_bk(const char* path, const char* filename, void* ctx) {
    qa_bk_list_t* l = (qa_bk_list_t*)ctx;
    if (l->n < 16) {
        snprintf(l->paths[l->n], sizeof(l->paths[l->n]), "%s", path);
        snprintf(l->names[l->n], sizeof(l->names[l->n]), "%s", filename);
    }
    ++l->n;
    return true;
}

static bool qa_visit_bk_stop(const char* path, const char* filename, void* ctx) {
    (void)path;
    (void)filename;
    int* n = (int*)ctx;
    ++*n;
    return false;
}

static qa_bk_list_t qa_list_backups(void) {
    qa_bk_list_t l;
    memset(&l, 0, sizeof(l));
    CHECK(anki_progress_list_backups(qa_visit_bk, &l) == ESP_OK);
    return l;
}

static void test_qa_progress_dual_root(void) {
    printf("== qa progress dual root / 双根备份枚举 ==\n");
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(FIX "/basic-v2.apkg", &imported) == ESP_OK);
    int64_t id = qa_deck_with_cards(3);
    CHECK(id != 0);
    if (!id) return;
    char out[320] = {0}, tf_bk[320], flash_bk[320];
    /* Q3 修正：宿主活动根就是非 TF 根，导出/恢复始终可用。
       / Q3: the host active root is the non-TF root; export works anyway. */
    CHECK(strcmp(anki_store_active_root(), ANKI_DIR) == 0);
    CHECK(anki_progress_export(id, out, sizeof(out)) == ESP_OK);
    qa_akb_path(ANKI_DIR, id, flash_bk, sizeof(flash_bk));
    qa_akb_path(ANKI_DIR_TF, id, tf_bk, sizeof(tf_bk));
    CHECK(strcmp(out, flash_bk) == 0);
    /* 两根各放一份同名 .akb。/ Same filename under both roots. */
    CHECK(copy_file(flash_bk, tf_bk));
    char expect_name[64];
    snprintf(expect_name, sizeof(expect_name), "%lld.akb", (long long)id);
    qa_bk_list_t l = qa_list_backups();
    CHECK(l.n == 1);
    if (l.n >= 1) {
        CHECK(strcmp(l.names[0], expect_name) == 0);
        CHECK(strncmp(l.paths[0], ANKI_DIR, strlen(ANKI_DIR)) == 0);
        CHECK(strncmp(l.paths[0], ANKI_DIR_TF, strlen(ANKI_DIR_TF)) != 0);
    }
    /* .akb.tmp 后缀不会被误列。/ A .akb.tmp intermediate is not listed. */
    char stale[320];
    snprintf(stale, sizeof(stale), "%.200s/stale.akb.tmp", ANKI_DIR);
    CHECK(qa_write(stale, "half", 4));
    CHECK(qa_list_backups().n == 1);
    /* 另一根的不同名备份仍要列出（两根都扫，按文件名去重）。
       / A differently named backup on the other root is still listed. */
    char extra[320];
    snprintf(extra, sizeof(extra), "%.200s/extra.akb", ANKI_DIR_TF);
    CHECK(qa_write(extra, "placeholder", 11));
    qa_bk_list_t l2 = qa_list_backups();
    CHECK(l2.n == 2);
    /* 单份覆盖：再导出一次，活动根仍只有一份 <id>.akb。
       / Single overwrite: a second export keeps one <id>.akb. */
    char out2[320] = {0};
    CHECK(anki_progress_export(id, out2, sizeof(out2)) == ESP_OK);
    CHECK(strcmp(out, out2) == 0);
    CHECK(qa_count_suffix(ANKI_DIR, ANKI_BACKUP_SUFFIX) == 1);
    int stop_count = 0;
    CHECK(anki_progress_list_backups(qa_visit_bk_stop, &stop_count) == ESP_OK);
    CHECK(stop_count == 1);  // 回调返回 false 应提前结束
    /* 无 TF 卡（宿主非 TF 根）恢复仍可用，且不创建卡组。
       / Restore works without a TF card and never creates a deck. */
    anki_progress_result_t r = {0};
    CHECK(anki_progress_restore(out, &r) == ESP_OK);
    CHECK(r.deck_id == id && r.covered == 3 && r.skipped == 0);
    list_seen = 0;
    CHECK(anki_store_list(visit_summary, NULL) == ESP_OK);
    CHECK(list_seen == 2);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

/* B13 续学状态随导出走；备份无 R 段则恢复后清除 .aks。
   / B13: resume state travels with the backup; no R segment clears .aks. */
static void qa_strip_rq(const char* src, const char* dst) {
    FILE* in = fopen(src, "r");
    FILE* out = fopen(dst, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "R,", 2) == 0 || strncmp(line, "Q,", 2) == 0) continue;
        (void)fputs(line, out);
    }
    fclose(in);
    fclose(out);
}

static void test_qa_progress_resume_state(void) {
    printf("== qa progress resume state (R/Q / E10) ==\n");
    int64_t id = 0;
    anki_deck_t deck;
    int idx[ANKI_SESSION_MAX];
    int cnt = 0;
    CHECK(qa_prepare(&id, &deck, idx, &cnt));
    /* 覆盖写一份有区分度的续学状态。/ Save a distinctive resume state. */
    CHECK(anki_store_resume_save(&deck, idx, cnt, 1, 5, 2, 3) == ESP_OK);
    anki_resume_t saved = {0};
    CHECK(anki_store_resume_load(&deck, &saved) == ESP_OK);
    CHECK(saved.session_count == cnt && saved.session_pos == 1);
    CHECK(saved.studied == 5 && saved.again == 2 && saved.good_or_better == 3);

    char out[320] = {0}, aks[320], stripped[320];
    qa_aks_path(ANKI_DIR, id, aks, sizeof(aks));
    snprintf(stripped, sizeof(stripped), "%.200s/qa-no-r.akb", ANKI_DIR);
    CHECK(anki_progress_export(id, out, sizeof(out)) == ESP_OK);
    /* 导出含 R 段且 Q 行数 == session_count，字段与 .aks 一致。
       / Export carries R and Q rows matching the saved state. */
    int r_rows = 0, q_rows = 0;
    int r_count = 0, r_pos = 0, r_studied = 0, r_again = 0, r_good = 0;
    {
        FILE* f = fopen(out, "r");
        char line[512];
        while (f && fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (strncmp(line, "R,", 2) == 0) {
                ++r_rows;
                CHECK(sscanf(line, "R,%d,%d,%d,%d,%d", &r_count, &r_pos, &r_studied,
                             &r_again, &r_good) == 5);
            } else if (strncmp(line, "Q,", 2) == 0) ++q_rows;
        }
        if (f) fclose(f);
    }
    CHECK(r_rows == 1 && q_rows == cnt);
    CHECK(r_count == cnt && r_pos == 1 && r_studied == 5 && r_again == 2 && r_good == 3);

    /* 清掉 .aks 后恢复 → 按备份 R/Q 段重写，且能再次 resume_load 成功。
       / Clear .aks, restore, then the rewritten state must validate. */
    anki_store_resume_clear(id);
    CHECK(!qa_exists(aks));
    anki_progress_result_t res = {0};
    CHECK(anki_progress_restore(out, &res) == ESP_OK);
    CHECK(qa_exists(aks));
    size_t aks_len = 0;
    uint8_t* sb = qa_slurp(aks, &aks_len);
    CHECK(sb != NULL && aks_len == ANKI_RESUME_BYTES);
    free(sb);
    anki_store_unload(&deck);
    CHECK(anki_store_load(id, &deck) == ESP_OK);
    anki_resume_t restored = {0};
    CHECK(anki_store_resume_load(&deck, &restored) == ESP_OK);
    CHECK(restored.session_count == cnt && restored.session_pos == 1);
    CHECK(restored.studied == 5 && restored.again == 2 && restored.good_or_better == 3);
    for (int i = 0; i < cnt; ++i) CHECK(restored.session_ids[i] == saved.session_ids[i]);

    /* 备份不含 R 段 → 恢复后清除 .aks（E10 降级）。
       / A backup without R clears .aks on restore. */
    qa_strip_rq(out, stripped);
    CHECK(anki_store_resume_save(&deck, idx, cnt, 1, 5, 2, 3) == ESP_OK);
    CHECK(qa_exists(aks));
    anki_progress_result_t res2 = {0};
    CHECK(anki_progress_restore(stripped, &res2) == ESP_OK);
    CHECK(res2.covered == cnt && res2.skipped == 0);
    CHECK(!qa_exists(aks));

    anki_store_unload(&deck);
    qa_clean_all(ANKI_DIR);
    qa_clean_all(ANKI_DIR_TF);
}

int main(void) {
    test_json();
    test_sqlite();
    test_import_v2();
    test_import_v1();
    test_import_v3();
    test_import_big();
    test_import_bare();
    test_unzeroed_slots();
    test_modern_export();
    test_rejects();
    test_render();
    test_choice_render();
    test_store();
    test_roots();
    test_dual_root();
    /* 阶段 4 QA：断点续学 .aks 与进度导出/回写 .akb 的入库回归断言。
       / Stage-4 QA in-repo regression assertions for .aks and .akb. */
    test_qa_contract();
    test_qa_resume_roundtrip();
    test_qa_resume_invalidation();
    test_qa_resume_corruption();
    test_qa_resume_peek();
    test_qa_progress_roundtrip();
    test_qa_progress_rejects();
    test_qa_progress_atomic();
    test_qa_progress_loose();
    test_qa_progress_dual_root();
    test_qa_progress_resume_state();
    if (failures) {
        printf("anki_host_test: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("anki_host_test: PASS (%d checks)\n", checks);
    return 0;
}
