/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡片学习页。卡组列表、.apkg 导入、学习会话（正面/翻面/评分）与
 *       会话小结；进度由 anki_store 落盘。
 * English: Anki study page. Deck list, .apkg import, study session
 *          (front/flip/grade) and a session summary; progress persists through
 *          anki_store.
 *
 * 冻结：导入同步执行并先绘“导入中”；长卡片分页显示；无字体走 no-font 页。
 * Frozen: import runs synchronously after painting a busy message; long cards
 * page; the no-font page shows without a font.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "app_content_open.h"
#include "anki_layout.h"
#include "anki_render.h"
#include "anki_store.h"
#include "anki_progress.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "ui_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_anki"

#define ANKI_TITLE "Anki 卡组"
#define ANKI_DETAIL "Anki 卡片学习 Cards"

#define ROW_H 92
#define LIST_MAX 96
// 单卡正文行数上限：超长卡片到此封顶，避免折行数组无界增长。220 时超长
#define APKG_DIRS 2

typedef enum {
    ANKI_VIEW_DECKS = 0,
    ANKI_VIEW_FILES,
    ANKI_VIEW_BUSY,
    ANKI_VIEW_STUDY,
    ANKI_VIEW_SUMMARY,
} anki_view_t;

typedef struct {
    char name[128];
    char path[288];
    off_t size;
} apkg_item_t;

// 列表与行缓冲放 PSRAM，内部 RAM 已经紧张。/ Lists and line buffers live in
// PSRAM; internal RAM is already tight.
typedef struct {
    anki_deck_summary_t* decks;
    int deck_count;
    apkg_item_t* files;
    int file_count;
} anki_lists_t;

typedef struct {
    bool active;
    anki_deck_t deck;
    int session[ANKI_SESSION_MAX];
    int session_count;
    int session_pos;
    bool show_back;
    bool back_loaded;
    char front[ANKI_CARD_TEXT_MAX];
    char back[ANKI_CARD_TEXT_MAX];
    // 折行结果：行号、高度、缩进与粗体标记都归排版单元管。
    // The wrapped lines: offsets, heights, indents and the emphasis flags
    // all belong to the layout unit.
    anki_layout_t layout;
    int studied;
    int again;
    int good_or_better;
} anki_session_t;

static anki_lists_t* s_lists;
static anki_view_t s_view = ANKI_VIEW_DECKS;
static int s_page;                 // 列表分页 / list page
static bool s_no_font;
// 锁屏前卸掉会话时置位：卡组列表的计数已过时，等解锁后第一次重绘再重扫。
// 不在锁屏那一刻重扫——那会把每个 .akd 整份读回内存，正好吃掉刚让出来的空间。
// / Set when the session is dropped before locking: the list counts are stale,
// so rescan on the first repaint after unlocking. Never rescan at lock time —
// that reads every .akd back into memory and eats the room just freed.
static bool s_rescan_pending;
static char s_message[96];
// 会话状态（含两块 16KB 卡面缓冲）常驻 PSRAM，避免占内部 RAM。
// Session state (with two 16KB card buffers) stays in PSRAM, off internal RAM.
static anki_session_t* s_session;
// FILES 视图复用：0 = .apkg 导入，1 = .akb 恢复；二次确认选中的行（-1 = 无）。
// FILES view reuse: 0 = .apkg import, 1 = .akb restore; -1 = no confirmation.
static int s_file_mode;
static int s_confirm_index = -1;

// Anki 是系统页：所有视图（卡组列表/文件/忙碌/学习/小结）统一走系统字体，
// 跟随「设置」里指定的系统字体；主循环每次切页都会先
// app_font_activate_system()（见 app_loop.c 的 switch_to），进入本页无需
// 再显式切换。阅读字体只属于书页正文，与学习页无关。/ Anki is a system
// page: every view (deck list, files, busy, study, summary) uses the system
// face and follows the system font chosen in Settings; the main loop calls
// app_font_activate_system() on every page switch (see switch_to in
// app_loop.c), so entering this page needs no explicit switch. The reading
// face belongs to book body text only and has nothing to do with the study
// view.

static anki_session_t* session_state(void) {
    if (!s_session) {
        s_session = heap_caps_calloc(1, sizeof(*s_session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_session;
}

// ---------------------------------------------------------------- 内存水位

/// 打印一次内存水位：PSRAM 与内部 RAM 的剩余量，外加 PSRAM 最大连续块。
/// 设备上的日志级别是 ERROR（WARN/INFO 已被编译掉），所以这里用 ESP_LOGE——
/// 用户接串口就能读到「学了几次之后还剩多少」，这是判断锁屏重启是否为内存
/// 耗尽、以及字体为何加载失败的唯一现场证据。
/// Print one memory watermark: free PSRAM, free internal RAM and the largest
/// contiguous PSRAM block. The device builds at ERROR level (WARN/INFO are
/// compiled out), so this uses ESP_LOGE — a user with a serial console then
/// reads "how much is left after N sessions", the only field evidence for
/// whether the lock-time reboot is memory exhaustion and why the font failed.
/// 点位固定三个：进入学习、退出学习、进入锁屏。/ Three fixed points: study
/// enter, study exit, lock enter.
static void anki_heap_log(const char* where) {
#ifdef ESP_PLATFORM
    ESP_LOGE(TAG, "heap %s: psram=%u internal=%u psram_block=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    (void)where; // 宿主没有 caps 分区堆 / the host has no caps heaps
#endif
}

// ---------------------------------------------------------------- 工具

#define SESS() (session_state())

static void lists_alloc(void) {
    if (s_lists) return;
    s_lists = heap_caps_calloc(1, sizeof(*s_lists), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lists) return;
    s_lists->decks = heap_caps_calloc(LIST_MAX, sizeof(anki_deck_summary_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lists->files = heap_caps_calloc(LIST_MAX, sizeof(apkg_item_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool lists_ready(void) {
    lists_alloc();
    return s_lists && s_lists->decks && s_lists->files;
}

static int rows_per_page(void) {
    return (UI_CONTENT_BOTTOM - UI_CONTENT_TOP - 8) / ROW_H;
}

static int page_count(int total) {
    int rows = rows_per_page();
    return total > 0 ? 1 + (total - 1) / rows : 1;
}

static EpdRect row_rect(int index) {
    int rows = rows_per_page();
    int local = index - s_page * rows;
    return (EpdRect){.x = UI_MARGIN,
                     .y = UI_CONTENT_TOP + local * ROW_H,
                     .width = ui_content_width(),
                     .height = ROW_H - UI_GAP};
}

static EpdRect row_delete_rect(int index) {
    EpdRect row = row_rect(index);
    row.x = row.x + row.width - 56;
    row.width = 48;
    row.y += 8;
    row.height -= 16;
    return row;
}

/// 行内导出钮 ↑：[532,580)，与删除钮 [588,636) 间隙 8 px，命中区不重叠。
/// In-row export button: [532,580), 8 px clear of delete [588,636).
static EpdRect row_export_rect(int index) {
    EpdRect row = row_rect(index);
    row.x = row.x + row.width - 112;
    row.width = 48;
    row.y += 8;
    row.height -= 16;
    return row;
}

/// 底部「恢复」：第二格 (272,1096,220,96)，右端 492 < 可用右界。
/// Bottom-bar "restore": second cell (272,1096,220,96).
static EpdRect bar_restore_rect(void) {
    return (EpdRect){.x = UI_MARGIN + 220 + UI_GAP,
                     .y = UI_BAR_TOP,
                     .width = 220,
                     .height = UI_BAR_H};
}

static EpdRect bar_button_rect(void) {
    return (EpdRect){.x = UI_MARGIN, .y = UI_BAR_TOP, .width = 220, .height = UI_BAR_H};
}

static void message_set(const char* text) {
    if (!text || !*text) {
        s_message[0] = 0;
        return;
    }
    snprintf(s_message, sizeof(s_message), "%s", text);
}

/// 恢复结果文案按 §2.5 错误码映射；成功汇总「覆盖 N 张 / 跳过 M 张」。
/// Map restore results to the §2.5 error codes; report covered/skipped.
static void restore_message(esp_err_t err, const anki_progress_result_t* result) {
    char line[96];
    if (err == ESP_OK) {
        snprintf(line, sizeof(line), "覆盖 %d 张 / 跳过 %d 张", result->covered,
                 result->skipped);
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(line, sizeof(line), "%s", "未找到对应卡组，请先导入原 .apkg");
    } else if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE) {
        snprintf(line, sizeof(line), "%s", "备份文件无法识别");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        snprintf(line, sizeof(line), "%s", "卡组过大，暂不支持恢复（上限 20000 张）");
    } else {
        snprintf(line, sizeof(line), "%s", "恢复失败，进度未改变");
    }
    message_set(line);
}

/// 取路径末段文件名，提示语里只显示它。/ Last path segment; only it is shown in hints.
static const char* base_name(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

// 待导入的文件路径；文件页点开 .apkg 与页内选择共用。/ Pending import path,
// shared by the file-page entry and in-page selection.
static char s_pending_path[288];
static bool s_pending_import;

static void size_text(off_t size, char out[24]) {
    if (size >= 1024 * 1024) snprintf(out, 24, "%lld.%lld MB", (long long)(size >> 20),
                                      (long long)((size >> 10) & 1023) / 103);
    else snprintf(out, 24, "%lld KB", (long long)(size / 1024));
}

// ---------------------------------------------------------------- 列表扫描

static bool visit_deck(const anki_deck_summary_t* summary, void* ctx) {
    // 0 卡空卡组不进列表：导入 .apkg 常带一个没有卡片的 Default，点进去也
    // 无卡可学，用户多次被它困扰；store 层语义不变（仍枚举、仍可删），
    // 只在界面列表层过滤。/ Keep 0-card decks out of the list: an imported
    // .apkg often carries a card-less Default that teaches nothing when
    // opened and has annoyed users repeatedly; the store layer is unchanged
    // (still enumerated, still deletable) and only the UI list filters.
    if (summary->total <= 0) return true;
    anki_lists_t* lists = (anki_lists_t*)ctx;
    if (lists->deck_count >= LIST_MAX) return false;
    lists->decks[lists->deck_count++] = *summary;
    return true;
}

static void scan_decks(void) {
    if (!lists_ready()) return;
    s_lists->deck_count = 0;
    anki_store_list(visit_deck, s_lists);
    if (s_page >= page_count(s_lists->deck_count)) s_page = page_count(s_lists->deck_count) - 1;
    if (s_page < 0) s_page = 0;
}

static void scan_apkg(void) {
    if (!lists_ready()) return;
    s_lists->file_count = 0;
    static const char* dirs[APKG_DIRS] = {"/sdcard/anki", "/sdcard"};
    for (int d = 0; d < APKG_DIRS; ++d) {
        DIR* dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL && s_lists->file_count < LIST_MAX) {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || strcasecmp(dot, ".apkg") != 0) continue;
            if (entry->d_name[0] == '.') continue; // Mac  AppleDouble / hidden
            char path[288];
            if (snprintf(path, sizeof(path), "%s/%s", dirs[d], entry->d_name) >=
                (int)sizeof(path)) {
                continue;
            }
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            apkg_item_t* item = &s_lists->files[s_lists->file_count];
            // d_name 最长 255，name 只有 128；显式限宽，避免 -O2 下
            // -Werror=format-truncation 报警。/ Explicit width: d_name is up to
            // 255 bytes while name holds 128.
            snprintf(item->name, sizeof(item->name), "%.127s", entry->d_name);
            snprintf(item->path, sizeof(item->path), "%s", path);
            item->size = st.st_size;
            ++s_lists->file_count;
        }
        closedir(dir);
    }
    if (s_page >= page_count(s_lists->file_count)) s_page = page_count(s_lists->file_count) - 1;
    if (s_page < 0) s_page = 0;
}

/// 备份枚举回调：复用 s_lists->files[]，显式清零新增项（0xAA 约定）。
/// Backup visitor: reuse s_lists->files[], explicitly zero each new item.
static bool visit_backup(const char* path, const char* filename, void* ctx) {
    anki_lists_t* lists = (anki_lists_t*)ctx;
    if (lists->file_count >= LIST_MAX) return false;
    apkg_item_t* item = &lists->files[lists->file_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->name, sizeof(item->name), "%.127s", filename);
    snprintf(item->path, sizeof(item->path), "%.280s", path);
    struct stat st;
    item->size = stat(path, &st) == 0 ? st.st_size : 0;
    ++lists->file_count;
    return true;
}

static void scan_backups(void) {
    if (!lists_ready()) return;
    s_lists->file_count = 0;
    anki_progress_list_backups(visit_backup, s_lists);
    if (s_page >= page_count(s_lists->file_count)) s_page = page_count(s_lists->file_count) - 1;
    if (s_page < 0) s_page = 0;
}

// 学习页的内容区：首行上沿、末行下沿，以及底部提示行（正面）与评分按钮
// （背面）各占多高。/ The study page's content band: where the first line
// starts, where the content ends, and the room for the bottom hint (front)
// or the grade buttons (back).
static anki_band_t study_band(void) {
    return (anki_band_t){
        .top = UI_CONTENT_TOP + 8,
        .bottom = UI_CONTENT_BOTTOM - 8,
        .hint_block_h = UI_PX_CAPTION + 10,
        .buttons_block_h = UI_BTN_H + 4,
    };
}

static void session_load_back(void);

static void session_open_card(void) {
    SESS()->show_back = false;
    SESS()->back_loaded = false;
    SESS()->layout.text = SESS()->front;
    SESS()->layout.show_back = false;
    SESS()->layout.band = study_band();
    anki_layout_run(&SESS()->layout, ui_text_effective_px(UI_PX_BODY));
}

static void session_show_back(void) {
    SESS()->show_back = true;
    if (!SESS()->back_loaded) session_load_back();
    SESS()->layout.text = SESS()->back;
    SESS()->layout.show_back = true;
    SESS()->layout.band = study_band();
    anki_layout_run(&SESS()->layout, ui_text_effective_px(UI_PX_BODY));
}

/// 已载入卡组的内容视图。/ Content view over the loaded deck.
static anki_collection_t session_collection(void) {
    anki_collection_t col = {
        .notetypes = SESS()->deck.notetypes,
        .notetype_count = SESS()->deck.notetype_count,
        .notes = SESS()->deck.notes,
        .note_count = SESS()->deck.note_count,
    };
    return col;
}

static void session_load_back(void) {
    anki_collection_t col = session_collection();
    const anki_card_t* card = &SESS()->deck.cards[SESS()->session[SESS()->session_pos]];
    if (anki_render_back(&col, card, SESS()->front, SESS()->back, sizeof(SESS()->back)) != ESP_OK) {
        snprintf(SESS()->back, sizeof(SESS()->back), "[卡片渲染失败]");
    }
    SESS()->back_loaded = true;
}

static void session_load_front(void) {
    anki_collection_t col = session_collection();
    const anki_card_t* card = &SESS()->deck.cards[SESS()->session[SESS()->session_pos]];
    if (anki_render_front(&col, card, SESS()->front, sizeof(SESS()->front)) != ESP_OK) {
        snprintf(SESS()->front, sizeof(SESS()->front), "[卡片渲染失败]");
    }
}

static void session_start(int deck_index) {
    if (!lists_ready() || deck_index < 0 || deck_index >= s_lists->deck_count) return;
    int64_t id = s_lists->decks[deck_index].deck_id;
    if (anki_store_load(id, &SESS()->deck) != ESP_OK) {
        message_set("卡组载入失败");
        s_view = ANKI_VIEW_DECKS;
        return;
    }
    // 日基准先推进：续学的 day 绑定与今日队列都以此为准（E4）。
    // Advance the day first: resume day binding and today's queue share it.
    anki_store_sync_day(&SESS()->deck);
    bool resumed = false;
    anki_resume_t snap = {0};
    if (anki_store_resume_load(&SESS()->deck, &snap) == ESP_OK) {
        int indices[ANKI_SESSION_MAX] = {0};
        int n = anki_store_resume_indices(&SESS()->deck, &snap, indices,
                                          ANKI_SESSION_MAX);
        if (n > 0 && n == snap.session_count) {
            memset(SESS()->session, 0, sizeof(SESS()->session));  // 0xAA 约定
            memcpy(SESS()->session, indices, sizeof(int) * (size_t)n);
            SESS()->session_count = n;
            SESS()->session_pos = snap.session_pos;
            SESS()->studied = snap.studied;
            SESS()->again = snap.again;
            SESS()->good_or_better = snap.good_or_better;
            resumed = true;
        }
    }
    if (!resumed) {
        SESS()->session_count =
            anki_store_session(&SESS()->deck, SESS()->session, ANKI_SESSION_MAX);
        SESS()->session_pos = 0;
        SESS()->studied = 0;
        SESS()->again = 0;
        SESS()->good_or_better = 0;
        // 重建后补写初始状态（pos=0）：一张没学就离开也能续，并固定 day。
        // Persist the initial state so leaving before the first grade resumes.
        (void)anki_store_resume_save(&SESS()->deck, SESS()->session,
                                     SESS()->session_count, 0, 0, 0, 0);
    }
    SESS()->active = true;
    // 整库已载入：这里是 Anki 页内存占用的峰值点位。
    // The whole deck is loaded: this is the Anki page's memory peak.
    anki_heap_log("anki study enter");
    if (SESS()->session_count == 0) {
        s_view = ANKI_VIEW_SUMMARY;
        return;
    }
    session_load_front();
    session_open_card();
    s_view = ANKI_VIEW_STUDY;
}

static void session_end(void) {
    anki_store_unload(&SESS()->deck);
    SESS()->active = false;
    s_view = ANKI_VIEW_DECKS;
    scan_decks();
    anki_heap_log("anki exit");
}

/// 卸掉学习会话，把整库占用的 PSRAM 还给堆，并把页面摆回一个自洽的状态。
/// 逐张进度在评分时已经落盘，所以这里丢掉会话队列不丢学习进度；返回卡组
/// 列表而不是留在只剩空指针的学习页，解锁后的重绘因此不会碰到已释放的
/// 卡表（那是 use-after-free）。
/// Drop the study session and hand the whole deck's PSRAM back, leaving the
/// page in a self-consistent state. Per-card progress was already written on
/// every grade, so discarding the queue loses no study progress; the page
/// falls back to the deck list instead of staying on a study view whose card
/// table is gone (which the post-unlock repaint would use after free).
static void session_release(void) {
    if (!SESS()) return;
    if (SESS()->active) anki_store_unload(&SESS()->deck);
    SESS()->active = false;
    SESS()->session_count = 0;
    SESS()->session_pos = 0;
    SESS()->show_back = false;
    SESS()->back_loaded = false;
    SESS()->front[0] = 0;
    SESS()->back[0] = 0;
    SESS()->layout.line_count = 0;
    SESS()->layout.line_page = 0;
    SESS()->layout.text = SESS()->front;
    if (s_view == ANKI_VIEW_STUDY || s_view == ANKI_VIEW_SUMMARY) s_view = ANKI_VIEW_DECKS;
    if (s_page < 0) s_page = 0;
}

static void session_next(void) {
    ++SESS()->session_pos;
    if (SESS()->session_pos >= SESS()->session_count) {
        // E3：走到小结视为本轮结束，清除续学状态；下一次重建今日队列。
        // 切勿在 session_release / anki_on_exit / anki_before_lock 里 clear（E1）。
        // E3: reaching the summary ends the round; never clear on the leave
        // paths (session_release / anki_on_exit / anki_before_lock, E1).
        anki_store_resume_clear(SESS()->deck.deck_id);
        s_view = ANKI_VIEW_SUMMARY;
        return;
    }
    session_load_front();
    session_open_card();
}

static void session_grade(anki_grade_t grade) {
    if (s_view != ANKI_VIEW_STUDY || !SESS()->show_back) return;
    int index = SESS()->session[SESS()->session_pos];
    anki_schedule(&SESS()->deck, index, grade);
    // 进度写回失败（如学习途中拔出 TF 卡）不打断会话：卡组已在内存里，
    // 本次评分照常继续，落盘等下次保存重试。/ A failed progress write (the
    // TF card pulled mid-study, say) never interrupts the session: the deck
    // is in memory, this grade proceeds, and persistence retries on the next
    // save.
    anki_store_save_card(&SESS()->deck, index);
    ++SESS()->studied;
    if (grade == ANKI_GRADE_AGAIN) ++SESS()->again;
    else ++SESS()->good_or_better;
    // 逐卡进度已落盘；紧接着覆盖写续学状态（游标前进一步）。写失败只记日志，
    // 绝不打断会话、绝不回滚已成功的评分（I3 / E12）。
    // Per-card progress is already on disk; then overwrite the resume state.
    // A failed state write never interrupts the session or rolls back a grade.
    int next_pos = SESS()->session_pos + 1;
    if (next_pos < SESS()->session_count) {
        esp_err_t state_err = anki_store_resume_save(
            &SESS()->deck, SESS()->session, SESS()->session_count, next_pos,
            SESS()->studied, SESS()->again, SESS()->good_or_better);
        if (state_err != ESP_OK) {
            ESP_LOGW(TAG, "resume state write failed (%d)", state_err);
        }
    }
    session_next();
}

// ---------------------------------------------------------------- 绘制

static void draw_deck_row(uint8_t* fb, int index, const anki_deck_summary_t* deck) {
    EpdRect row = row_rect(index);
    ui_draw_round_rect(fb, row, UI_BTN_RADIUS, UI_GRAY_WHITE);
    char stats[64];
    char short_stats[64];
    char resume[32];
    snprintf(stats, sizeof(stats), "新 %d · 复习 %d · 共 %d", deck->fresh, deck->due,
             deck->total);
    snprintf(short_stats, sizeof(short_stats), "新 %d · 复习 %d", deck->fresh, deck->due);
    snprintf(resume, sizeof(resume), "续 %d/%d", deck->resume_pos, deck->resume_count);
    // PRD §4.2 自省略：文字起点 x=60，按钮带左界 532 → 净宽 472 px。
    // 用 ui_text_fixed_width_px() 按实际系统字号实测，不缩字号、不新增第三行。
    // Auto-elide by measured width at the effective system size; no shrink,
    // no third line (PRD §4.2).
    const int avail = 472;
    const int px = ui_text_effective_px(UI_PX_CAPTION);
    char line[160];
    char candidate[160];
    if (deck->resume_count <= 0) {
        snprintf(line, sizeof(line), "%s", stats);
    } else {
        snprintf(candidate, sizeof(candidate), "%s   %s", stats, resume);
        if (ui_text_fixed_width_px(px, candidate) <= avail) {
            snprintf(line, sizeof(line), "%s", candidate);
        } else {
            // 由低到高省略：先去「· 共 Z」，再把「续 N/M」缩为单字「续」。
            snprintf(candidate, sizeof(candidate), "%s   %s", short_stats, resume);
            if (ui_text_fixed_width_px(px, candidate) <= avail) {
                snprintf(line, sizeof(line), "%s", candidate);
            } else {
                // 卡组名与「续」不得同时被省略；这是最低保底。
                snprintf(line, sizeof(line), "%s   续", short_stats);
            }
        }
    }
    ui_text(fb, row.x + 20, row.y + 10, UI_PX_LABEL, deck->name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, row.x + 20, row.y + 10 + UI_PX_LABEL + 6, UI_PX_CAPTION, line,
            EPD_DRAW_ALIGN_LEFT, false);
    // 导出 ↑ 与既有删除 ×；命中区间隙 8 px，不重叠。
    ui_draw_button(fb, row_export_rect(index), "↑", false);
    EpdRect del = row_delete_rect(index);
    ui_draw_button(fb, del, "×", false);
}

static void draw_file_row(uint8_t* fb, int index, const apkg_item_t* item) {
    EpdRect row = row_rect(index);
    ui_draw_round_rect(fb, row, UI_BTN_RADIUS, UI_GRAY_WHITE);
    char size[24];
    size_text(item->size, size);
    ui_text(fb, row.x + 20, row.y + 10, UI_PX_LABEL, item->name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, row.x + 20, row.y + 10 + UI_PX_LABEL + 6, UI_PX_CAPTION, size,
            EPD_DRAW_ALIGN_LEFT, false);
}

/// 二次确认提示带（U2：同视图中部，零新增视图）。文案点明日基准会一并恢复、
/// 操作不可撤销（R-1）。/ In-view confirmation band (U2); states that the day
/// base is restored too and that the action is irreversible (R-1).
static void draw_confirm_band(uint8_t* fb) {
    EpdRect band = {.x = UI_MARGIN,
                    .y = UI_CONTENT_TOP + 260,
                    .width = ui_content_width(),
                    .height = 132};
    ui_draw_round_rect(fb, band, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_text(fb, band.x + 16, band.y + 14, UI_PX_CAPTION,
            "将用备份覆盖该卡组学习进度与日基准，", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, band.x + 16, band.y + 14 + UI_PX_CAPTION + 8, UI_PX_CAPTION,
            "此操作不可撤销；卡片正文与模板不变。", EPD_DRAW_ALIGN_LEFT, false);
    if (s_lists && s_confirm_index >= 0 && s_confirm_index < s_lists->file_count) {
        ui_text(fb, band.x + 16, band.y + 14 + 2 * (UI_PX_CAPTION + 8), UI_PX_CAPTION,
                base_name(s_lists->files[s_confirm_index].path), EPD_DRAW_ALIGN_LEFT, false);
    }
}

// 列表分页与书架/文件页同款：绘制、命中、目标共用一套坐标。
// The list pager matches the shelf/file pages: draw, hit and target share one origin.
#define ANKI_PAGER_TOP (UI_CONTENT_BOTTOM - 56)

static void draw_pager(uint8_t* fb, int page, int pages) {
    char label[48];
    snprintf(label, sizeof(label), "第 %d 页 · 共 %d 页", page + 1, pages);
    ui_pager_draw_at(fb, label, page, pages, false, UI_PAGER_NONE, ANKI_PAGER_TOP);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    ui_clear_page(fb);
    if (!ttf_font_ready()) {
        ui_draw_no_font_page(fb, true, false);
        s_no_font = true;
        return;
    }
    s_no_font = false;
    char subtitle[96];

    switch (s_view) {
        case ANKI_VIEW_DECKS:
        case ANKI_VIEW_FILES: {
            // 解锁后的第一次重绘补齐锁屏前跳过的重扫：此时整库已经还给堆，
            // 重新读卡组文件不会和锁屏路径抢内存。
            // The first repaint after unlocking catches up on the rescan that
            // was skipped before locking: the deck is back on the heap by now,
            // so re-reading deck files cannot compete with the lock path.
            if (s_rescan_pending) {
                s_rescan_pending = false;
                if (s_view == ANKI_VIEW_DECKS) scan_decks();
            }
            bool decks = s_view == ANKI_VIEW_DECKS;
            int total = decks ? (lists_ready() ? s_lists->deck_count : 0)
                              : (lists_ready() ? s_lists->file_count : 0);
            const char* base_subtitle =
                decks ? "选择卡组开始学习"
                      : (s_file_mode == 1 ? "选择进度备份恢复 · 会覆盖进度，不可撤销"
                                          : "选择 .apkg 导入");
            // 操作结果提示直接落在副标题：DECKS/FILES 视图没有独立的提示带。
            // Surface operation feedback in the subtitle; no separate band here.
            snprintf(subtitle, sizeof(subtitle), "%s",
                     s_message[0] ? s_message : base_subtitle);
            ui_draw_header(fb, decks ? ANKI_TITLE
                                    : (s_file_mode == 1 ? "选择进度备份恢复"
                                                        : "导入 Anki 卡组"),
                           subtitle);
            int rows = rows_per_page();
            int first = s_page * rows;
            int last = first + rows;
            if (last > total) last = total;
            for (int i = first; i < last; ++i) {
                if (decks) draw_deck_row(fb, i, &s_lists->decks[i]);
                else draw_file_row(fb, i, &s_lists->files[i]);
            }
            if (total == 0) {
                const char* hint =
                    decks ? "还没有卡组，先导入 .apkg 文件"
                          : (s_file_mode == 1 ? "未找到 .akb 备份文件（两根均已扫描）"
                                              : "TF 卡上未找到 .apkg 文件");
                ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40, UI_PX_BODY, hint,
                        EPD_DRAW_ALIGN_LEFT, false);
            }
            draw_pager(fb, s_page, page_count(total));
            // 底栏：主操作按钮 + 菜单把手。/ Bar: primary action plus menu handle.
            if (decks) {
                ui_draw_button(fb, bar_button_rect(), "导入 Import", false);
                ui_draw_button(fb, bar_restore_rect(), "恢复", false);
            } else if (s_file_mode == 1 && s_confirm_index >= 0) {
                ui_draw_button(fb, bar_button_rect(), "确认恢复", false);
                ui_draw_button(fb, bar_restore_rect(), "取消", false);
                draw_confirm_band(fb);
            } else {
                ui_draw_button(fb, bar_button_rect(), "返回 Back", false);
            }
            ui_draw_menu_handle(fb, false);
            break;
        }
        case ANKI_VIEW_BUSY: {
            ui_draw_header(fb, ANKI_TITLE, "请稍候");
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 60, UI_PX_BODY,
                    s_message[0] ? s_message : "处理中…", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_menu_handle(fb, false);
            break;
        }
        case ANKI_VIEW_STUDY: {
            char progress[48];
            snprintf(progress, sizeof(progress), "%d/%d", SESS()->session_pos + 1,
                     SESS()->session_count);
            ui_draw_header(fb, SESS()->deck.name, progress);
            // 分页与绘制共用一套累计高度：页数、每页首行与行高来自同一次排版。
            // Paging and drawing share one accumulated height: the page
            // count, each page's first line and the line heights all come
            // from the same layout.
            int first = 0, count = 0, pages = 1;
            anki_layout_page_map(&SESS()->layout, SESS()->layout.line_page, &first, &count,
                                     &pages);
            if (SESS()->layout.line_page >= pages) {
                SESS()->layout.line_page = pages > 0 ? pages - 1 : 0;
                anki_layout_page_map(&SESS()->layout, SESS()->layout.line_page, &first, &count,
                                     &pages);
            }
            int y = UI_CONTENT_TOP + 8;
            char line[ANKI_LINE_BUF];
            // 记住进页时的字重；粗体行临时切 700，绘制后立刻恢复，最后再
            // 兜底恢复一次，绝不把字重泄漏到评分按钮或其它 app。
            // Remember the weight on entry; bold lines switch to 700 and
            // restore right after drawing, with one final restore so the
            // grade buttons and other apps are never affected.
            const int saved_weight = ttf_get_weight();
            for (int k = 0; k < count; ++k) {
                const int i = first + k;
                size_t n = SESS()->layout.line_len[i];
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, SESS()->layout.text + SESS()->layout.line_off[i], n);
                line[n] = 0;
                // 带 \x01 标记的行（正确选项与答案）用黑色加粗；其余行常规
                // 黑字。所有行都传 inverted=false，反色绘制已完全移除——
                // 墨水屏看不到彩色，白字反色反而难读。行首缩进来自排版
                // 结果，选项续行悬挂在前缀宽度下。
                // Lines carrying the leading \x01 marker (correct options
                // and the answer) draw black bold; every other line draws
                // ordinary black. inverted=false everywhere: inverted
                // drawing is gone, since e-paper has no colour and
                // white-on-black is harder to read. The indent comes from
                // the layout, so option continuations hang under their
                // prefix.
                if (n > 0) {
                    const bool bold = SESS()->layout.line_bold[i];
                    const int x = UI_MARGIN + SESS()->layout.line_x[i];
                    const int px = SESS()->layout.text_px;
                    if (bold) ttf_set_weight(ANKI_BOLD_WGHT);
                    ui_text_fixed(fb, x, y, px, line, EPD_DRAW_ALIGN_LEFT, false);
                    if (bold && ui_text_uses_builtin(line)) {
                        // 内建位图字体没有 wght 轴：水平 1px 双描边模拟加粗。
                        // 只叠加黑字（ui_text_fixed 不写背景），advance 不变，
                        // 与排版层 seg_measure_px() 的 +1 宽度口径一致。
                        // The built-in bitmap face has no wght axis: a 1px
                        // horizontal double strike fakes bold. Only black ink
                        // is overlaid (ui_text_fixed never paints a
                        // background), the advance is unchanged, matching
                        // the +1 width budget in seg_measure_px().
                        ui_text_fixed(fb, x + 1, y, px, line, EPD_DRAW_ALIGN_LEFT, false);
                    }
                    if (bold) ttf_set_weight(saved_weight);
                }
                y += SESS()->layout.line_height[i];
            }
            ttf_set_weight(saved_weight);
            if (SESS()->layout.line_count == 0) {
                ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 8, UI_PX_BODY, "[空卡片]",
                        EPD_DRAW_ALIGN_LEFT, false);
            }
            if (!SESS()->show_back) {
                ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - UI_PX_CAPTION - 6, UI_PX_CAPTION,
                        pages > 1 ? "上下滑翻页 · 轻点或中键翻面" : "轻点或按中键翻面",
                        EPD_DRAW_ALIGN_LEFT, false);
            } else {
                // 评分按钮。/ Grade buttons.
                static const char* labels[4] = {"忘记", "困难", "良好", "简单"};
                int gap = 8;
                int w = (ui_content_width() - 3 * gap) / 4;
                for (int i = 0; i < 4; ++i) {
                    EpdRect rect = {.x = UI_MARGIN + i * (w + gap),
                                    .y = UI_CONTENT_BOTTOM - UI_BTN_H,
                                    .width = w,
                                    .height = UI_BTN_H};
                    ui_draw_button(fb, rect, labels[i], false);
                }
            }
            ui_draw_menu_handle(fb, false);
            break;
        }
        case ANKI_VIEW_SUMMARY: {
            char line[96];
            ui_draw_header(fb, ANKI_TITLE, "本组完成");
            snprintf(line, sizeof(line), "已学习 %d 张", SESS()->studied);
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40, UI_PX_BODY, line,
                    EPD_DRAW_ALIGN_LEFT, false);
            snprintf(line, sizeof(line), "记住 %d · 忘记 %d", SESS()->good_or_better,
                     SESS()->again);
            const int summary_gap = anki_layout_pitch(ui_text_effective_px(UI_PX_BODY), "");
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40 + summary_gap, UI_PX_BODY, line,
                    EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, bar_button_rect(), "返回卡组 Back", false);
            ui_draw_menu_handle(fb, false);
            break;
        }
    }
}

// ---------------------------------------------------------------- 命中

static int decks_hit(uint16_t x, uint16_t y) {
    if (!lists_ready()) return -1;
    int rows = rows_per_page();
    int first = s_page * rows;
    int total = s_view == ANKI_VIEW_DECKS ? s_lists->deck_count : s_lists->file_count;
    int last = first + rows;
    if (last > total) last = total;
    for (int i = first; i < last; ++i) {
        if (ui_rect_hit(row_rect(i), x, y)) return i;
    }
    return -1;
}

static bool bar_button_hit(uint16_t x, uint16_t y) {
    return ui_rect_hit(bar_button_rect(), x, y);
}

// ---------------------------------------------------------------- 事件

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_page = 0;
    message_set(NULL);
    // 防御：带会话状态重进本页时先卸载旧会话。/ Defensive: drop a live
    // session when re-entering the page.
    session_release();
    s_confirm_index = -1;
    s_file_mode = 0;
    // 先自愈残留 tmp/bak 与孤儿 .aks，再枚举卡组：列表必须看到提交后的完整文件。
    // Recover stranded tmp/bak and orphan .aks before enumerating decks.
    anki_progress_recover();
    // 文件页带了 .apkg 进来：保持忙碌态，等下一 tick 导入，别被卡组列表盖掉。
    // / Arriving from the file page with an .apkg: stay busy and import on the
    // next tick instead of painting the deck list over it.
    if (s_pending_import) {
        s_view = ANKI_VIEW_BUSY;
        snprintf(s_message, sizeof(s_message), "正在导入 %.75s …", base_name(s_pending_path));
        return;
    }
    s_view = ANKI_VIEW_DECKS;
    scan_decks();
}

/* 注意：不能命名为 on_exit —— newlib 的 stdlib.h 已声明同名标准函数，会触发
   "conflicting types"（-Werror 下编不过）。此处统一加 anki_ 前缀。 */
static void anki_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    if (SESS()->active) {
        anki_store_unload(&SESS()->deck);
        SESS()->active = false;
        anki_heap_log("anki exit");
    }
}

/// 锁屏前释放学习会话：电源键锁屏走的是 on_before_lock，它比 on_exit 更早，
/// 而且此时页面不会切走——所以必须在锁图绘制、帧缓存写入、深睡交接之前把
/// 整库卸掉，把 PSRAM 让给锁屏路径。这里刻意不调用 scan_decks()：那会重新
/// 把每个 .akd 整份读进内存（千卡卡组 600KB），正好抵消刚让出来的空间。
/// Release the study session before locking: a power-key lock goes through
/// on_before_lock, which runs earlier than on_exit and never leaves the page,
/// so the whole deck must go before the lock art, frame cache write and deep
/// sleep handoff need that PSRAM. It deliberately skips scan_decks(): rescanning
/// reads every .akd back into memory (600KB for a thousand-card deck) and would
/// hand back exactly the room just freed.
static void anki_before_lock(app_ctx_t* ctx) {
    (void)ctx;
    anki_heap_log("anki lock enter");
    bool had_session = s_session != NULL && s_session->active;
    session_release();
    if (had_session) s_rescan_pending = true;
    anki_heap_log("anki lock released");
}

static void import_request(const char* path) {
    if (!path || !*path) return;
    snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
    s_pending_import = true;
    s_file_mode = 0;
    s_confirm_index = -1;
    // name 最长 127，s_message 只有 96，限宽到 75 保证中文前缀 + 省略号也放得下。
    // / Bound the field so the CJK prefix and ellipsis always fit in s_message.
    snprintf(s_message, sizeof(s_message), "正在导入 %.75s …", base_name(path));
    s_view = ANKI_VIEW_BUSY;
    // 先绘忙碌态，下一 tick 再执行导入。/ Paint the busy state first; import
    // runs on the next tick.
}

static void import_selected(int index) {
    if (!lists_ready() || index < 0 || index >= s_lists->file_count) return;
    import_request(s_lists->files[index].path);
}

static app_redraw_t run_import(app_ctx_t* ctx) {
    int imported = 0;
    // 先记下源文件是否在 TF 卡上：失败时才能把「中途拔卡」和「文件损坏」
    // 分开说，而不是笼统一句失败。/ Remember whether the source lives on the
    // TF card so a failure can tell "card pulled mid-import" apart from
    // "file corrupt" instead of one generic message.
    bool from_tf = strncmp(s_pending_path, "/sdcard", 7) == 0;
    esp_err_t err = anki_store_import(s_pending_path, &imported);
    s_pending_import = false;
    s_pending_path[0] = 0;
    if (err == ESP_OK && imported > 0) {
        snprintf(s_message, sizeof(s_message), "导入完成：%d 个卡组", imported);
        s_view = ANKI_VIEW_DECKS;
        scan_decks();
    } else {
        // 失败时留在忙碌态：只有 BUSY 视图绘制 s_message，切回卡组列表会把
        // 失败原因吞掉，用户只见列表“无反应”。按返回键（见 on_key）退出。
        // / Stay busy on failure: only the BUSY view paints s_message, and
        // switching back to the deck list would swallow the reason, leaving
        // the user with a list that appears dead. The back key (see on_key)
        // exits this state.
        read_pico_sd_info_t info = {0};
        read_pico_sd_get_info(&info);
        message_set(from_tf && !info.mounted ? "TF 卡已拔出，导入已中断"
                                             : "导入失败，文件可能损坏");
        s_view = ANKI_VIEW_BUSY;
    }
    render(ctx, ctx->fb);
    guard_draw_result(ctx->hl, update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE));
    return APP_REDRAW_DONE;
}

/// 所有点击交互的统一入口（由 on_gesture 的 UI_GESTURE_TAP 调用）。
/// 注册了 on_gesture 的 app，主循环不再调用 on_touch——点击必须经 TAP
/// 处理（参考 app_image.c 的 on_gesture）。TAP 语义即「一次点击一次触发」，
/// 本函数不依赖 ctx->pressed/released 或触点数量。
/// / Single entry for all tap interactions (invoked from on_gesture's
/// UI_GESTURE_TAP). Once an app registers on_gesture the main loop never
/// calls on_touch — taps must be handled via TAP (see app_image.c's
/// on_gesture). TAP semantics are one tap, one trigger; this function
/// depends on neither ctx->pressed/released nor the touch-point count.
static app_redraw_t handle_tap(uint16_t x, uint16_t y, app_ctx_t* ctx) {
    if (s_no_font) {
        switch (ui_no_font_hit_test(x, y)) {
            case UI_NO_FONT_HIT_REMOUNT:
            case UI_NO_FONT_HIT_FORMAT:
                return APP_REDRAW_NONE;
            default:
                return APP_REDRAW_NONE;
        }
    }
    switch (s_view) {
        case ANKI_VIEW_DECKS:
        case ANKI_VIEW_FILES: {
            if (!lists_ready()) return APP_REDRAW_NONE;
            int total = s_view == ANKI_VIEW_DECKS ? s_lists->deck_count : s_lists->file_count;
            // 二次确认态是模态：只允许确认/取消，其余命中一律忽略，防误触列表。
            // Confirmation is modal: only confirm/cancel are live.
            if (s_view == ANKI_VIEW_FILES && s_file_mode == 1 && s_confirm_index >= 0) {
                if (ui_rect_hit(bar_button_rect(), x, y)) {
                    anki_progress_result_t result = {0};
                    esp_err_t err = anki_progress_restore(
                        s_lists->files[s_confirm_index].path, &result);
                    restore_message(err, &result);
                    s_confirm_index = -1;
                    s_file_mode = 0;
                    s_view = ANKI_VIEW_DECKS;
                    s_page = 0;
                    scan_decks();
                    return APP_REDRAW_FULL;
                }
                if (ui_rect_hit(bar_restore_rect(), x, y)) {
                    s_confirm_index = -1;  // 取消不产生任何写盘
                    message_set("已取消恢复");
                    return APP_REDRAW_PAGE;
                }
                return APP_REDRAW_NONE;
            }
            int pager = ui_pager_hit_at(x, y, ANKI_PAGER_TOP);
            if (pager >= 0) {
                int target = ui_pager_target(pager, s_page, page_count(total));
                if (target != s_page) {
                    s_page = target;
                    return APP_REDRAW_PAGE;
                }
                return APP_REDRAW_NONE;
            }
            if (bar_button_hit(x, y)) {
                if (s_view == ANKI_VIEW_DECKS) {
                    read_pico_sd_info_t info = {0};
                    read_pico_sd_get_info(&info);
                    if (!info.mounted) {
                        message_set("请先插入 TF 卡");
                        render(ctx, ctx->fb);
                        guard_draw_result(ctx->hl,
                                          update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE));
                        return APP_REDRAW_DONE;
                    }
                    s_file_mode = 0;
                    s_confirm_index = -1;
                    s_view = ANKI_VIEW_FILES;
                    s_page = 0;
                    scan_apkg();
                } else {
                    s_file_mode = 0;
                    s_confirm_index = -1;
                    s_view = ANKI_VIEW_DECKS;
                    s_page = 0;
                    scan_decks();
                }
                return APP_REDRAW_PAGE;
            }
            // 底部「恢复」：Q3 修正，不实现「无 TF 卡禁用」分支。
            if (s_view == ANKI_VIEW_DECKS && ui_rect_hit(bar_restore_rect(), x, y)) {
                s_file_mode = 1;
                s_confirm_index = -1;
                s_view = ANKI_VIEW_FILES;
                s_page = 0;
                scan_backups();
                return APP_REDRAW_PAGE;
            }
            int row = decks_hit(x, y);
            if (row < 0) return APP_REDRAW_NONE;
            if (s_view == ANKI_VIEW_DECKS) {
                // 行内删除钮。/ In-row delete button.
                if (ui_rect_hit(row_delete_rect(row), x, y)) {
                    if (anki_store_delete(s_lists->decks[row].deck_id) == ESP_OK) {
                        message_set("已删除");
                    } else {
                        message_set("删除失败");
                    }
                    scan_decks();
                    return APP_REDRAW_PAGE;
                }
                // 行内 ↑：导出到活动根，成功提示最终路径。
                if (ui_rect_hit(row_export_rect(row), x, y)) {
                    char out_path[288] = {0};
                    esp_err_t err = anki_progress_export(s_lists->decks[row].deck_id,
                                                         out_path, sizeof(out_path));
                    if (err == ESP_OK) {
                        snprintf(s_message, sizeof(s_message), "已导出 %.60s", out_path);
                    } else {
                        message_set("导出失败，设备进度未改变");
                    }
                    return APP_REDRAW_PAGE;
                }
                session_start(row);
                return APP_REDRAW_FULL;
            }
            if (s_file_mode == 1) {
                s_confirm_index = row;  // 进入同视图二次确认
                return APP_REDRAW_PAGE;
            }
            import_selected(row);
            return APP_REDRAW_PAGE;
        }
        case ANKI_VIEW_STUDY: {
            if (SESS()->show_back) {
                int gap = 8;
                int w = (ui_content_width() - 3 * gap) / 4;
                for (int i = 0; i < 4; ++i) {
                    EpdRect rect = {.x = UI_MARGIN + i * (w + gap),
                                    .y = UI_CONTENT_BOTTOM - UI_BTN_H,
                                    .width = w,
                                    .height = UI_BTN_H};
                    if (ui_rect_hit(rect, x, y)) {
                        session_grade((anki_grade_t)i);
                        return APP_REDRAW_PAGE;
                    }
                }
                return APP_REDRAW_NONE;
            }
            // 正面：轻点翻面。/ Front: tap flips.
            session_show_back();
            return APP_REDRAW_PAGE;
        }
        case ANKI_VIEW_SUMMARY: {
            if (bar_button_hit(x, y)) {
                session_end();
                return APP_REDRAW_FULL;
            }
            return APP_REDRAW_NONE;
        }
        case ANKI_VIEW_BUSY:
        default:
            return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    switch (key) {
        case UI_KEY_3:
            // 菜单出口。/ Menu exit.
            ctx->request_menu = true;
            return APP_REDRAW_NONE;
        case UI_KEY_1:
            // 返回。/ Back.
            switch (s_view) {
                case ANKI_VIEW_BUSY:
                    // 导入失败提示停在忙碌态；导入已结束（无待办）时放行回
                    // 卡组列表，导入进行中则不可打断。
                    // / A failed-import hint parks on the busy view; once the
                    // import has finished (nothing pending) release back to
                    // the deck list, and never interrupt a running import.
                    if (!s_pending_import) {
                        s_view = ANKI_VIEW_DECKS;
                        s_page = 0;
                        scan_decks();
                        return APP_REDRAW_PAGE;
                    }
                    return APP_REDRAW_NONE;
                case ANKI_VIEW_FILES:
                    if (s_file_mode == 1 && s_confirm_index >= 0) {
                        s_confirm_index = -1;  // 返回键先取消确认
                        return APP_REDRAW_PAGE;
                    }
                    s_file_mode = 0;
                    s_confirm_index = -1;
                    s_view = ANKI_VIEW_DECKS;
                    s_page = 0;
                    scan_decks();
                    return APP_REDRAW_PAGE;
                case ANKI_VIEW_STUDY:
                case ANKI_VIEW_SUMMARY:
                    session_end();
                    return APP_REDRAW_FULL;
                case ANKI_VIEW_DECKS:
                    ctx->request_return = true;
                    return APP_REDRAW_NONE;
                default:
                    return APP_REDRAW_NONE;
            }
        case UI_KEY_2:
            // 中键：学习页翻面，其余回到列表。/ Middle: flip in study, else decks.
            if (s_view == ANKI_VIEW_STUDY && !SESS()->show_back) {
                session_show_back();
                return APP_REDRAW_PAGE;
            }
            if (s_view == ANKI_VIEW_SUMMARY) {
                session_end();
                return APP_REDRAW_FULL;
            }
            return APP_REDRAW_NONE;
        default:
            return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key == UI_KEY_2 && (s_view == ANKI_VIEW_STUDY || s_view == ANKI_VIEW_SUMMARY)) {
        // 长按中键结束本组。/ Long-press middle ends the session.
        session_end();
        return APP_REDRAW_FULL;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == ANKI_VIEW_BUSY && s_pending_import) return run_import(ctx);
    return APP_REDRAW_NONE;
}

/// 注册 on_gesture 后主循环不再调用 on_touch，全部触摸走手势通道：
/// 点击（UI_GESTURE_TAP）交给 handle_tap，上下滑仅在学习页用于卡片内
/// 翻页，其余手势忽略。此前点击逻辑留在 on_touch 里从未被调用，导致
/// Anki 页触摸全无反应——勿再把点击写回 on_touch。
/// / Once on_gesture is registered the main loop never calls on_touch and
/// every touch travels through the gesture channel: taps
/// (UI_GESTURE_TAP) go to handle_tap, vertical swipes page within a card
/// on the study view only, and other gestures are ignored. Tap logic used
/// to live in on_touch where it was never invoked, leaving the whole Anki
/// page touch-dead — never move taps back into on_touch.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* event) {
    switch (event->type) {
        case UI_GESTURE_TAP:
            // TAP 的逻辑坐标是按下原点。/ A tap's logical position is its origin.
            return handle_tap(event->x0, event->y0, ctx);
        case UI_GESTURE_SWIPE_U:
        case UI_GESTURE_SWIPE_D: {
            if (s_view != ANKI_VIEW_STUDY) return APP_REDRAW_NONE;
            int first = 0, count = 0, pages = 1;
            anki_layout_page_map(&SESS()->layout, SESS()->layout.line_page, &first, &count,
                                     &pages);
            if (pages <= 1) return APP_REDRAW_NONE;
            if (event->type == UI_GESTURE_SWIPE_U) {
                if (SESS()->layout.line_page + 1 < pages) ++SESS()->layout.line_page;
                return APP_REDRAW_PAGE;
            }
            if (SESS()->layout.line_page > 0) --SESS()->layout.line_page;
            return APP_REDRAW_PAGE;
        }
        case UI_GESTURE_PRESS:
        case UI_GESTURE_MOVE:
        case UI_GESTURE_LONG_PRESS:
        case UI_GESTURE_SWIPE_L:
        case UI_GESTURE_SWIPE_R:
        case UI_GESTURE_CANCEL:
        default:
            return APP_REDRAW_NONE;
    }
}

const app_desc_t app_anki = {
    .title = ANKI_TITLE,
    .detail = ANKI_DETAIL,
    .enter_full = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = anki_on_exit,
    // 电源键锁屏：先把整库还给堆，再让锁屏路径去画图与交接深睡。
    // Power-key lock: hand the deck back to the heap before the lock path
    // draws its art and hands off to deep sleep.
    .on_before_lock = anki_before_lock,
    .on_gesture = on_gesture,
    // 注意：注册了 on_gesture 后主循环不再调用 on_touch（全部触摸走手势
    // 通道），故不上报 .on_touch；点击统一经 on_gesture 的 TAP 分支处理。
    // / Note: with on_gesture registered the main loop never calls on_touch
    // (all touch flows through the gesture channel), so .on_touch stays
    // unset; taps are handled by on_gesture's TAP branch exclusively.
    .on_key = on_key,
    .on_key_long = on_key_long,
    .on_tick = on_tick,
    .owns_keys = true,
};

/// 文件页入口：记录路径并让主循环切到本页，进页后即导入该 .apkg。
/// / File-page entry: stash the path so the loop switches here and the page
/// imports that .apkg right after entering.
bool app_anki_request_open(const char* path) {
    if (!path || !*path) return false;
    import_request(path);
    return true;
}
