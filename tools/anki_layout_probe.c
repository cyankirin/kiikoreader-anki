/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 学习页排版单元的宿主探针。直接 include 真机的排版头文件
 *       （main/apps/anki_layout.h），把字体度量与字宽换成确定性假实现，
 *       所以测的就是设备上跑的那份代码。
 *       判据一律用探针自己的表与模型，不拿被测函数当判据：禁则分类被改坏
 *       时必须被抓到。
 * English: The host probe for the Anki study-page layout unit. It includes the
 *          real layout header (main/apps/anki_layout.h) and swaps the font
 *          metrics and text widths for deterministic fakes, so it exercises
 *          exactly the code the device runs. The oracles are the probe's own
 *          tables and ink model, never the function under test, so a broken
 *          classifier is actually caught.
 *
 * 冻结：只验证排版规则，不替代真机刷屏验证。
 * Frozen: verifies layout rules only; no substitute for on-device validation.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "anki_layout.h"

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/* ---- 假实现：探针替掉排版单元的外部依赖 --------------------------------- */

// 确定性假测量：CJK 与 emoji 宽 = px，ASCII 宽 = px/2。
// Deterministic fake measure: CJK and emoji take px, ASCII takes px/2.
int ui_text_fixed_width_px(int px, const char* text) {
    if (!text || !*text || px <= 0) return 0;
    const size_t len = strlen(text);
    int width = 0;
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            width += px / 2;
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            width += px;
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            width += px;
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            width += px;
            i += 4;
        } else {
            // 截断的多字节字符按一个字宽计一次，并跳过它的续行字节：
            // 真字体对半个字符也是按一个字量，探针不能多收一次。
            // A truncated multi-byte character counts as one character and its
            // continuation bytes are skipped: a real font measures half a
            // character as one glyph, so the probe must not charge twice.
            width += px;
            ++i;
            while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80) ++i;
        }
    }
    return width;
}

// 假字体度量：行盒 = 上延伸 + 下延伸 = px（与真字体同口径：ScaleForPixelHeight
// 把 asc-desc 映射到像素高）。汉字的墨迹比拉丁高，但仍在行盒里。
// Fake font metrics: the line box is ascent + descent = px, like a real font.
// CJK ink is taller than Latin ink yet still inside the box.
int ui_font_ascender_px(int px) { return px * 4 / 5; }
int ui_font_descender_px(int px) { return px / 5; }
int ttf_ascender_px(int px) { return px * 4 / 5; }
int ttf_descender_px(int px) { return px / 5; }

// 假墨迹模型：拉丁上延伸 px/2，汉字上延伸 3px/4；下延伸一律 px/4。
// Fake ink model: Latin reaches px/2 above the baseline, CJK 3px/4; the
// descent is px/4 for both.
static void fake_ink_px(int px, const char* text, int* above, int* below) {
    int top = 0, bottom = px / 4;
    const size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)text[i];
        int reach = px / 2;
        if (c >= 0x80) {
            reach = px * 3 / 4;
            i += (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        } else {
            ++i;
        }
        if (reach > top) top = reach;
    }
    if (above) *above = top;
    if (below) *below = bottom;
}

void ui_font_measure_line_px(int px, const char* text, int* above, int* below) {
    fake_ink_px(px, text, above, below);
}
int ui_text_effective_px(int px) { return px + 2; }
int ui_content_width(void) { return 604; }

/* ---- 探针自带判据 / the probe's own oracles ---------------------------- */

// 禁则表：探针自己维护一份，不调用被测的分类函数。
// The punctuation tables live here, independent of the code under test.
static const uint32_t k_closing[] = {
    0x3001, 0x3002, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0xFF01, 0xFF0C,
    0xFF0E, 0xFF09, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D, 0x2014, 0x2019,
    0x201D, 0x2026, 0x2030,
};
static const uint32_t k_opening[] = {
    0x3008, 0x300A, 0x300C, 0x300E, 0x3010, 0x3014, 0xFF08, 0xFF3B, 0xFF5B,
    0x2018, 0x201C,
};

static bool in_table(const uint32_t* table, size_t count, uint32_t cp) {
    for (size_t i = 0; i < count; ++i)
        if (table[i] == cp) return true;
    return false;
}

// 取码点与字符起点的小工具（探针自带，不依赖排版单元的内部函数）。
// Small helpers for code points and character starts (the probe's own, not
// the layout unit's internals).
static uint32_t probe_cp(const char* s, size_t len, size_t* used) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *used = 1;
        return c;
    }
    size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
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

static size_t probe_prev_start(const char* s, size_t at) {
    size_t p = at;
    while (p > 0 && ((unsigned char)s[p - 1] & 0xC0) == 0x80) --p;
    return p > 0 ? p - 1 : 0;
}

/* ---- 探针状态 / probe state --------------------------------------------- */

static anki_layout_t g_layout;

static anki_band_t probe_band(void) {
    return (anki_band_t){
        .top = 176 + 8,
        .bottom = 1076 - 8,
        .hint_block_h = 28 + 10,
        .buttons_block_h = 84 + 4,
    };
}

static void layout(const char* text, bool show_back) {
    memset(&g_layout, 0, sizeof(g_layout));
    g_layout.text = text;
    g_layout.show_back = show_back;
    g_layout.band = probe_band();
    anki_layout_run(&g_layout, ui_text_effective_px(38));
}

static const char* line_text(int i, char* buf, size_t cap) {
    size_t n = g_layout.line_len[i];
    if (n >= cap) n = cap - 1;
    memcpy(buf, g_layout.text + g_layout.line_off[i], n);
    buf[n] = 0;
    return buf;
}

// 分类函数逐码点直接断言：漏一个码点或整体取反都要被抓到。
// Direct per-codepoint assertions on the classifiers: a dropped code point
// or an inverted table must be caught.
static void check_classifiers(void) {
    printf("== classifiers ==\n");
    CHECK(cjk_closing(0x3002));  // 。
    CHECK(cjk_closing(0x3001));  // 、
    CHECK(cjk_closing(0xFF0C));  // ，
    CHECK(cjk_closing(0xFF01));  // ！
    CHECK(cjk_closing(0xFF1F));  // ？
    CHECK(cjk_closing(0xFF1A));  // ：
    CHECK(cjk_closing(0xFF1B));  // ；
    CHECK(cjk_closing(0x300D));  // 」
    CHECK(cjk_closing(0x300F));  // 』
    CHECK(cjk_closing(0x300B));  // 》
    CHECK(cjk_closing(0x3011));  // 】
    CHECK(cjk_closing(0xFF09));  // ）
    CHECK(cjk_closing(0x2026));  // …
    CHECK(cjk_closing(0x2014));  // —
    CHECK(!cjk_closing(0x6C49)); // 汉
    CHECK(!cjk_closing(0x6211)); // 我
    CHECK(!cjk_closing(0x0041)); // A
    CHECK(cjk_opening(0x300A));  // 《
    CHECK(cjk_opening(0x3008));  // 〈
    CHECK(cjk_opening(0x300C));  // 「
    CHECK(cjk_opening(0x300E));  // 『
    CHECK(cjk_opening(0x3010));  // 【
    CHECK(cjk_opening(0xFF08));  // （
    CHECK(cjk_opening(0xFF3B));  // ［
    CHECK(!cjk_opening(0x6211)); // 我
    CHECK(!cjk_opening(0x0041)); // A
}

// 逐行核对：禁则（独立表）、行宽上限、墨迹不重叠。
// Per line: the break rules (independent tables), the width budget and the
// no-overlap invariant.
static void check_lines(const char* name) {
    char buf[ANKI_LINE_BUF];
    const int width = ui_content_width();
    const int pitch = g_layout.line_count ? g_layout.line_height[0] : 0;
    printf("-- %s: %d lines, px=%d, pitch=%d\n", name, g_layout.line_count, g_layout.text_px,
           pitch);
    for (int i = 0; i < g_layout.line_count; ++i) {
        line_text(i, buf, sizeof(buf));
        const size_t n = strlen(buf);
        if (n == 0) continue;  // 空行 / blank line
        size_t used = 0;
        const uint32_t head = probe_cp(buf, n, &used);
        const size_t tail_start = probe_prev_start(buf, n);
        size_t tail_used = 0;
        const uint32_t tail = probe_cp(buf + tail_start, n - tail_start, &tail_used);
        // 禁则：用探针自己的表判，不调用被测函数。
        // The rules: judged with the probe's own tables, not the code under test.
        CHECK(!in_table(k_closing, sizeof(k_closing) / sizeof(k_closing[0]), head));
        CHECK(!in_table(k_opening, sizeof(k_opening) / sizeof(k_opening[0]), tail));
        const int indent = g_layout.line_x[i];
        const int line_width = ui_text_fixed_width_px(g_layout.text_px, buf);
        CHECK(line_width + indent <= width);
        printf("   [%02d] off=%3zu len=%3zu inv=%d x=%3u w=%3d | %s\n", i,
               g_layout.line_off[i], g_layout.line_len[i], (int)g_layout.line_invert[i],
               (unsigned)indent, line_width, buf);
    }
    // 墨迹不重叠：pitch ≥ below(上行) + above(下行)，用探针自己的墨迹模型算。
    // No overlapping ink: pitch ≥ below(previous) + above(next), judged with
    // the probe's own ink model.
    for (int i = 0; i + 1 < g_layout.line_count; ++i) {
        char a[ANKI_LINE_BUF], b[ANKI_LINE_BUF];
        line_text(i, a, sizeof(a));
        line_text(i + 1, b, sizeof(b));
        if (!a[0] || !b[0]) continue;
        int above = 0, below = 0, next_above = 0;
        fake_ink_px(g_layout.text_px, a, &above, &below);
        fake_ink_px(g_layout.text_px, b, &next_above, &above);
        CHECK(pitch >= below + next_above);
    }
}

// 逐段核对缩进：选项段的首行 x=0，续行 x=该段前缀宽度。
// Check the indent per segment: an option segment's first line sits at 0 and
// its continuations at the width of that segment's own prefix.
static void check_option_indents(void) {
    const char* text = g_layout.text ? g_layout.text : "";
    const size_t len = strlen(text);
    size_t seg_start = 0;
    int checked = 0;
    while (seg_start < len) {
        size_t seg_end = seg_start;
        while (seg_end < len && text[seg_end] != '\n') ++seg_end;
        size_t p = seg_start;
        if (p < seg_end && text[p] == '\x01') ++p;
        const bool is_option = p + 2 < seg_end && text[p + 1] == '.' && text[p + 2] == ' ' &&
                               ((text[p] >= 'A' && text[p] <= 'Z') ||
                                (text[p] >= 'a' && text[p] <= 'z'));
        if (is_option) {
            char prefix[4] = {text[p], '.', ' '};
            prefix[3] = 0;
            const int expect = ui_text_fixed_width_px(g_layout.text_px, prefix);
            int seen = 0;
            for (int i = 0; i < g_layout.line_count; ++i) {
                const size_t off = g_layout.line_off[i];
                if (off < seg_start || off >= seg_end) continue;
                if (seen == 0) {
                    CHECK(g_layout.line_x[i] == 0);
                } else {
                    CHECK(g_layout.line_x[i] == expect);
                }
                ++seen;
            }
            CHECK(seen >= 1);
            ++checked;
        }
        seg_start = seg_end + 1;
    }
    printf("   option segments checked: %d\n", checked);
}

// 总页数（按累计行高分页，与设备同一套口径）。
// The total page count (paged by accumulated line height, same metric as the
// device).
static int probe_pages(void) {
    int total = 1;
    for (int page = 0; page < 64; ++page) {
        int first = 0, count = 0, pages = 1;
        anki_layout_page_map(&g_layout, page, &first, &count, &pages);
        if (pages <= page + 1) return pages;
        total = pages;
    }
    return total;
}

static void case_chinese(void) {
    printf("== case: chinese ==\n");
    layout("我国依法负责对业余无线电台实施监督管理的机构是：（国家无线电管理机构和省、自治区、"
           "直辖市无线电管理机构）本题正确答案为A。",
           false);
    check_lines("chinese");
}

static void case_english(void) {
    printf("== case: english ==\n");
    layout("The authority responsible for supervising amateur radio stations is the radio "
           "management authority at national and provincial levels, and the answer is A.",
           false);
    check_lines("english");
}

static void case_mixed(void) {
    printf("== case: mixed ==\n");
    layout("CQ 表示通用呼叫（General Call），用于 CQ CQ DE BY1ABC，频率 145.05MHz，注意守听。",
           false);
    check_lines("mixed");
}

static void case_emoji(void) {
    printf("== case: emoji ==\n");
    layout("信号报告 🎙️📡 收到 QSA 59，OK 👌，谢谢！", false);
    check_lines("emoji");
}

static void case_spaces(void) {
    printf("== case: consecutive spaces ==\n");
    layout("第一段    第二段\t第三段（括号）结束。", false);
    check_lines("spaces");
}

static void case_long_token(void) {
    printf("== case: long no-space run ==\n");
    layout("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
           "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
           false);
    check_lines("long-token");
}

static void case_option_hanging(void) {
    printf("== case: option hanging indent ==\n");
    // 注意："\x01B" 会被 C 编译器吃成十六进制转义 0x1B，标记与字母必须
    // 拆成两段字面量。/ Note: "\x01B" is one hex escape (0x1B) to the C
    // compiler, so the marker and the letter must be separate literals.
    layout("\x01"
           "B. 在国家或地方民政部门注册的业余无线电协会，选项正文很长需要折行显示，"
           "所以续行必须悬挂在前缀宽度之下。",
           false);
    check_lines("option-hanging");
    check_option_indents();
    int indented = 0;
    for (int i = 0; i < g_layout.line_count; ++i) if (g_layout.line_x[i] > 0) ++indented;
    CHECK(indented >= 1);
    CHECK(g_layout.line_x[0] == 0);
    CHECK(g_layout.line_invert[0]);
}

static void case_choice_back(void) {
    printf("== case: choice back layout ==\n");
    layout("我国依法负责对业余无线电台实施监督管理的机构是：\n"
           "\n"
           "A. 国家无线电管理机构和省、自治区、直辖市无线电管理机构\n"
           "\x01"
           "B. 在国家或地方民政部门注册的业余无线电协会\n"
           "\x01"
           "C. 国家体育管理机构和地方体育管理机构\n"
           "\x01"
           "D. 国家和地方公安部门\n"
           "\n"
           "答案：A\n",
           true);
    check_lines("choice-back");
    check_option_indents();
    printf("   pages=%d\n", probe_pages());
    // 题干后与选项后各有一个空行：层级靠空行而不是字号。
    // One blank line after the question and one after the options: the
    // hierarchy comes from blank lines, not from a smaller size.
    int blanks = 0;
    for (int i = 0; i < g_layout.line_count; ++i) if (g_layout.line_len[i] == 0) ++blanks;
    CHECK(blanks == 2);
    // 正确项（A）与答案行不带标记，错误项（B/C/D）带标记。
    // The correct option and the answer carry no marker; the wrong ones do.
    char buf[ANKI_LINE_BUF];
    int correct_marks = 0, wrong_marks = 0, answer_marks = 0, answer_seen = 0;
    for (int i = 0; i < g_layout.line_count; ++i) {
        line_text(i, buf, sizeof(buf));
        if (!buf[0]) continue;
        // 标记字节在折行时已被摘掉，行内容就是可见文本。
        // The marker byte is dropped during wrapping, so the line content is
        // the visible text.
        if (buf[0] == 'A' && buf[1] == '.' && buf[2] == ' ') {
            correct_marks += g_layout.line_invert[i] ? 1 : 0;
        } else if (buf[0] >= 'B' && buf[0] <= 'D' && buf[1] == '.' && buf[2] == ' ') {
            wrong_marks += g_layout.line_invert[i] ? 1 : 0;
        }
        if (strncmp(buf, "答案：", 9) == 0) {
            ++answer_seen;
            answer_marks += g_layout.line_invert[i] ? 1 : 0;
        }
    }
    CHECK(correct_marks == 0);
    CHECK(wrong_marks == 3);
    CHECK(answer_seen == 1);
    CHECK(answer_marks == 0);
}

// 折行要能放就放：整段的空白总量要小（避头尾最多留一个字的空）。
// The wrap must fill the line: the total slack stays small (the break rules
// may leave at most one character of room).
static void case_fill(void) {
    printf("== case: line fill ==\n");
    layout("我国依法负责对业余无线电台实施监督管理的机构是国家无线电管理机构和省、自治区、"
           "直辖市无线电管理机构本题正确答案为A这是用来检验行利用率的整段中文文本。",
           false);
    check_lines("fill");
    char buf[ANKI_LINE_BUF];
    const int width = ui_content_width();
    int lines = 0, slack = 0;
    for (int i = 0; i < g_layout.line_count; ++i) {
        line_text(i, buf, sizeof(buf));
        if (!buf[0]) continue;
        ++lines;
        const int used = ui_text_fixed_width_px(g_layout.text_px, buf) + g_layout.line_x[i];
        if (used < width) slack += width - used;
    }
    printf("   lines=%d slack=%d avg=%d (one char = %d px) pages=%d\n", lines, slack,
           lines ? slack / lines : 0, g_layout.text_px, probe_pages());
    CHECK(lines > 0);
    // 平均每行的空白不超过一个汉字：25% 步长收缩的老做法会超出这个上限。
    // The slack per line stays within one character; the old 25% shrink
    // overshot this budget.
    CHECK(slack <= lines * g_layout.text_px);
}

static void case_adaptive_size(void) {
    printf("== case: adaptive size ==\n");
    // 足够长的卡：正文必须缩到 3 页以内（100 行在 40px 下放不下）。
    // A long card: the body must shrink to fit three pages (100 lines do not
    // fit at 40px).
    static char big[64 * 1024];
    size_t at = 0;
    for (int i = 0; i < 100; ++i) {
        const char* piece = "业余无线电考试题目正文内容需要足够长才能触发自适应字号。";
        size_t n = strlen(piece);
        if (at + n + 1 >= sizeof(big)) break;
        memcpy(big + at, piece, n);
        at += n;
        big[at++] = '\n';
    }
    big[at] = 0;
    layout(big, false);
    const int limit = anki_layout_content_height(&g_layout) * ANKI_PAGES_TARGET;
    printf("   px=%d lines=%d total=%d limit=%d\n", g_layout.text_px, g_layout.line_count,
           anki_layout_total_height(&g_layout), limit);
    CHECK(g_layout.text_px < ui_text_effective_px(38));
    CHECK(g_layout.text_px > ANKI_MIN_BODY_PX);
    CHECK(g_layout.text_px % 2 == ui_text_effective_px(38) % 2);
    CHECK(anki_layout_total_height(&g_layout) <= limit);
    // 到下限仍超长时保持下限，不再往下缩。
    // Still overlong at the floor: the size holds there.
    static char huge[256 * 1024];
    at = 0;
    while (at + 64 < sizeof(huge)) {
        memcpy(huge + at, "超长文本", 12);
        at += 12;
    }
    huge[at] = 0;
    layout(huge, false);
    printf("   huge: px=%d\n", g_layout.text_px);
    CHECK(g_layout.text_px == ANKI_MIN_BODY_PX);
}

// 反色标记随折行传播：整段（首行 + 全部续行）都反色，且不泄漏到下一段。
// The emphasis marker follows the wrap: the whole segment (first line and
// every continuation) is inverted, and it never leaks into the next segment.
static void case_emphasis_wrap(void) {
    printf("== case: emphasis across wrapped lines ==\n");
    // 1) 反色长选项：折成多行，每一行都要反色。
    // A marked long option wrapping over several lines: every line inverted.
    layout("\x01"
           "B. 在国家或地方民政部门注册的业余无线电协会，这一行足够长，必然折成三行以上才放得下。",
           false);
    check_lines("emphasis-wrapped");
    printf("   lines=%d\n", g_layout.line_count);
    CHECK(g_layout.line_count >= 3);
    for (int i = 0; i < g_layout.line_count; ++i) CHECK(g_layout.line_invert[i]);

    // 2) 无反色标记的长段：每一行都不反色（防止改反）。
    // A long segment with no marker: no line is inverted (guards a reversed fix).
    layout("我国依法负责对业余无线电台实施监督管理的机构是国家无线电管理机构和省、自治区、"
           "直辖市无线电管理机构，这一行同样足够长，必然折行显示。",
           false);
    check_lines("plain-wrapped");
    CHECK(g_layout.line_count >= 3);
    for (int i = 0; i < g_layout.line_count; ++i) CHECK(!g_layout.line_invert[i]);

    // 3) 反色段后面紧跟普通段：前者全行反色，后者不反色（段落隔离）。
    // A marked segment followed by a plain one: the former is inverted on every
    // line, the latter on none (the marker stays inside its segment).
    layout("\x01"
           "B. 在国家或地方民政部门注册的业余无线电协会，这一行足够长，必然折成三行以上才放得下。\n"
           "答案：B\n",
           false);
    check_lines("emphasis-then-plain");
    const int last = g_layout.line_count - 1;
    CHECK(last >= 3);
    for (int i = 0; i < last; ++i) CHECK(g_layout.line_invert[i]);
    CHECK(!g_layout.line_invert[last]);
    char tail[ANKI_LINE_BUF];
    line_text(last, tail, sizeof(tail));
    CHECK(strcmp(tail, "答案：B") == 0);
}

int main(void) {
    printf("== anki layout probe ==\n");
    check_classifiers();
    case_chinese();
    case_english();
    case_mixed();
    case_emoji();
    case_spaces();
    case_long_token();
    case_option_hanging();
    case_choice_back();
    case_emphasis_wrap();
    case_fill();
    case_adaptive_size();
    if (failures) {
        printf("anki_layout_probe: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("anki_layout_probe: PASS (%d checks)\n", checks);
    return 0;
}
