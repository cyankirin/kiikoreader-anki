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

## 实现位置 / Implementation map

| 路径 / Path | 职责 / Role |
| --- | --- |
| `main/anki/anki_sqlite.h/.c` | 只读 SQLite 子集：页头、表 b-树、溢出页组装、记录解码；拒绝 WAL 与加密库。Read-only SQLite subset (header, table b-trees, overflow assembly, record decoding); rejects WAL and encrypted files. |
| `main/anki/anki_json.h/.c` | 最小 JSON 解析器：v2 `col.models/decks` 与 v3 模板配置。Minimal JSON parser for v2 col JSON and v3 template configs. |
| `main/anki/anki_apkg.h/.c` | `.apkg`/裸库导入：ZIP 取集合库，v2/v3 schema 解析为内存模型；跳过挂起卡与过滤卡组。Import: pulls the collection out of the ZIP, parses v2/v3 schemas into a model; skips suspended cards and filtered decks. |
| `main/anki/anki_render.h/.c` | 模板渲染：字段替换、`{{#}}/{{^}}` 条件段、cloze、hint、`{{FrontSide}}`，HTML 转纯文本复用 `html_to_blocks`。Template rendering: fields, conditional sections, cloze, hint, FrontSide; HTML-to-text reuses html_to_blocks. |
| `main/anki/anki_store.h/.c` | 设备存储：`/flash/anki/<卡组id>.akd` 定长记录、日计数同步、会话队列、简化 SM-2、进度原地更新。Storage: fixed-record `.akd` files, day-counter sync, session queue, simplified SM-2, in-place progress patching. |
| `main/apps/app_anki.c` | 页面：卡组列表、文件选择、导入、学习会话与小结。Page: deck list, file picker, import, study session and summary. |
| `main/app/app_registry.c` | 菜单注册（追加一项，不动其它页）。Menu registration (one appended entry). |
| `main/CMakeLists.txt` | 新增 `anki/` 源与包含目录。Adds the anki sources and include dir. |
| `tools/gen_anki_fixtures.py` | 宿主夹具：真实 SQLite 集合打包成 `.apkg`（v2/v3/v1 调度/大库/坏文件）。Host fixtures: real SQLite collections packed as .apkg. |
| `tools/anki_host_test.c`、`tools/run_anki_host_tests.sh` | 宿主测试与运行脚本（CI 同款参数）。Host test and runner (same flags as CI). |
| `tools/anki_stubs/` | 宿主替身头文件与 vendored miniz（tinfl）。Host stand-in headers and vendored miniz (tinfl). |

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

## 测试 / Tests

```sh
bash tools/run_anki_host_tests.sh   # 需要 gcc/clang + Python3
```

覆盖：JSON 反转义与非法输入、SQLite 大端/溢出页/多页 b-树/WAL 拒绝、v2/v3/v1
三种集合导入、模板条件段与 cloze、存储落盘-载入-评分-保存-重载闭环、坏文件拒绝。
Covers JSON unescaping and malformed rejection, SQLite endianness/overflow/
multi-page b-trees/WAL rejection, v2/v3/v1 imports, template conditionals and
cloze, and the store round trip (write, load, grade, save, reload).
