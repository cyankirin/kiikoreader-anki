# 本仓库的修复与新增 / This fork's changes

<!--
  中文：这份文件是**本仓库**（kiikoread · Anki fork）的变更记录，与上游 `docs/CHANGELOG.md`
        分开保存，部署时由 `tools/stage_changelog.py` 把两者合并成更新日志页
        `/kiikoread-anki/changelog.html`。格式固定：

            ## <版本号> — <日期>
            ### 新增 | 修复 | 已知限制
            - 一条一行

        第一条的版本号必须等于 `boards/pico/board.json` 的 version，闸门会拦下版本脱节的
        情况：日志写了新版本而固件没跟上（或反过来）都会在部署前失败。
  English: This file records **this repository's** changes (kiikoread · Anki fork), kept apart
        from upstream's `docs/CHANGELOG.md`. `tools/stage_changelog.py` merges both into
        `/kiikoread-anki/changelog.html` at deploy time. The shape is fixed:

            ## <version> — <date>
            ### Added | Fixed | Known limits
            - one line per item

        The first version must equal the `version` in `boards/pico/board.json`; the deploy gate
        fails when the log and the firmware version drift apart.
-->

## 0.3.3-rc105.01 — 2026-11-05

### 新增
- 合并上游 `v0.3.3-rc105`：内建字库扩到 7668 字符（缺字回退、16 级灰阶补字、缩放边缘与逐字墨量校正），默认全屏、紧凑阅读线按钮、稳定的上下操作栏与分页，修复书架分页箭头。
  Merged upstream v0.3.3-rc105: 7668 embedded glyphs with fallback, 16-level supplements and per-glyph ink calibration, plus default fullscreen, compact guide controls, stable reader chrome/pagination and shelf paging fixes.
- Pico 局部刷新按精确像素边界屏蔽，沉浸阅读普通翻页不再把页脚未读进度线带进灰阶重驱；后缓冲只提交实际刷新范围。
  Pico partial updates are masked to exact pixel bounds so an immersive body turn no longer re-drives the unread footer progress track; only the scanned bounds are committed.

### 修复
- 合并冲突逐项处置：CI 恢复为只门禁 Pico、`board.json` 增 `fork_supported`、`docs/CHANGELOG.md` 与语言包双源合并、PR59 驱动基线按**索引 blob**（LF，与 CI 一致）重算 22 项覆盖。
  Resolved the merge conflicts item by item: CI gates the Pico board only, `board.json` carries `fork_supported`, both changelog and locale sources were merged, and the PR59 driver baseline was recomputed from index blobs (LF, as CI sees them) for 22 overrides.

### 保留
- Anki：`.apkg` 导入、复习与学习记录；系统升级的「Anki 版联网升级」通道与官方通道并存；网页刷机 `/kiikoread-anki/` 的多版本选择、逐部件校验与更新日志页。
  Kept the Anki importer/review/progress, the Anki OTA channel alongside the official one, and the `/kiikoread-anki/` flasher with version picker, per-part verification and changelog.

### 已知限制
- 首次升级到本版本仍需网页刷机或线刷（通道本身在该固件里）；此后可在设备上联网更新。
  The first hop to this build still needs the web flasher or a cable (the channel lives inside this firmware); later updates run on the device.
- 应用镜像 **4,113,824 B / 4,194,304 B**，余量仅 **80 KB（1.9%）**。上游这版把内建字库扩到 7668 字，几乎吃满分区；后续加功能前必须先腾空间（例如按需裁剪未用字形或下线旧回退包）。
  The app image is 4,113,824 B of a 4,194,304 B partition — 80 KB (1.9%) left. Upstream's 7668-glyph expansion nearly filled it, so free space before adding features (trim unused glyphs or retire old rollback bundles).
- S31（metalio47）不再构建与门禁，源码保留以便合并上游。
  The S31 board is no longer built or gated; its sources stay for upstream merges.

## 0.3.3-rc104.02 — 2026-11-03

### 新增

- 设备「系统升级」页多一个通道选择：官方通道的清单来源、解析、下载与提交一字未改，新增的「Anki 版联网升级」只换了取清单的地址，两块板的行为差异只有「从哪个 URL 取清单」。
- Anki 通道的清单与镜像发布在本站站点根（`anki-update.json` 与 `Pico-anki-<版本>.bin`）：设备端 `pico_release_url_valid()` 只接受白名单前缀下的扁平 `.bin`，所以不用 GitHub Release 的下载地址（含子目录且会 302），Release 只作打包与发布记录。
- 网页刷机页新增版本下拉框：默认最新版，可回退到 rc104.01 / rc103.01 / rc102.01；每个版本都是一次完整刷机（引导程序、分区表、应用、启动槽标记四件套），并过与当前版相同的闸门。
- 刷机页地址从 `/anki/` 迁到 `/kiikoread-anki/`：旧地址不删，改放一个自动跳转页，已经发出去的链接与书签还能用；站点根下 `anki` 与设备端的 `anki-update.json` 挨在一起本来就容易被当成清单地址。
- 刷机页顶部加导航：原版刷机（本站根）、kiikoread-anki（当前页）、kiiko 在线刷机（上游官方页）、更新日志；链接一律用站点绝对路径，页面在两个路径下打开都不会断。
- 新增更新日志页 `/kiikoread-anki/changelog.html`：一页两段 ——「源仓库更新」（解析上游 `docs/CHANGELOG.md`，保留原有版本号与日期）与「本仓库修复与新增」（本文件），每段可锚点跳转，样式与刷机页一致。

### 修复

- 网页刷机现在真的校验：四个部件各下载一次，逐个核对尺寸、SHA-256、芯片身份与镜像内版本，全部一致才打开串口；任何一项不符立即中止，串口不打开、不擦除、不写入（此前页面承诺校验，实际并未执行）。
- 校正「擦除设备」的开关极性：清单里 `new_install_prompt_erase: false` 在安装对话框里等于「不询问、立即整片 chip erase」，会连设置与阅读进度一起抹掉；现在写 `true`，先弹「Erase device?」询问页，复选框默认不勾，保持不勾就保留数据。
- 校验模块没加载成功时页面改为「必然失败」：只能抛错的占位钩子先把 `prepareManifest` 占住，`integrity.js` 404 / MIME 不对 / 缓存过期 / 被 CSP 拦下时点「开始刷机」直接中止，不会写入未校验的字节。
- 部署闸门改为匹配真正的语句（先去掉 HTML 与 JS 注释再匹配）：注释里写着正确路径或钩子名，不再能让一个坏页面在绿灯下上线。
- 「系统升级」页 OTA 卡片的「联网 OTA」标题与说明行不再跟随系统字号放大：120% 时说明行会压进第一个通道行 4px，150% 时两行重叠，200% 时标题顶到通道行上沿；改成固定排版后四档字号与 100% 逐像素一致。
- 站点发布前先在同级临时目录生成 Anki 刷机包并过闸门，通过后才逐文件原子替换；闸门失败时目标目录保持原样，线上不会出现写了一半的清单或 bin。
- 宿主测试基线跟上上游与本 fork 的改动：系统字号探针补上 `ui_text_uses_builtin`，驱动基线通过既有的 `approved_overrides` 认可 `CMakeLists.txt` 的版本号来源变更，首页分钟刷新探针补上「组件暂停」分支，不再误报 `-Werror`。

### 已知限制

- 题目插图（`.apkg` 里的媒体）暂不显示，题目中的图会以占位文本出现。
- 0 张卡的卡组不在列表显示（例如导入后留下的空 `Default`）。
- 回退版本使用仓库**当前**的引导程序与分区表（各版本分区表逐行相同，仅应用镜像不同），不是当年那套字节；页面上也照实这么写。
- 网页刷机需要电脑上的 Chrome / Edge 89 及以上，并通过 HTTPS 或 `localhost` 打开；手机浏览器没有 WebSerial。
- 设备端联网升级要求基线不低于 `0.3.3-rc72`，更低的版本要先整机刷一次。
