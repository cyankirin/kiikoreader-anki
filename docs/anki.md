# Anki 卡组学习 / Anki deck study

为 kiikoread 增加 Anki 卡片学习能力：把 Anki 导出的卡组包导入设备，在墨水屏上
翻面学习，并按简化的 SM-2 规则记录每张卡的复习进度。导入、解析、渲染、调度与
持久化全部在设备本地完成，不依赖 Anki 运行时、网络或电脑。

Anki study on kiikoread: import Anki deck packages, flip through cards on the
e-paper panel and record per-card review progress with a simplified SM-2
scheduler. Import, parsing, rendering, scheduling and persistence all run
on-device; no Anki runtime, network or PC is required.

## 使用方式 / Usage

1. 用电脑把 Anki 导出的 `.apkg` 文件复制到 TF 卡根目录或 `anki/` 目录
   （旧版单个集合库 `collection.anki2` / `collection.anki21` 也可以直接放入）。
   Copy an Anki-exported `.apkg` to the TF card root or `anki/` (a bare
   `collection.anki2` / `collection.anki21` also works).
2. 打开菜单里的 **Anki 卡组** 页。
   Open the **Anki 卡组** page from the menu.
3. 点 **导入 Import** 选择文件；导入按卡组分别落盘，重复导入同一卡组会覆盖更新。
   Tap **导入 Import** and pick a file; each deck is stored separately and
   re-importing the same deck updates it.
4. 在卡组列表点某一卡组开始学习：正面 → 轻点或中键翻面 → 背面四选一评分
   （忘记/困难/良好/简单）。每次评分立即写回进度。
   Tap a deck to study: front → tap or middle key to flip → grade on the back
   (Again/Hard/Good/Easy). Every grade persists immediately.
5. 行内 **×** 删除已导入卡组（只删设备内的转换结果，不动 TF 卡上的 `.apkg`）。
   The in-row **×** deletes the imported deck (the converted copy only; the
   `.apkg` on the TF card is untouched).

按键与手势 / Keys and gestures：

| 操作 / Input | 行为 / Behavior |
| --- | --- |
| 轻点卡片正面 / tap the front | 翻到背面 / flip to the back |
| 轻点背面评分钮 / tap a grade button | 记录评分并进入下一张 / grade and advance |
| 中键（KEY2）/ middle key | 翻面；小结页确认返回 / flip; confirm on the summary |
| 长按中键 / long-press middle | 结束本组学习 / end the session |
| 左键（KEY1）/ left key | 逐级返回 / step back |
| 右键（KEY3）/ right key | 打开菜单 / open the menu |
| 上下滑 / swipe up or down | 长卡片在内翻页 / page within a long card |

### 选择题卡片 / Choice cards

选项字段是单字母（A、B、C、D…）、另有字段的值是这些字母的子集（如 "AC"）的卡组
走专用版面，模板里的脚本不执行：

Decks whose option fields are single letters (A, B, C, D, …) with another field
holding a subset of them (such as "AC") use a dedicated layout that runs no
template script:

- **正面 / Front**：元信息行（题号 / 来源 / 章节，取 `I`、`J`、`P` 三个字段，
  都空则不显示）→ `【单选】` 或 `【多选】` + 题干 → 空行 → 每个选项一行
  `<字母>. <文本>`。模板里只写在 `id` 属性上的字母与 `<li class="hidden">`
  这类隐藏条目不会进正文。
  A meta line (card id / source / section from the I, J and P fields, hidden
  when all three are empty), then `【单选】` or `【多选】` plus the question, a
  blank line and one `<letter>. <text>` line per option. Letters that only
  lived in `id` attributes and hidden items such as `<li class="hidden">`
  never reach the text.
- **背面 / Back**：同正面，再加空行、错项反色（正确项保持常规黑字；折行时整段反色）、
  `答案：X` 与 `备注：`（注释字段非空时）。
  The same plus a blank line, inverted wrong options (correct ones keep
  ordinary black ink; a wrapped option stays inverted on every line),
  `答案：X` and `备注：` when the note field is set.
- 其余卡组继续走模板路径，行为不变。
  Every other deck keeps the template path unchanged.

## 实现位置 / Implementation map

| 路径 / Path | 职责 / Role |
| --- | --- |
| `main/anki/anki_sqlite.h/.c` | 只读 SQLite 子集：页头、表 b-树、溢出页组装、记录解码；拒绝 WAL 与加密库。Read-only SQLite subset (header, table b-trees, overflow assembly, record decoding); rejects WAL and encrypted files. |
| `main/anki/anki_json.h/.c` | 最小 JSON 解析器：v2 `col.models/decks` 与 v3 模板配置。Minimal JSON parser for v2 col JSON and v3 template configs. |
| `main/anki/anki_apkg.h/.c` | `.apkg`/裸库导入：ZIP 取集合库，v2/v3 schema 解析为内存模型；跳过挂起卡与过滤卡组。Import: pulls the collection out of the ZIP, parses v2/v3 schemas into a model; skips suspended cards and filtered decks. |
| `main/anki/anki_render.h/.c` | 模板渲染：字段替换、`{{#}}/{{^}}` 条件段、cloze、hint、`{{FrontSide}}`，HTML 转纯文本复用 `html_to_blocks`。Template rendering: fields, conditional sections, cloze, hint, FrontSide; HTML-to-text reuses html_to_blocks. |
| `main/anki/anki_store.h/.c` | 设备存储：`/flash/anki/<卡组id>.akd` 定长记录、日计数同步、会话队列、简化 SM-2、进度原地更新。Storage: fixed-record `.akd` files, day-counter sync, session queue, simplified SM-2, in-place progress patching. |
| `main/apps/app_anki.c` | 页面：卡组列表、文件选择、导入、学习会话与小结。Page: deck list, file picker, import, study session and summary. |
| `main/apps/anki_layout.h` | 学习页排版单元：折行、避头尾、选项悬挂缩进与自适应字号；纯逻辑，字宽与字体度量由外部函数注入，设备与宿主探针共用同一份代码。Layout unit: wrapping, CJK break rules, option hanging indents and the adaptive type size; pure logic with the widths and font metrics injected, so the device and the host probe share one implementation. |
| `main/app/app_registry.c` | 菜单注册（追加一项，不动其它页）。Menu registration (one appended entry). |
| `main/CMakeLists.txt` | 新增 `anki/` 源与包含目录。Adds the anki sources and include dir. |
| `tools/gen_anki_fixtures.py` | 宿主夹具：真实 SQLite 集合打包成 `.apkg`（v2/v3/v1 调度/大库/坏文件）。Host fixtures: real SQLite collections packed as .apkg. |
| `tools/anki_host_test.c`、`tools/run_anki_host_tests.sh` | 宿主测试与运行脚本（CI 同款参数）。Host test and runner (same flags as CI). |
| `tools/anki_stubs/` | 宿主替身头文件与 vendored miniz（tinfl）。Host stand-in headers and vendored miniz (tinfl). |
| `tools/anki_layout_probe.c`、`tools/run_anki_layout_probe.sh` | 排版探针：直接 include 真机的排版头文件，只把字宽与字体度量换成确定性假实现；判据用探针自己的标点表与墨迹模型，不拿被测函数当判据。Layout probe: includes the real layout header and only swaps the widths and font metrics for deterministic fakes; its oracles are its own punctuation tables and ink model, never the function under test. || `tools/run_anki_layout_probe_mutations.sh` | 探针的变异验证：故意打断真机排版代码里的禁则分类与悬挂缩进，探针必须失败；探针若在变异下仍然通过，说明断言无效。Mutation check: breaks the real break rules and hanging indent on purpose and requires the probe to fail. |

## 依赖 / Dependencies

- 运行时复用既有组件：`zip_reader`（ZIP+ROM miniz）、`html_text`（HTML 转文本）、
  `book_store` 的内部 FAT 挂载（新增 `book_store_ensure_flash()`）、`ttf_font` 与
  `ui_kit` 绘制。无新增第三方运行时依赖。
  Runtime reuses existing pieces: zip_reader (ZIP + ROM miniz), html_text,
  book_store's internal FAT mount (new book_store_ensure_flash()), ttf_font
  and ui_kit. No new third-party runtime dependency.
- 宿主测试额外 vendor 了 miniz 的 tinfl 部分（`tools/anki_stubs/miniz*`，MIT，
  见 THIRD_PARTY_NOTICES.md），只为在 PC 上走通与设备相同的 ZIP/deflate 路径。
  Host tests additionally vendor miniz's tinfl part (MIT) so the same
  ZIP/deflate path runs on a PC.

## 数据与容量 / Data and limits

- 转换结果写在内部存储 `/flash/anki/<卡组id>.akd`，与书籍共享 5 MiB `storage`
  分区；单卡组上限 6 MiB，卡片上限 20 万、笔记上限 10 万，超出报错。
  Converted decks live in internal storage under /flash/anki, sharing the 5 MiB
  storage partition with books. Per-deck cap 6 MiB; 200k cards and 100k notes
  hard caps with clear errors.
- 进度记录在卡片定长记录里（52 字节），每次评分原地更新一格。
  Progress lives in fixed 52-byte card records, patched in place per grade.
- 到期日用设备日计数：有 RTC（WiFi 对时后）按 Unix 日推进；未对时保持当前计数，
  卡片不会消失，只是全部视为可学。
  Due dates use a device day counter: with an RTC (after WiFi time sync) it
  advances by Unix day; without one it holds, so cards never disappear and all
  count as available.
- v1 调度器的复习 due 按集合创建日重映射；v2/v3 按 Unix 日重映射。
  v1-scheduler review dues are remapped from the collection creation day; v2/v3
  from Unix days.

## 已知限制 / Known limits

- 仅文本卡片：字段里的图片显示 `[图片]`，`[sound:...]` 被剔除；不做音频播放。
  Text cards only: field images show [图片] and [sound:...] is dropped; no
  audio playback.
- `{{type:Field}}` 输入域不支持（电纸屏无输入框），按空处理。
  type: inputs are unsupported on e-paper and render empty.
- 模板里的 `{{Tags}}`/`{{Deck}}` 等特殊字段不展开（标签未导入）。
  Special fields such as Tags/Deck are not expanded (tags are not imported).
- 导入同步执行：大卡组会阻塞界面数秒，先显示“正在导入”。
  Import is synchronous; large decks block the UI for a few seconds behind a
  busy message.
- 真机刷屏、波形与功耗未验证（无设备）；宿主测试只覆盖解析、渲染与调度逻辑。
  On-device rendering, waveforms and power are unverified (no hardware); host
  tests cover parsing, rendering and scheduling logic only.
- 学习页行距按字体行盒（上延伸 + 下延伸 + 2px）算，正文实测墨迹与字体设计值
  取大者，保证相邻两行墨迹不重叠；改字体或字号后行距随之变化。
  The study page's line pitch follows the font line box (ascent + descent plus
  2px); the measured ink of the text and the font design metrics are combined
  so neighbouring lines never overlap, and the pitch follows any font or size
  change.

## 测试 / Tests

```sh
bash tools/run_anki_host_tests.sh        # 需要 gcc/clang + Python3
bash tools/run_anki_layout_probe.sh     # 排版探针：禁则、缩进、行利用率、自适应字号
bash tools/run_anki_layout_probe_mutations.sh   # 变异验证：探针必须抓得住打断
```

覆盖：JSON 反转义与非法输入、SQLite 大端/溢出页/多页 b-树/WAL 拒绝、v2/v3/v1
三种集合导入、模板条件段与 cloze、存储落盘-载入-评分-保存-重载闭环、坏文件拒绝。
Covers JSON unescaping and malformed rejection, SQLite endianness/overflow/
multi-page b-trees/WAL rejection, v2/v3/v1 imports, template conditionals and
cloze, and the store round trip (write, load, grade, save, reload).

排版探针覆盖：禁则（行首不落闭符号、行尾不落开符号）、选项续行的悬挂缩进、
每行的宽度上限与利用率、相邻两行的墨迹不重叠、以及过长正文缩到 16px 下限。
The layout probe covers the break rules (no closing mark at a line head, no
opening mark at a line tail), the hanging indent of option continuations, the
per-line width budget and fill, the no-overlap invariant between neighbouring
lines, and the shrink to the 16px floor for overlong bodies.
