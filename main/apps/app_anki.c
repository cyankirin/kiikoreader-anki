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
#include "anki_render.h"
#include "anki_store.h"
#include "app_font_context.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_anki"

#define ANKI_TITLE "Anki 卡组"
#define ANKI_DETAIL "Anki 卡片学习 Cards"

#define ROW_H 92
#define LIST_MAX 96
#define LINE_MAX 220
#define LINE_BUF 256
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
    const char* text;      // 当前显示的文本 / text on screen
    size_t line_off[LINE_MAX];
    size_t line_len[LINE_MAX];
    int line_count;
    int line_page;
    int studied;
    int again;
    int good_or_better;
} anki_session_t;

static anki_lists_t* s_lists;
static anki_view_t s_view = ANKI_VIEW_DECKS;
static int s_page;                 // 列表分页 / list page
static bool s_no_font;
static char s_message[96];
// 会话状态（含两块 16KB 卡面缓冲）常驻 PSRAM，避免占内部 RAM。
// Session state (with two 16KB card buffers) stays in PSRAM, off internal RAM.
static anki_session_t* s_session;

// 学习卡片正文属于阅读内容：进学习视图切到阅读字体，离开学习视图的所有
// 路径（结束/翻到小结/退出 app）恢复原上下文。s_font_saved_valid 表示
// s_font_saved 持有未恢复的保存值。/ A card body is reading content: the
// study view switches to the reading face, and every path leaving the study
// view (session end, summary, app exit) restores the previous context.
// s_font_saved_valid marks s_font_saved as holding an unrestored save.
static app_lock_font_t s_font_saved;
static bool s_font_saved_valid;

/// 离开学习视图时恢复字体上下文（幂等）。/ Restore the font context when
/// leaving the study view (idempotent).
static void study_font_restore(void) {
    if (!s_font_saved_valid) return;
    app_font_end_lock(&s_font_saved);
    s_font_saved_valid = false;
}

static anki_session_t* session_state(void) {
    if (!s_session) {
        s_session = heap_caps_calloc(1, sizeof(*s_session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_session;
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

// ---------------------------------------------------------------- 会话

static void wrap_card_text(void) {
    const char* text = SESS()->text ? SESS()->text : "";
    SESS()->line_count = 0;
    SESS()->line_page = 0;
    int px = UI_PX_BODY;
    int width = ui_content_width();
    size_t i = 0;
    size_t len = strlen(text);
    while (i < len && SESS()->line_count < LINE_MAX) {
        // 先按换行切段。/ Split on explicit newlines first.
        size_t end = i;
        while (end < len && text[end] != '\n') ++end;
        size_t seg_len = end - i;
        size_t start = i;
        while (seg_len > 0 && SESS()->line_count < LINE_MAX) {
            // 贪心折行：能放就放，放不下从最后一个空格断。/ Greedy wrap: fit what
            // fits, break at the last space when a word overflows.
            size_t take = seg_len;
            while (take > 1) {
                char probe[LINE_BUF];
                size_t n = take;
                if (n >= sizeof(probe)) n = sizeof(probe) - 1;
                memcpy(probe, text + start, n);
                probe[n] = 0;
                if (ui_text_fixed_width_px(px, probe) <= width) break;
                // 二进制+线性混合：先减半再逐字回退，避免超长行反复全量测量。
                // Halve then step back by characters, so long lines do not
                // remeasure the whole span each time.
                take = take > 16 ? take - take / 4 : take - 1;
                if (take < 1) take = 1;
            }
            // 优先在空格处断。/ Prefer breaking at a space.
            size_t cut = take;
            for (size_t k = cut; k > 1; --k) {
                if (text[start + k - 1] == ' ') {
                    cut = k;
                    break;
                }
                if (k + 8 < cut) break; // 只回看一小段 / only look back a little
            }
            SESS()->line_off[SESS()->line_count] = start;
            SESS()->line_len[SESS()->line_count] = cut;
            ++SESS()->line_count;
            start += cut;
            while (start < end && text[start] == ' ') ++start; // 吞掉断行空格 / drop the break space
            seg_len = end - start;
        }
        i = end + 1;
    }
}

static int lines_per_page(void) {
    int top = UI_CONTENT_TOP + 8;
    int bottom = UI_CONTENT_BOTTOM - 8;
    return (bottom - top) / (UI_PX_BODY + 10);
}

static void session_load_back(void);

static void session_open_card(void) {
    SESS()->show_back = false;
    SESS()->back_loaded = false;
    SESS()->text = SESS()->front;
    wrap_card_text();
}

static void session_show_back(void) {
    SESS()->show_back = true;
    if (!SESS()->back_loaded) session_load_back();
    SESS()->text = SESS()->back;
    wrap_card_text();
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
    if (SESS()->session_count == 0) {
        s_view = ANKI_VIEW_SUMMARY;
        return;
    }
    // 卡面正文用阅读字体；必须在渲染第一张卡之前切换。/ Card bodies use
    // the reading face; switch before rendering the first card.
    app_font_begin_card(&s_font_saved);
    s_font_saved_valid = true;
    session_load_front();
    session_open_card();
    s_view = ANKI_VIEW_STUDY;
}

static void session_end(void) {
    // 学习页回到卡组列表（系统字体界面），先恢复字体上下文。
    // / Back to the deck list (a system-face UI); restore the font context
    // first.
    study_font_restore();
    anki_store_unload(&SESS()->deck);
    SESS()->active = false;
    s_view = ANKI_VIEW_DECKS;
    scan_decks();
}

static void session_next(void) {
    ++SESS()->session_pos;
    if (SESS()->session_pos >= SESS()->session_count) {
        // 小结是系统字体界面。/ The summary is a system-face UI.
        study_font_restore();
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
            int per = lines_per_page();
            if (SESS()->line_page >= (SESS()->line_count + per - 1) / (per > 0 ? per : 1))
                SESS()->line_page = 0;
            int pages = SESS()->line_count > 0
                            ? 1 + (SESS()->line_count - 1) / (per > 0 ? per : 1)
                            : 1;
            if (SESS()->line_page >= pages) SESS()->line_page = pages - 1;
            int first = SESS()->line_page * per;
            int y = UI_CONTENT_TOP + 8;
            char line[LINE_BUF];
            for (int i = first; i < first + per && i < SESS()->line_count; ++i) {
                size_t n = SESS()->line_len[i];
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, SESS()->text + SESS()->line_off[i], n);
                line[n] = 0;
                ui_text(fb, UI_MARGIN, y, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
                y += UI_PX_BODY + 10;
            }
            if (SESS()->line_count == 0) {
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
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40 + UI_PX_BODY + 12, UI_PX_BODY, line,
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
    if (SESS()->active) {
        // 防御：带会话状态重进本页时一并恢复字体。/ Defensive: also
        // restore the face when re-entering with live session state.
        study_font_restore();
        anki_store_unload(&SESS()->deck);
        SESS()->active = false;
    }
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
    // 学习页直接退出到别的 app 也要恢复字体。/ Leaving the app mid-study
    // must restore the face too.
    study_font_restore();
    if (SESS()->active) {
        anki_store_unload(&SESS()->deck);
        SESS()->active = false;
    }
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
        message_set("导入失败，文件可能损坏");
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
            int per = lines_per_page();
            int pages =
                SESS()->line_count > 0 ? 1 + (SESS()->line_count - 1) / (per > 0 ? per : 1) : 1;
            if (pages <= 1) return APP_REDRAW_NONE;
            if (event->type == UI_GESTURE_SWIPE_U) {
                if (SESS()->line_page + 1 < pages) ++SESS()->line_page;
                return APP_REDRAW_PAGE;
            }
            if (SESS()->line_page > 0) --SESS()->line_page;
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
