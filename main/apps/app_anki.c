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
// 卡片会被静默截断，提到 400 后常见长卡可完整显示；会话结构体走 PSRAM
// 分配，增量约 3KB（line_off/line_len 各 +1440B、line_invert +180B），
// 内部 RAM 不受影响。/ Per-card body line cap: overlong cards stop here so
// the wrap arrays cannot grow without bound. At 220 long cards were silently
// truncated; 400 shows common long cards in full. The session struct is
// PSRAM-allocated, so the ~3KB growth (line_off/line_len +1440B each,
// line_invert +180B) does not touch internal RAM.
#define LINE_MAX 400
// 折行探测与绘制行缓冲的容量，即单行字节上限：任何长度的无空格串都会
// 在宽度内被切短，测量值永不小于真实占用。/ Probe/line buffer capacity,
// i.e. the byte cap per line: any no-space run is cut to fit the width and
// measurement never undercounts the real span.
#define LINE_BUF 512
#define APKG_DIRS 2
// 自适应字号：正文超过这个页数就缩小字号，取第一个不超标的字号。
// Adaptive size: shrink the body type when it runs past this many pages and
// keep the first size that fits.
#define ANKI_PAGES_TARGET 3
// 自适应字号下限：再小就读不清了。/ Adaptive size floor: below this the text stops being readable.
#define ANKI_MIN_BODY_PX 16

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
    // 行首 \x01 强调标记（选择题背面错误选项行）：该行反色绘制。
    // Leading \x01 emphasis marker (choice-back wrong option lines): that
    // line is drawn inverted.
    bool line_invert[LINE_MAX];
    // 每行高度（像素）：文本行与空行同为行距，后续图片行为其缩放高度。
    // Height of each line in pixels: text and blank lines share the pitch, a
    // later image line carries its scaled height.
    uint16_t line_height[LINE_MAX];
    // 行首缩进：0 = 左边距；选项续行缩进到 "<字母>. " 前缀宽度。
    // Left indent of a line: 0 = the left margin; an option's continuation
    // lines indent to the "<letter>. " prefix width.
    uint8_t line_x[LINE_MAX];
    // 本次排版实际使用的字号（自适应后）。/ The size this layout settled on.
    int text_px;
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

// ---------------------------------------------------------------- 排版

// 正文排版与绘制同口径：ui_text() 内部会把每个字号过一次
// ui_text_effective_px()（+2 且按系统字号百分比缩放），折行测量与行距
// 必须复用该口径，否则量窄画宽、文本超屏。/ Body layout shares the draw
// metric: ui_text() passes every size through ui_text_effective_px()
// (+2 plus system-size scaling), so wrap measurement and line pitch must
// reuse it, or lines measure narrow, draw wide and overflow the screen.
static int body_px(void) {
    return ui_text_effective_px(UI_PX_BODY);
}

// 行距按字体的真实行盒算：上延伸 + 下延伸，字形都落在里面，相邻两行的
// 墨迹就不会叠上。两条字体分支都算取大者：内建字库缺字的行会回退到 TTF，
// 两个行盒都得兜住。内建补充字库（ui-hanzi.bin）的位图可能高出行盒，
// 再按正文实测补一刀。
// 不用硬编码的拉丁样本量行高：汉字的字形盒比拉丁高得多，用 "AA" 量会把
// 中文行距压短，相邻两行的墨迹就会粘在一起。
// The pitch follows the font's real line box (ascent + descent): every glyph
// lives inside it, so neighbouring lines cannot overlap. Both font branches
// are measured and the larger wins, because a line missing a glyph in the
// built-in face falls back to the TTF. The built-in supplementary bitmaps
// (ui-hanzi.bin) can exceed the line box, so the actual text is measured as
// a floor. A hardcoded Latin sample would under-measure Chinese lines.
static int body_pitch(int px, const char* text) {
    int box = ui_font_ascender_px(px) + ui_font_descender_px(px);
    const int ttf_box = ttf_ascender_px(px) + ttf_descender_px(px);
    if (ttf_box > box) box = ttf_box;
    int above = 0, below = 0;
    ui_font_measure_line_px(px, text, &above, &below);
    if (above + below > box) box = above + below;
    const int pitch = box + 2;
    return pitch > 0 ? pitch : px + 2;
}

// 一个 UTF-8 字符的字节长度；非法字节按 1 计。
// Byte length of one UTF-8 character; an invalid byte counts as one.
static size_t utf8_char_len(const char* s) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 解一个 UTF-8 字符；used 回报字节数。
// Decode one UTF-8 character; used reports its byte length.
static uint32_t utf8_decode(const char* s, size_t len, size_t* used) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *used = 1;
        return c;
    }
    const size_t n = utf8_char_len(s);
    if (n > len) {
        *used = 1;
        return c;
    }
    uint32_t cp = c & (0x7Fu >> n);
    for (size_t i = 1; i < n; ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            *used = 1;
            return c;
        }
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *used = n;
    return cp;
}

// 行首禁则：闭符号不起一行。/ Line-head rule: a closing mark never starts a line.
static bool cjk_closing(uint32_t cp) {
    switch (cp) {
        case 0x3001: case 0x3002:                 // 、。
        case 0x300B: case 0x300D: case 0x300F:   // 》」』
        case 0x3011: case 0x3015:                 // 】〕
        case 0xFF01: case 0xFF0C: case 0xFF0E:   // ！，．
        case 0xFF09: case 0xFF1A: case 0xFF1B: case 0xFF1F:  // ）：；？
        case 0xFF3D: case 0xFF5D:                 // ］｝
        case 0x2014: case 0x2019: case 0x201D:   // —’ ”
        case 0x2026: case 0x2030:                 // …‰
            return true;
        default: return false;
    }
}

// 行尾禁则：开符号不收一行。/ Line-tail rule: an opening mark never ends a line.
static bool cjk_opening(uint32_t cp) {
    switch (cp) {
        case 0x3008: case 0x300A:                 // 〈《
        case 0x300C: case 0x300E:                 // 「『
        case 0x3010: case 0x3014:                 // 【〔
        case 0xFF08: case 0xFF3B: case 0xFF5B:   // （［｛
        case 0x2018: case 0x201C:                 // ‘“
            return true;
        default: return false;
    }
}

// 段内某个字节位置往前找字符起点。/ Find the start of the character ending at an offset.
static size_t utf8_prev_start(const char* s, size_t at) {
    size_t p = at;
    while (p > 0 && ((unsigned char)s[p - 1] & 0xC0) == 0x80) --p;
    return p > 0 ? p - 1 : 0;
}

// 前 n 字节的显示宽度（用行内探测缓冲）。
// Display width of the first n bytes, measured with the line's probe buffer.
static int seg_width_px(const char* seg, size_t n, int px, char* probe, size_t cap) {
    if (n > cap - 1) n = cap - 1;
    memcpy(probe, seg, n);
    probe[n] = 0;
    return ui_text_fixed_width_px(px, probe);
}

// 切点避头尾：闭符号不落行首、开符号不落行尾。拉得动就把符号拉进本行，
// 拉不动就把本行末尾的字符推到下一行，让符号跟着内容走。
// Break-point rules: a closing mark never starts a line and an opening mark
// never ends one. A mark that fits moves onto this line; one that does not
// pushes the line's last character to the next line so the mark keeps company.
static size_t cjk_fix_break(const char* seg, size_t cut, size_t seg_len, int px, int avail,
                            char* probe, size_t cap) {
    const size_t original = cut;
    for (int guard = 0; guard < 16; ++guard) {
        // 行尾禁则 / line-tail rule
        if (cut > 0) {
            const size_t p = utf8_prev_start(seg, cut);
            size_t used = 0;
            const uint32_t cp = utf8_decode(seg + p, cut - p, &used);
            if (cjk_opening(cp)) {
                size_t next_len = 0;
                const bool has_next = cut < seg_len;
                if (has_next) utf8_decode(seg + cut, seg_len - cut, &next_len);
                if (has_next && cut + next_len < cap &&
                    seg_width_px(seg, cut + next_len, px, probe, cap) <= avail) {
                    cut += next_len;  // 下一个字符拉进本行 / pull the next character in
                    continue;
                }
                cut = p;  // 开符号挪到下一行 / move the opening mark down
                continue;
            }
        }
        // 行首禁则 / line-head rule
        if (cut < seg_len) {
            size_t used = 0;
            const uint32_t cp = utf8_decode(seg + cut, seg_len - cut, &used);
            if (used > 0 && cjk_closing(cp)) {
                if (cut + used < cap && seg_width_px(seg, cut + used, px, probe, cap) <= avail) {
                    cut += used;  // 闭符号拉回本行末端 / pull the closing mark back
                    continue;
                }
                if (cut > 0) {
                    cut = utf8_prev_start(seg, cut);  // 本行末尾字符推到下一行
                    continue;
                }
            }
        }
        break;
    }
    // 退化情形（整行只有一个字符）：保留原切点，宁可破一次禁则也不出空行。
    // Degenerate case (a line of one character): keep the original cut rather
    // than emit an empty line, even at the price of one broken rule.
    return cut > 0 ? cut : original;
}

// 折行与排版：把卡片文本拆成行，量宽度、避头尾、给选项续行悬挂缩进，
// 并记下每行的高度与缩进。pitch 由调用方按字号给（行盒随字号成正比）。
// Wrap and lay out: split the card text into lines, measure them, apply the
// CJK break rules, hang option continuations under their prefix, and record
// each line's height and indent. The caller supplies the pitch, which
// scales with the type size.
static void build_card_lines(int px, int pitch) {
    const char* text = SESS()->text ? SESS()->text : "";
    SESS()->line_count = 0;
    SESS()->line_page = 0;
    // 反色与缩进随折行全量赋值，这里再清零一遍，避免复用会话留下脏值。
    // The invert and indent flags are fully assigned during wrapping; zero
    // them here too so a reused session never keeps stale values.
    memset(SESS()->line_invert, 0, sizeof(SESS()->line_invert));
    memset(SESS()->line_x, 0, sizeof(SESS()->line_x));
    const int width = ui_content_width();
    // 探测缓冲一次分配复用；候选长度先钳到容量内，测量永远是完整候选的
    // 宽度，超长无空格串也会在宽度内被切短。/ One reusable probe buffer;
    // the candidate is clamped to its capacity first, so measurement always
    // covers the full candidate and long no-space runs still cut to width.
    static char probe[LINE_BUF];
    const size_t max_take = sizeof(probe) - 1;
    const size_t len = strlen(text);
    size_t i = 0;
    while (i < len && SESS()->line_count < LINE_MAX) {
        // 先按换行切段。/ Split on explicit newlines first.
        size_t end = i;
        while (end < len && text[end] != '\n') ++end;
        size_t seg_len = end - i;
        size_t start = i;
        // 空段就是一行空行：选择题背面靠它把题干、选项与答案分层。
        // An empty segment is a blank line: the choice back uses it to layer
        // the question, the options and the answer.
        if (seg_len == 0) {
            SESS()->line_off[SESS()->line_count] = start;
            SESS()->line_len[SESS()->line_count] = 0;
            SESS()->line_invert[SESS()->line_count] = false;
            SESS()->line_height[SESS()->line_count] = (uint16_t)pitch;
            SESS()->line_x[SESS()->line_count] = 0;
            ++SESS()->line_count;
            i = end + 1;
            continue;
        }
        // 选项行首模式：可选 \x01 + 单个 ASCII 字母 + ". "；续行按前缀宽度悬挂。
        // Option line head: an optional \x01 then one ASCII letter and ". ";
        // continuations hang under that prefix.
        int hang = 0;
        {
            size_t p = start;
            if (p < end && text[p] == '\x01') ++p;
            if (p + 2 < end && text[p + 1] == '.' && text[p + 2] == ' ' &&
                ((text[p] >= 'A' && text[p] <= 'Z') || (text[p] >= 'a' && text[p] <= 'z'))) {
                char prefix[4] = {text[p], '.', ' '};
                prefix[3] = 0;
                hang = ui_text_fixed_width_px(px, prefix);
                if (hang > width / 2) hang = width / 2;
                if (hang > 255) hang = 255;
            }
        }
        bool seg_first = true;
        while (seg_len > 0 && SESS()->line_count < LINE_MAX) {
            // 行首 \x01 为强调标记：该行反色绘制；跳过该字节再开始本行
            // 内容，标记不占显示宽度。/ A leading \x01 is the emphasis
            // marker: the line draws inverted; skip the byte before the line
            // content starts, the marker takes no display width.
            bool invert = false;
            if (seg_first && text[start] == '\x01') {
                invert = true;
                ++start;
                --seg_len;
            }
            if (seg_len == 0) break;  // 段仅剩标记：无内容可显示 / marker-only segment
            // 首行顶格，续行悬挂在前缀宽度下，可用宽度相应变窄。
            // The first line sits at the margin; continuations hang under the
            // prefix, so their usable width shrinks accordingly.
            const int indent = seg_first ? 0 : hang;
            const int avail = width - indent;
            seg_first = false;
            // 二分找能放下的最大长度：比 25% 步长收缩更准（后者每行都留
            // 下一大片空白，行数因此偏多），测量次数也少。
            // Binary-search the longest fitting length: the old 25% shrink
            // left a wide gap on every line (and so produced extra lines),
            // while this needs fewer measurements.
            size_t lo = 1, hi = seg_len < max_take ? seg_len : max_take;
            size_t take = 1;
            while (lo <= hi) {
                const size_t mid = lo + (hi - lo) / 2;
                memcpy(probe, text + start, mid);
                probe[mid] = 0;
                if (ui_text_fixed_width_px(px, probe) <= avail) {
                    take = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            // 截断点对齐 UTF-8 字符边界：text[start+take] 是续行字节说明
            // 切在了多字节字符中间，回退到字符起点；take 已缩到 1 且首字节
            // 是引导字节时向前吃满整个字符，不留孤立引导字节。/ Align the
            // cut to a UTF-8 boundary: a continuation byte at
            // text[start+take] means the cut split a multi-byte character,
            // so back off to its start; when take has shrunk to 1 on a lead
            // byte, consume the whole character rather than leave a lone
            // lead byte behind.
            while (take > 1 && ((unsigned char)text[start + take] & 0xC0) == 0x80) --take;
            if (take == 1 && ((unsigned char)text[start] & 0xC0) == 0xC0) {
                while (start + take < end && ((unsigned char)text[start + take] & 0xC0) == 0x80) ++take;
            }
            // 优先在空格处断（空格是单字节，断后必为字符边界）。
            // / Prefer breaking at a space (ASCII, so the cut is a boundary).
            size_t cut = take;
            for (size_t k = cut; k > 1; --k) {
                if (text[start + k - 1] == ' ') {
                    cut = k;
                    break;
                }
                if (k + 8 < cut) break;  // 只回看一小段 / only look back a little
            }
            // 避头尾：切点可能落在标点上，按禁则调整。
            // The cut may land on punctuation; the break rules settle it.
            cut = cjk_fix_break(text + start, cut, seg_len, px, avail, probe, sizeof(probe));
            SESS()->line_off[SESS()->line_count] = start;
            SESS()->line_len[SESS()->line_count] = cut;
            SESS()->line_invert[SESS()->line_count] = invert;
            SESS()->line_height[SESS()->line_count] = (uint16_t)pitch;
            SESS()->line_x[SESS()->line_count] = (uint8_t)indent;
            ++SESS()->line_count;
            start += cut;
            while (start < end && text[start] == ' ') ++start;  // 吞掉断行空格 / drop the break space
            seg_len = end - start;
        }
        i = end + 1;
    }
}

// 内容区高度：页眉分隔线到底栏之间，上下各留 8px；学习页还要给底部提示
// 行（正面）或评分按钮（背面）让位，否则最后一行会压在提示或按钮上。
// Height of the content band: between the header rule and the bar, 8px clear
// on each side; the study page also leaves room for the bottom hint (front)
// or the grade buttons (back), so the last line never lands on them.
static int study_content_height(void) {
    const int bottom = SESS()->show_back ? (UI_CONTENT_BOTTOM - UI_BTN_H - 4)
                                          : (UI_CONTENT_BOTTOM - UI_PX_CAPTION - 10);
    return bottom - (UI_CONTENT_TOP + 8);
}

// 全部行高之和。/ Sum of every line height.
static int card_total_height(void) {
    int total = 0;
    for (int i = 0; i < SESS()->line_count; ++i) total += SESS()->line_height[i];
    return total;
}

// 排版并按目标页数自适应字号：正文太长时字号按 -2 步进缩小，取第一个
// 不超标的字号，下限 16px。只作用于学习视图正文，列表等固定字号不动。
// 行距与字号成正比（字形盒按字号线性缩放），所以只在基准字号量一次，
// 其余字号按比例缩放，不必每档重新量字形。
// Lay the card out and shrink the type when it runs past the page target:
// the size steps down by 2 and the first size that fits wins, with a 16px
// floor. Only the study body is affected; fixed-size lists stay untouched.
// The pitch scales with the type size (glyph boxes scale linearly), so it is
// measured once at the base size and scaled for the rest.
static void layout_card(void) {
    const int px0 = body_px();
    const char* text = SESS()->text ? SESS()->text : "";
    const int pitch0 = body_pitch(px0, text);
    int px = px0;
    for (;;) {
        // 向上取整：宁松勿紧，行距比字形盒差一像素也不会压字。
        // Round up: a pitch one pixel larger than the glyph box is safe.
        const int pitch = px == px0 ? pitch0 : (int)(((long)pitch0 * px + px0 - 1) / px0);
        build_card_lines(px, pitch);
        if (px <= ANKI_MIN_BODY_PX) break;
        if (card_total_height() <= study_content_height() * ANKI_PAGES_TARGET) break;
        px -= 2;
    }
    SESS()->text_px = px;
}

// 按累计行高分页：给出指定页的首行、行数与总页数。
// Page by accumulated line height: the first line, the line count and the
// total page count of one page.
static void study_page_map(int want, int* first, int* count, int* pages) {
    const int limit = study_content_height();
    int page = 0, used = 0;
    *first = 0;
    *count = 0;
    *pages = 1;
    bool taking = want == 0;
    for (int i = 0; i < SESS()->line_count; ++i) {
        int h = SESS()->line_height[i];
        if (h <= 0) h = 1;
        if (used > 0 && used + h > limit) {
            ++page;
            used = 0;
            taking = want == page;
        }
        if (taking) {
            if (*count == 0) *first = i;
            ++*count;
        }
        used += h;
    }
    *pages = page + 1;
}

static void session_load_back(void);

static void session_open_card(void) {
    SESS()->show_back = false;
    SESS()->back_loaded = false;
    SESS()->text = SESS()->front;
    layout_card();
}

static void session_show_back(void) {
    SESS()->show_back = true;
    if (!SESS()->back_loaded) session_load_back();
    SESS()->text = SESS()->back;
    layout_card();
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
    session_load_front();
    session_open_card();
    s_view = ANKI_VIEW_STUDY;
}

static void session_end(void) {
    anki_store_unload(&SESS()->deck);
    SESS()->active = false;
    s_view = ANKI_VIEW_DECKS;
    scan_decks();
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
            study_page_map(SESS()->line_page, &first, &count, &pages);
            if (SESS()->line_page >= pages) {
                SESS()->line_page = pages > 0 ? pages - 1 : 0;
                study_page_map(SESS()->line_page, &first, &count, &pages);
            }
            int y = UI_CONTENT_TOP + 8;
            char line[LINE_BUF];
            for (int k = 0; k < count; ++k) {
                const int i = first + k;
                size_t n = SESS()->line_len[i];
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, SESS()->text + SESS()->line_off[i], n);
                line[n] = 0;
                // 行首带 \x01 强调标记的行反色绘制（黑底白字），墨水屏上
                // 清晰醒目；其余行常规黑字。行首缩进来自排版结果，选项续行
                // 悬挂在前缀宽度下。/ Lines carrying the leading \x01 emphasis
                // marker draw inverted (white on black) for clarity on
                // e-paper; every other line draws ordinary black. The indent
                // comes from the layout, so option continuations hang under
                // their prefix.
                if (n > 0) {
                    ui_text_fixed(fb, UI_MARGIN + SESS()->line_x[i], y, SESS()->text_px, line,
                                  EPD_DRAW_ALIGN_LEFT, SESS()->line_invert[i]);
                }
                y += SESS()->line_height[i];
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
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40 + body_pitch(body_px(), ""), UI_PX_BODY, line,
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
        // 防御：带会话状态重进本页时先卸载旧会话。/ Defensive: drop a live
        // session when re-entering the page.
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
            study_page_map(SESS()->line_page, &first, &count, &pages);
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
