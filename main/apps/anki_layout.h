/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 学习页的排版单元：折行、避头尾、悬挂缩进与自适应字号。纯逻辑，
 *       不碰界面与存储：字宽、行高与字体度量都从外部函数取——真机用
 *       ui_kit / ui_font / ttf_font，宿主探针用假实现，所以同一份代码两边
 *       都能跑，探针测的就是设备上跑的那份。
 * English: The Anki study page's layout unit: wrapping, CJK break rules,
 *          hanging indents and the adaptive type size. Pure logic with no UI
 *          or storage: widths, line heights and font metrics come from
 *          external functions (the real ui_kit / ui_font / ttf_font on device,
 *          fakes in the host probe), so one implementation serves both and
 *          the probe exercises exactly what the device runs.
 *
 * 冻结：不执行脚本、不加载网络资源；行数与行长都有上限，输出有界。
 * Frozen: never executes scripts or loads network resources; the line count
 * and line length are capped, so the output stays bounded.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/// 单卡正文行数上限：超长卡片到此封顶，避免折行数组无界增长。220 时超长
/// 卡片会被静默截断，提到 400 后常见长卡可完整显示；布局状态随会话结构体
/// 走 PSRAM 分配，不占内部 RAM。
/// Per-card body line cap: overlong cards stop here so the wrap arrays
/// cannot grow without bound. At 220 long cards were silently truncated;
/// 400 shows common long cards in full. The layout state rides along in the
/// PSRAM-allocated session struct, so internal RAM stays free.
#define ANKI_LINE_MAX 400
/// 折行探测与绘制行缓冲的容量，即单行字节上限。
/// Probe/line buffer capacity, i.e. the byte cap per line.
#define ANKI_LINE_BUF 512
/// 自适应字号：正文超过这个页数就缩小字号，取第一个不超标的字号。
/// Adaptive size: shrink the body type when it runs past this many pages and
/// keep the first size that fits.
#define ANKI_PAGES_TARGET 3
/// 自适应字号下限：再小就读不清了。/ Adaptive size floor: below this the text stops being readable.
#define ANKI_MIN_BODY_PX 16

/// 外部依赖：字宽、内容宽与字体度量。真机由 ui_kit / ui_font / ttf_font 提供，
/// 宿主探针提供可重复的假实现。
/// External dependencies: text widths, the content width and the font
/// metrics. The device provides them from ui_kit / ui_font / ttf_font; the
/// host probe provides reproducible fakes.
int ui_text_effective_px(int px);
int ui_content_width(void);
int ui_text_fixed_width_px(int px, const char* text);
int ui_font_ascender_px(int px);
int ui_font_descender_px(int px);
void ui_font_measure_line_px(int px, const char* text, int* above, int* below);
int ttf_ascender_px(int px);
int ttf_descender_px(int px);

/// 内容区：首行上沿、内容下边界，以及底部提示行/评分按钮各占多高。屏幕尺寸
/// 是界面的事，排版单元只吃这四个数，宿主探针因此不必依赖 ui_kit.h。
/// The content band: where the first line starts, where the content ends and
/// how much room the bottom hint or the grade buttons take. The screen size
/// belongs to the UI, so the layout unit takes these four numbers and the
/// host probe stays free of ui_kit.h.
typedef struct {
    int top;            ///< 首行上沿 / top of the first line
    int bottom;         ///< 内容下边界 / bottom of the content area
    int hint_block_h;   ///< 底部提示行占位（正面）/ room for the bottom hint
    int buttons_block_h;///< 底部按钮占位（背面）/ room for the grade buttons
} anki_band_t;

/// 折行结果：一段文本拆成的行，以及每行的高度与缩进。
/// The wrapped result: the lines of one text plus each line's height and
/// indent.
typedef struct {
    const char* text;                      ///< 当前文本 / the text on screen
    size_t line_off[ANKI_LINE_MAX];        ///< 行首在 text 中的字节偏移 / byte offset of each line
    size_t line_len[ANKI_LINE_MAX];        ///< 行字节数 / byte length of each line
    bool line_invert[ANKI_LINE_MAX];       ///< 该行是否反色绘制 / draw this line inverted
    uint16_t line_height[ANKI_LINE_MAX];   ///< 行高（像素）/ line height in pixels
    uint8_t line_x[ANKI_LINE_MAX];         ///< 行首缩进（0 = 左边距）/ left indent, 0 = margin
    int text_px;                           ///< 排版实际字号 / the size the layout settled on
    int line_count;                        ///< 行数 / number of lines
    int line_page;                         ///< 当前页 / current page
    bool show_back;                        ///< 是否背面（决定内容区留位）/ back side (content band)
    anki_band_t band;                      ///< 内容区 / the content band
} anki_layout_t;

// ---------------------------------------------------------------- 排版

// 正文排版与绘制同口径：ui_text() 内部会把每个字号过一次
// ui_text_effective_px()（+2 且按系统字号百分比缩放），折行测量与行距
// 必须复用该口径，否则量窄画宽、文本超屏。/ Body layout shares the draw
// metric: ui_text() passes every size through ui_text_effective_px()
// (+2 plus system-size scaling), so wrap measurement and line pitch must
// reuse it, or lines measure narrow, draw wide and overflow the screen.
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
static inline int anki_layout_pitch(int px, const char* text) {
    // 上延伸与下延伸分别取各来源的最大值，再加 2px 行间隙。这样任一相邻两行
    // 的「上行下延伸 + 下行上延伸」都不超过 pitch-2，墨迹必然不重叠：正文
    // 实测覆盖内建字库（含 ui-hanzi.bin 补充位图），字体设计值覆盖 TTF。
    // 不用硬编码的拉丁样本量行高：汉字的字形盒比拉丁高得多，用 "AA" 量会把
    // 中文行距压短，相邻两行的墨迹就会粘在一起。
    // The ascent and the descent each take the largest of every source, plus
    // a 2px gap, so the descent of any line plus the ascent of the next can
    // never exceed pitch-2 and the ink cannot overlap. The measured text
    // covers the built-in face (including the supplementary hanzi bitmaps) and
    // the font design metrics cover the TTF. A hardcoded Latin sample would
    // under-measure Chinese lines.
    int above = 0, below = 0;
    ui_font_measure_line_px(px, text, &above, &below);
    int top = above;
    int bottom = below;
    const int builtin_asc = ui_font_ascender_px(px);
    const int builtin_desc = ui_font_descender_px(px);
    const int ttf_asc = ttf_ascender_px(px);
    const int ttf_desc = ttf_descender_px(px);
    if (builtin_asc > top) top = builtin_asc;
    if (ttf_asc > top) top = ttf_asc;
    if (builtin_desc > bottom) bottom = builtin_desc;
    if (ttf_desc > bottom) bottom = ttf_desc;
    const int pitch = top + bottom + 2;
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
static inline void anki_layout_build(anki_layout_t* L, int px, int pitch) {
    const char* text = L->text ? L->text : "";
    L->line_count = 0;
    L->line_page = 0;
    // 反色与缩进随折行全量赋值，这里再清零一遍，避免复用会话留下脏值。
    // The invert and indent flags are fully assigned during wrapping; zero
    // them here too so a reused session never keeps stale values.
    memset(L->line_invert, 0, sizeof(L->line_invert));
    memset(L->line_x, 0, sizeof(L->line_x));
    const int width = ui_content_width();
    // 探测缓冲一次分配复用；候选长度先钳到容量内，测量永远是完整候选的
    // 宽度，超长无空格串也会在宽度内被切短。/ One reusable probe buffer;
    // the candidate is clamped to its capacity first, so measurement always
    // covers the full candidate and long no-space runs still cut to width.
    static char probe[ANKI_LINE_BUF];
    const size_t max_take = sizeof(probe) - 1;
    const size_t len = strlen(text);
    size_t i = 0;
    while (i < len && L->line_count < ANKI_LINE_MAX) {
        // 先按换行切段。/ Split on explicit newlines first.
        size_t end = i;
        while (end < len && text[end] != '\n') ++end;
        size_t seg_len = end - i;
        size_t start = i;
        // 空段就是一行空行：选择题背面靠它把题干、选项与答案分层。
        // An empty segment is a blank line: the choice back uses it to layer
        // the question, the options and the answer.
        if (seg_len == 0) {
            L->line_off[L->line_count] = start;
            L->line_len[L->line_count] = 0;
            L->line_invert[L->line_count] = false;
            L->line_height[L->line_count] = (uint16_t)pitch;
            L->line_x[L->line_count] = 0;
            ++L->line_count;
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
        while (seg_len > 0 && L->line_count < ANKI_LINE_MAX) {
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
            L->line_off[L->line_count] = start;
            L->line_len[L->line_count] = cut;
            L->line_invert[L->line_count] = invert;
            L->line_height[L->line_count] = (uint16_t)pitch;
            L->line_x[L->line_count] = (uint8_t)indent;
            ++L->line_count;
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
static inline int anki_layout_content_height(const anki_layout_t* L) {
    const int bottom = L->band.bottom -
                       (L->show_back ? L->band.buttons_block_h : L->band.hint_block_h);
    return bottom - L->band.top;
}

// 全部行高之和。/ Sum of every line height.
static inline int anki_layout_total_height(const anki_layout_t* L) {
    int total = 0;
    for (int i = 0; i < L->line_count; ++i) total += L->line_height[i];
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
static inline void anki_layout_run(anki_layout_t* L, int body_px) {
    const int px0 = body_px;
    const char* text = L->text ? L->text : "";
    const int pitch0 = anki_layout_pitch(px0, text);
    int px = px0;
    for (;;) {
        // 向上取整：宁松勿紧，行距比字形盒差一像素也不会压字。
        // Round up: a pitch one pixel larger than the glyph box is safe.
        const int pitch = px == px0 ? pitch0 : (int)(((long)pitch0 * px + px0 - 1) / px0);
        anki_layout_build(L, px, pitch);
        if (px <= ANKI_MIN_BODY_PX) break;
        if (anki_layout_total_height(L) <= anki_layout_content_height(L) * ANKI_PAGES_TARGET) break;
        px -= 2;
    }
    L->text_px = px;
}

// 按累计行高分页：给出指定页的首行、行数与总页数。
// Page by accumulated line height: the first line, the line count and the
// total page count of one page.
static inline void anki_layout_page_map(const anki_layout_t* L, int want, int* first,
                                        int* count, int* pages) {
    const int limit = anki_layout_content_height(L);
    int page = 0, used = 0;
    *first = 0;
    *count = 0;
    *pages = 1;
    bool taking = want == 0;
    for (int i = 0; i < L->line_count; ++i) {
        int h = L->line_height[i];
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

