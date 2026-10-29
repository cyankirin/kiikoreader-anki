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
    // 折行结果：行号、高度、缩进与反色标记都归排版单元管。
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
    SESS()->session_count =
        anki_store_session(&SESS()->deck, SESS()->session, ANKI_SESSION_MAX);
    SESS()->session_pos = 0;
    SESS()->studied = 0;
    SESS()->again = 0;
    SESS()->good_or_better = 0;
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
    session_next();
}

// ---------------------------------------------------------------- 绘制

static void draw_deck_row(uint8_t* fb, int index, const anki_deck_summary_t* deck) {
    EpdRect row = row_rect(index);
    ui_draw_round_rect(fb, row, UI_BTN_RADIUS, UI_GRAY_WHITE);
    char stats[64];
    snprintf(stats, sizeof(stats), "新 %d · 复习 %d · 共 %d", deck->fresh, deck->due, deck->total);
    ui_text(fb, row.x + 20, row.y + 10, UI_PX_LABEL, deck->name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, row.x + 20, row.y + 10 + UI_PX_LABEL + 6, UI_PX_CAPTION, stats,
            EPD_DRAW_ALIGN_LEFT, false);
    // 删除小钮。/ Delete button.
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
            snprintf(subtitle, sizeof(subtitle), "%s", decks ? "选择卡组开始学习" : "选择 .apkg 导入");
            ui_draw_header(fb, decks ? ANKI_TITLE : "导入 Anki 卡组", subtitle);
            int rows = rows_per_page();
            int first = s_page * rows;
            int last = first + rows;
            if (last > total) last = total;
            for (int i = first; i < last; ++i) {
                if (decks) draw_deck_row(fb, i, &s_lists->decks[i]);
                else draw_file_row(fb, i, &s_lists->files[i]);
            }
            if (total == 0) {
                const char* hint = decks ? "还没有卡组，先导入 .apkg 文件" : "TF 卡上未找到 .apkg 文件";
                ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40, UI_PX_BODY, hint,
                        EPD_DRAW_ALIGN_LEFT, false);
            }
            draw_pager(fb, s_page, page_count(total));
            // 底栏：主操作按钮 + 菜单把手。/ Bar: primary action plus menu handle.
            ui_draw_button(fb, bar_button_rect(), decks ? "导入 Import" : "返回 Back", false);
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
            for (int k = 0; k < count; ++k) {
                const int i = first + k;
                size_t n = SESS()->layout.line_len[i];
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, SESS()->layout.text + SESS()->layout.line_off[i], n);
                line[n] = 0;
                // 行首带 \x01 强调标记的行反色绘制（黑底白字），墨水屏上
                // 清晰醒目；其余行常规黑字。行首缩进来自排版结果，选项续行
                // 悬挂在前缀宽度下。/ Lines carrying the leading \x01 emphasis
                // marker draw inverted (white on black) for clarity on
                // e-paper; every other line draws ordinary black. The indent
                // comes from the layout, so option continuations hang under
                // their prefix.
                if (n > 0) {
                    ui_text_fixed(fb, UI_MARGIN + SESS()->layout.line_x[i], y, SESS()->layout.text_px, line,
                                  EPD_DRAW_ALIGN_LEFT, SESS()->layout.line_invert[i]);
                }
                y += SESS()->layout.line_height[i];
            }
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
                    s_view = ANKI_VIEW_FILES;
                    s_page = 0;
                    scan_apkg();
                } else {
                    s_view = ANKI_VIEW_DECKS;
                    s_page = 0;
                    scan_decks();
                }
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
                session_start(row);
                return APP_REDRAW_FULL;
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
