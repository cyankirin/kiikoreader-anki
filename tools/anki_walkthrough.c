/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：通用 Anki 走查工具 —— 对一份真实 .apkg 端到端跑
 *       导入→列表→载入→会话→渲染→评分→持久化→重载→删除 全链路，
 *       全部断言在宿主上执行，不碰真机。
 *       English: generic Anki walkthrough — runs the whole
 *       import→list→load→session→render→grade→persist→reload→delete chain
 *       over a real .apkg. Every assertion runs on the host; no device needed.
 *
 * 用法 / Usage:
 *   anki_walkthrough <deck.apkg>
 *   bash tools/run_anki_walkthrough.sh <deck.apkg>
 * 卡组路径必须由调用方给出（没有内置默认值，工具不认识任何本机目录）。
 * The deck path is always supplied by the caller; there is no built-in
 * default, so the tool knows nothing about any one machine's directory layout.
 *
 * 断言里的「基准卡数」对应仓库 QA 的基准卡组（rc99.03 基线 1375 卡）。
 * 换别的卡组时用 -DANKI_WALKTHROUGH_CARDS=<n> 覆盖；其余断言都自洽，
 * 不依赖具体卡组。
 * The card-count assertion encodes the repository QA baseline deck (1375
 * cards, the rc99.03 baseline). Point it at another deck by passing
 * -DANKI_WALKTHROUGH_CARDS=<n>; every other assertion is self-consistent and
 * independent of the deck.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anki_apkg.h"
#include "anki_progress.h"
#include "anki_render.h"
#include "anki_store.h"

/* 存储模块依赖 book_store 的挂载助手；宿主恒成功。/ Mount helper stub. */
esp_err_t book_store_ensure_flash(void) { return ESP_OK; }

/* 基准卡数：默认是仓库 QA 基准卡组（rc99.03 基线 1375 卡）。
/ Baseline card count: the repository QA deck by default (rc99.03 baseline,
1375 cards). Override with -DANKI_WALKTHROUGH_CARDS=<n> for another deck. */
#ifndef ANKI_WALKTHROUGH_CARDS
#define ANKI_WALKTHROUGH_CARDS 1375
#endif

static int failures;
static int checks;

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

#ifdef _WIN32
#include <direct.h>
#define walk_mkdir(path) _mkdir(path)
#else
#define walk_mkdir(path) mkdir((path), 0777)
#endif

/* 逐级建目录（宿主 -DANKI_DIR 的父级需存在；ESP 上挂载点恒在）。
/ Create parent dirs (host -DANKI_DIR parents must exist; on ESP the mount
points always do). */
static void mkdir_p(const char* path) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            walk_mkdir(buf);
            *p = '/';
        }
    }
    walk_mkdir(buf);
}

static void clean_dir(const char* root) {
    char path[320];
    DIR* dir = opendir(root);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
            remove(path);
        }
        closedir(dir);
    }
}

/* 端到端补测用的整文件字节工具（只读写磁盘，不改被测源码语义）。
   / Byte helpers for the end-to-end resume/export-restore section. */
static uint32_t w_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t w_i64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

static void w_wu32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void w_wi64(uint8_t* p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((u >> (8 * i)) & 0xFFu);
}

static uint8_t* w_slurp(const char* path, size_t* out_len) {
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

static bool w_write(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

typedef struct {
    int count;
    int64_t first_id;
    char first_name[ANKI_DECK_NAME_MAX];
    int first_total;
    int max_total;
    int64_t max_id;
    char max_name[ANKI_DECK_NAME_MAX];
} list_acc_t;

static bool visit_deck(const anki_deck_summary_t* s, void* ctx) {
    list_acc_t* acc = (list_acc_t*)ctx;
    if (acc->count == 0) {
        acc->first_id = s->deck_id;
        snprintf(acc->first_name, sizeof(acc->first_name), "%s", s->name);
        acc->first_total = s->total;
    }
    if (s->total > acc->max_total) {
        acc->max_total = s->total;
        acc->max_id = s->deck_id;
        snprintf(acc->max_name, sizeof(acc->max_name), "%s", s->name);
    }
    ++acc->count;
    return true;
}

int main(int argc, char** argv) {
    /* 卡组路径必须传参：没有默认值，也就没有本机路径。
    / The deck path is always an argument — no default, so no local path. */
    if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0') {
        fprintf(stderr, "usage: anki_walkthrough <deck.apkg>\n");
        return 2;
    }
    const char* apkg = argv[1];
    printf("== QA walkthrough: %s ==\n", apkg);

    /* ---------- 1. 直接导入解析 / raw import parse ---------- */
    anki_import_t imp = {0};
    CHECK(anki_apkg_import(apkg, &imp) == ESP_OK);
    printf("import: decks=%d notes=%d cards=%d notetypes=%d\n", imp.deck_count,
           imp.note_count, imp.card_count, imp.notetype_count);
    CHECK(imp.deck_count >= 1);
    /* 基准卡数（默认 rc99.03 基线 1375 卡）。
    / Baseline card count (1375 cards, the rc99.03 baseline, by default). */
    CHECK(imp.card_count == ANKI_WALKTHROUGH_CARDS);
    CHECK(imp.note_count > 0);
    CHECK(imp.notetype_count > 0);

    /* 选择题检测：正确选项行与「答案：X」行带 \x01 粗体标记，错误项
       保持常规黑字。
       / Choice model detection: correct option lines and the answer line
         carry the \x01 bold marker while wrong options stay plain black. */
    anki_collection_t col;
    anki_collection_from_import(&imp, &col);
    int choice_cards = 0;
    int single_choice_cards = 0;
    int multi_choice_cards = 0;
    int option_total = 0;
    int marked_total = 0;
    int answer_marked = 0;
    static char front[ANKI_CARD_TEXT_MAX], back[ANKI_CARD_TEXT_MAX];
    for (int i = 0; i < imp.card_count; ++i) {
        if (anki_render_front(&col, &imp.cards[i], front, sizeof(front)) != ESP_OK) continue;
        if (anki_render_back(&col, &imp.cards[i], front, back, sizeof(back)) != ESP_OK) continue;
        int options = 0, marked = 0, option_marks = 0;
        for (const char* p = back; *p;) {
            const char* nl = strchr(p, '\n');
            const size_t len = nl ? (size_t)(nl - p) : strlen(p);
            const bool bold = len > 0 && p[0] == '\x01';
            const char* body = bold ? p + 1 : p;
            const size_t blen = len - (bold ? 1u : 0u);
            const bool is_option = blen >= 3 && body[0] >= 'A' && body[0] <= 'Z' &&
                                   body[1] == '.' && body[2] == ' ';
            if (is_option) ++options;
            if (bold) ++marked;
            if (is_option && bold) ++option_marks;
            if (blen >= 9 && strncmp(body, "答案：", 9) == 0 && bold) ++answer_marked;
            if (!nl) break;
            p = nl + 1;
        }
        if (options < 2) continue;
        ++choice_cards;
        option_total += options;
        marked_total += marked;
        // 标记落在正确项上，答案行另算，所以正确项数 = 带标记的选项行数。
        // Marked lines are the correct options; the answer line is counted
        // separately, so correct == marked option lines.
        const int correct = option_marks;
        if (correct >= 2) ++multi_choice_cards;
        else if (correct == 1) ++single_choice_cards;
    }
    printf("choice backs: cards=%d single=%d multi=%d options=%d bold-marked=%d answer-marked=%d\n",
           choice_cards, single_choice_cards, multi_choice_cards, option_total, marked_total,
           answer_marked);
    CHECK(choice_cards > 0);
    CHECK(single_choice_cards > 0);
    CHECK(multi_choice_cards > 0);
    CHECK(answer_marked > 0); /* 答案陈述必须黑色加粗 / the answer must draw black bold */

    /* ---------- 2. 落盘导入 / on-disk import ---------- */
    clean_dir(ANKI_DIR);
    clean_dir(ANKI_DIR_TF);
    mkdir_p(ANKI_DIR);
    mkdir_p(ANKI_DIR_TF);
    int imported = 0;
    CHECK(anki_store_import(apkg, &imported) == ESP_OK);
    CHECK(imported == imp.deck_count);
    printf("store import: %d decks\n", imported);

    /* ---------- 3. 列表 / list ---------- */
    list_acc_t acc = {0};
    CHECK(anki_store_list(visit_deck, &acc) == ESP_OK);
    CHECK(acc.count == imp.deck_count);
    printf("list: %d decks, first=%s(%d cards), max=%s(%d cards)\n", acc.count, acc.first_name,
           acc.first_total, acc.max_name, acc.max_total);

    /* ---------- 4. 载入最大卡组 / load the largest deck ---------- */
    anki_deck_t deck = {0};
    CHECK(anki_store_load(acc.max_id, &deck) == ESP_OK);
    CHECK(deck.card_count == acc.max_total);
    CHECK(strncmp(deck.path, ANKI_DIR, strlen(ANKI_DIR)) == 0);
    printf("load: %s cards=%d path=%s\n", deck.name, deck.card_count, deck.path);

    /* ---------- 5. 会话 / session ---------- */
    static int session[ANKI_SESSION_MAX];
    int count = anki_store_session(&deck, session, ANKI_SESSION_MAX);
    printf("session: %d cards (cap %d)\n", count, ANKI_SESSION_MAX);
    CHECK(count > 0);
    CHECK(count <= ANKI_SESSION_MAX); /* rc99.03 基线：50 / baseline: 50 */

    /* ---------- 6. 渲染当前会话的卡片 / render the session ---------- */
    anki_collection_t dcol = {
        .notetypes = deck.notetypes,
        .notetype_count = deck.notetype_count,
        .notes = deck.notes,
        .note_count = deck.note_count,
    };
    int rendered = 0, bold_lines = 0;
    for (int i = 0; i < count; ++i) {
        const anki_card_t* card = &deck.cards[session[i]];
        if (anki_render_front(&dcol, card, front, sizeof(front)) != ESP_OK) continue;
        if (anki_render_back(&dcol, card, front, back, sizeof(back)) != ESP_OK) continue;
        if (front[0] && back[0]) {
            ++rendered;
            /* 加粗行数 = 正确选项行 + 答案行。/ Bold lines = correct options plus the answer. */
            for (const char* p = back; (p = strchr(p, '\x01')) != NULL; ++p) ++bold_lines;
        }
    }
    printf("rendered: %d/%d session cards, %d bold (correct option / answer) lines\n", rendered, count,
           bold_lines);
    CHECK(rendered == count);

    /* ---------- 7. 评分并持久化 / grade and persist ---------- */
    int64_t day = anki_store_sync_day(&deck);
    for (int i = 0; i < count && i < 8; ++i) {
        int idx = session[i];
        anki_grade_t grade = (anki_grade_t)(i % 4);
        anki_card_t before = deck.cards[idx];
        anki_schedule(&deck, idx, grade);
        CHECK(deck.cards[idx].reps == before.reps + 1);
        CHECK(anki_store_save_card(&deck, idx) == ESP_OK);
    }
    printf("graded+persisted 8 cards (day=%lld)\n", (long long)day);

    /* ---------- 8. 重载验证进度落盘 / reload and verify ---------- */
    anki_deck_t reloaded = {0};
    CHECK(anki_store_load(acc.max_id, &reloaded) == ESP_OK);
    CHECK(reloaded.card_count == deck.card_count);
    int persisted = 0;
    for (int i = 0; i < count && i < 8; ++i) {
        int idx = session[i];
        if (reloaded.cards[idx].reps == deck.cards[idx].reps &&
            reloaded.cards[idx].type == deck.cards[idx].type &&
            reloaded.cards[idx].queue == deck.cards[idx].queue &&
            reloaded.cards[idx].ivl == deck.cards[idx].ivl &&
            reloaded.cards[idx].due == deck.cards[idx].due)
            ++persisted;
    }
    printf("persisted: %d/8 cards survived reload\n", persisted);
    CHECK(persisted == 8 || (count < 8 && persisted == count));
    anki_store_unload(&reloaded);

    /* ---------- 8b. 断点续学 + 导出/恢复 端到端 ----------
       / 8b. Resume + export/restore end to end (stage-4 QA).
       不依赖具体卡数，任意 .apkg 都自洽。/ Independent of the deck's card count. */
    anki_store_sync_day(&deck);
    static int resume_idx[ANKI_SESSION_MAX];
    int resume_cnt = anki_store_session(&deck, resume_idx, ANKI_SESSION_MAX);
    CHECK(resume_cnt > 0);
    int resume_pos = resume_cnt > 1 ? 1 : 0;
    CHECK(anki_store_resume_save(&deck, resume_idx, resume_cnt, resume_pos, 3, 1, 2) ==
          ESP_OK);
    anki_resume_t snap = {0};
    CHECK(anki_store_resume_load(&deck, &snap) == ESP_OK);
    CHECK(snap.session_count == resume_cnt && snap.session_pos == resume_pos);
    CHECK(snap.studied == 3 && snap.again == 1 && snap.good_or_better == 2);
    static int resume_map[ANKI_SESSION_MAX];
    CHECK(anki_store_resume_indices(&deck, &snap, resume_map, ANKI_SESSION_MAX) ==
          resume_cnt);

    char backup[320] = {0};
    CHECK(anki_progress_export(acc.max_id, backup, sizeof(backup)) == ESP_OK);
    CHECK(backup[0] != 0);
    struct stat bst;
    CHECK(stat(backup, &bst) == 0 && bst.st_size > 0);

    /* 快照 .akd，再模拟「换机后 day=0 + 卡片调度字段受损」。
       / Snapshot .akd, then simulate a fresh device (day=0) plus damaged cards. */
    size_t akd_len = 0;
    uint8_t* akd_before = w_slurp(deck.path, &akd_len);
    CHECK(akd_before != NULL && akd_len >= AKD_HEADER_BYTES);
    int64_t saved_day = 0, saved_rtc = 0;
    uint32_t table_at = 0, cards = 0;
    if (akd_before) {
        saved_day = w_i64(akd_before + AKD_OFF_DAY);
        saved_rtc = w_i64(akd_before + AKD_OFF_LAST_RTC_DAY);
        table_at = w_u32(akd_before + AKD_OFF_TABLE_AT);
        cards = w_u32(akd_before + AKD_OFF_CARD_COUNT);
        uint8_t* damaged = malloc(akd_len);
        CHECK(damaged != NULL);
        if (damaged) {
            memcpy(damaged, akd_before, akd_len);
            w_wi64(damaged + AKD_OFF_DAY, 0);
            w_wi64(damaged + AKD_OFF_LAST_RTC_DAY, 0);
            for (uint32_t k = 0; k < cards && k < 2; ++k) {
                uint8_t* rec = damaged + table_at + (size_t)k * AKD_CARD_RECORD;
                for (int j = 0; j < 7; ++j) {
                    w_wu32(rec + AKD_CARD_OFF_TYPE + (size_t)j * 4,
                           0x00C0FFEEu + (unsigned)j);
                }
            }
            CHECK(w_write(deck.path, damaged, akd_len));
            free(damaged);
        }
    }

    anki_progress_result_t pres = {0};
    CHECK(anki_progress_restore(backup, &pres) == ESP_OK);
    CHECK(pres.deck_id == acc.max_id);
    CHECK(pres.covered == (int)cards && pres.skipped == 0);
    size_t akd_after_len = 0;
    uint8_t* akd_after = w_slurp(deck.path, &akd_after_len);
    CHECK(akd_after != NULL && akd_after_len == akd_len);
    if (akd_after && akd_before && akd_after_len == akd_len) {
        /* R-1：头 day/last_rtc_day 实际写回；I1：其余字节逐字节不变。
           / R-1 header day write-back; I1 byte invariance outside allowed ranges. */
        CHECK(w_i64(akd_after + AKD_OFF_DAY) == saved_day);
        CHECK(w_i64(akd_after + AKD_OFF_LAST_RTC_DAY) == saved_rtc);
        size_t diff = 0;
        for (size_t k = 0; k < akd_len; ++k) {
            if (k >= (size_t)AKD_OFF_DAY && k < (size_t)AKD_OFF_DAY + 16) continue;
            bool allowed = false;
            if (k >= table_at) {
                size_t rel = k - table_at;
                size_t rec = rel / AKD_CARD_RECORD;
                size_t off = rel % AKD_CARD_RECORD;
                if (rec < cards && off >= (size_t)AKD_CARD_OFF_TYPE &&
                    off < (size_t)AKD_CARD_OFF_RESERVED) {
                    allowed = true;
                }
            }
            if (!allowed && akd_after[k] != akd_before[k]) ++diff;
        }
        CHECK(diff == 0);
    }
    free(akd_before);
    free(akd_after);
    /* 列表摘要新字段存在且不阻塞枚举。/ The list still enumerates every deck. */
    list_acc_t after_list = {0};
    CHECK(anki_store_list(visit_deck, &after_list) == ESP_OK);
    CHECK(after_list.count == imp.deck_count);

    /* ---------- 9. 删除 / delete ---------- */
    CHECK(anki_store_delete(acc.max_id) == ESP_OK);
    struct stat st;
    char path[320];
    snprintf(path, sizeof(path), "%s/%lld.akd", ANKI_DIR, (long long)acc.max_id);
    CHECK(stat(path, &st) != 0);
    snprintf(path, sizeof(path), "%s/%lld.akd", ANKI_DIR_TF, (long long)acc.max_id);
    CHECK(stat(path, &st) != 0);
    /* 其余卡组也清掉，收尾。/ Clean the rest for a tidy finish. */
    CHECK(anki_store_delete(acc.first_id) == ESP_OK);

    anki_store_unload(&deck);
    anki_apkg_free(&imp);

    if (failures) {
        printf("QA WALKTHROUGH: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("QA WALKTHROUGH: PASS (%d checks)\n", checks);
    return 0;
}
