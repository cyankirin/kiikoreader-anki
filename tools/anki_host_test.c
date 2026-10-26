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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anki_apkg.h"
#include "anki_json.h"
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

// 选择题背面强调：错误选项行带 \x01（反色绘制）标记；正确选项与「答案：X」
// 行不带标记（墨水屏上反色白字看不清，正确项与答案陈述必须保持常规黑字）。
// 背面结构：题干 → 空行 → 选项 → 空行 → 答案 →（备注）。
// Choice-back emphasis: wrong option lines carry the \x01 (inverted draw)
// marker; the correct option and the "答案：X" line do not (inverted white on
// e-paper is hard to read, so the correct option and the answer statement
// keep ordinary black ink). The back layout is: question, blank line,
// options, blank line, answer and the optional note.
static void test_choice_render(void) {
    printf("== choice render ==\n");
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(FIX "/choice-v2.apkg", &imp) == ESP_OK);
    anki_collection_t col;
    anki_collection_from_import(&imp, &col);
    static char front[ANKI_CARD_TEXT_MAX], back[ANKI_CARD_TEXT_MAX];
    if (imp.card_count == 2) {
        // 单选卡：T="A"，只有错误项 B/C/D 被反色。/ Single-choice card: T="A",
        // only the wrong options B/C/D are inverted.
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(strstr(front, "\x01") == NULL); // 正面无强调标记 / no marker on the front
        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        // 整篇背面逐行钉死：题干、空行、选项（错误项行首带 \x01）、空行、
        // 答案行（不带标记），注释为空故无备注行。
        // The whole back is pinned line by line: question, blank line,
        // options (wrong ones lead with \x01), blank line, answer line
        // (no marker); the empty note means no note line.
        CHECK(strcmp(back,
                     "我国依法负责对业余无线电台实施监督管理的机构是：\n"
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
        CHECK(strstr(back, "\n\x01" "A. ") == NULL);  // 正确项不反色 / correct option not inverted
        CHECK(strstr(back, "\n\x01" "B. ") != NULL);  // 错误项反色 / wrong option inverted
        CHECK(strstr(back, "\n\x01" "C. ") != NULL);
        CHECK(strstr(back, "\n\x01" "D. ") != NULL);
        CHECK(strstr(back, "\n答案：A") != NULL);      // 答案行常规 / answer line plain
        CHECK(strstr(back, "\n\x01" "答案") == NULL);  // 答案行不反色 / answer not inverted
        CHECK(strstr(back, "：\n\nA. ") != NULL);      // 题干后有空行 / blank line after the question
        CHECK(strstr(back, "门\n\n答案：A") != NULL);  // 选项后有空行 / blank line after the options
        CHECK(strstr(back, "备注：") == NULL); // 注释为空无备注行 / no note line when N is empty

        // 多选卡：T="AC"，A/C 常规，B/D 反色。/ Multiple-choice card: T="AC",
        // A and C stay plain while B and D are inverted.
        CHECK(anki_render_front(&col, &imp.cards[1], front, sizeof(front)) == ESP_OK);
        CHECK(strstr(front, "\x01") == NULL);
        CHECK(anki_render_back(&col, &imp.cards[1], front, back, sizeof(back)) == ESP_OK);
        CHECK(strcmp(back,
                     "我国专门针对无线电管理的行政法规及其制定机构是：\n"
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
        CHECK(strstr(back, "\n备注：参见《条例》第一章") != NULL); // 注释非空出备注行 / note line when N is set
    }
    anki_apkg_free(&imp);

    // 回归保护：普通（非选择题）夹具背面不含强调标记。/ Regression guard:
    // an ordinary (non-choice) fixture's back carries no emphasis marker.
    imp = (anki_import_t){0};
    CHECK(anki_apkg_import(FIX "/basic-v2.apkg", &imp) == ESP_OK);
    anki_collection_from_import(&imp, &col);
    if (imp.card_count == 3) {
        CHECK(anki_render_front(&col, &imp.cards[0], front, sizeof(front)) == ESP_OK);
        CHECK(anki_render_back(&col, &imp.cards[0], front, back, sizeof(back)) == ESP_OK);
        CHECK(strstr(back, "\x01") == NULL);
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
    if (failures) {
        printf("anki_host_test: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("anki_host_test: PASS (%d checks)\n", checks);
    return 0;
}
