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

## 0.3.3-rc107.01 — 2026-11-12

### 变更
- 合并官方 rc107 基线：设置列表快滑翻半屏、松手即清残影，阅读缓存迁入 TF 卡 `.kiikoread` 目录，阅读空闲 8 秒后关闭屏幕电源。
- 按用户决定移除设置页二级分组菜单，回归官方单列滚动列表；Anki 双通道联网升级与升级页固定字号排版保留。
- 卡组答案界面：正确答案黑色加粗（TTF 字重 700，内建字体 1px 双描边），弃用错误选项反色的旧突出方案。

### 修复
- 设定自动休眠后设备常亮不休眠：随官方 rc107 在锁屏浅睡前读回屏幕断电状态，失败时重新断电并限频重试，不再停留在常亮锁屏。
- 宿主测试桩与上游新增声明重复定义 `ble_pt_take_standby` 导致编译失败。

### 已知限制
- 真机的休眠观感、设置列表快滑手感与加粗显示效果需上机确认。

## 0.3.3-rc106.03 — 2026-11-12

### 新增
- 基于官方 rc106 基线新增卡组学习进度记录：断点续学与进度导出/回写。
- 基线沿用官方 rc106 的阅星瞳 APP 导入、星曈云同步、阅读控制与书架百分比显示。
- 进度备份是可读纯文本文件，可拷回电脑保存；恢复前二次确认，按卡片 id 逐条覆盖并汇总张数。
- 续学状态与备份落在活动根目录（插 TF 卡落卡内，否则落内部存储可经 USB 取出），已有卡组无需重新导入。
- 卡组列表行内显示续学位置「续 N/M」并新增导出按钮，恢复入口复用文件选择页，不新增页面。
- 进度恢复整份卡组在内存中改写后原子替换，掉电或写盘失败时设备进度不变，老卡组无需重新导入。

### 修复
- 换机或重刷后复习卡永不到期：进度恢复时同步设备日计数与上次对时日，避免无对时设备把所有复习卡判为未到期。

### 已知限制
- 备份只含学习进度，不是卡组包；恢复前提是设备已有同 id 卡组，不改写 Anki 桌面版集合库。

## 0.3.3-rc106.02 — 2026-11-11

### 修复
- 设置二级页无法返回上一级：分组页被当成主页面，侧键变成了切底部标签，返回键也到不了索引；现在侧键与左缘右滑都回索引。
- 分组页空白处误触：组内最后一行下方的空白被平移进下一分组的坐标，轻点就会打开别的设置项；现在点击只在本组行带内生效。
- 官方版联网升级只显示「已是最新」：官方基线已并入本机分支版时改为明确提示，不再让人以为通道失效。

### 新增
- 联网升级日志取自本仓变更日志首条，显示这版实际改了什么（此前是固定的一句话）。
- 合并官方 rc106 的同步源：阅星瞳 APP 导入、星曈云同步、阅读控制（排版/翻页/手势）、书架百分比显示。

## 0.3.3-rc106.01 — 2026-11-11

### 新增
- **设置改为「索引 + 二级分组」**：主页面只留头像卡片和四个分组入口（连接与时间 / 显示 / 个性化功能 / 升级和恢复），点进去后该组各行单屏显示。两级都放得下，不再需要长距离滑动，也取消了主列表的滚动位移。
  Settings now use an index plus one page per group: the main screen keeps the profile card and four group entries (connection/time, display, personalisation, upgrade/recovery) and each group opens on a single screen. No long swipes, and the main list no longer scrolls at all.

### 变更
- 合并上游 `v0.3.3-rc106`：锁屏清屏改为「真实历史 + 一次 GC16 白目标」再输出锁图（`present_clean_lock`），这也是实机反馈修复成片残留的那处改动；另含页脚章内页码、跨文件 EPUB 脚注、书架封面缓存到 TF、锁屏解锁提速。
  Merged upstream v0.3.3-rc106: the lock screen now runs one calibrated white GC16 pass over the true history before the final art (`present_clean_lock`) — the change device testers report fixes the ghost patches — plus chapter page numbers in the footer, cross-file EPUB footnotes, shelf cover caching on TF and faster lock/unlock.
- 显示路径按上游实现走：`highlevel.c` 与 `pico_reader_crop_host_test.py` 采用上游版本，本 fork 之前的 epdiy 回退与配套契约探针已撤除。
  The display path follows upstream: highlevel.c and the crop host test are taken as-is, and the fork's earlier epdiy revert and its contract probe are gone.

## 0.3.3-rc105.02 — 2026-11-11

### 修复
- 阅读页长时间使用后出现成片黑斑。rc105 的 `96b96e8` 把 Pico 局部刷新的驱动列裁剪到请求矩形，回写却仍按矩形进行；被跳过的列在面板上逐次翻页累积电荷，最终连成黑块。现已恢复 rc102 的契约：驱动、扫描与回写都落在同一个差分对齐段（S31 未受影响）。
  Fixed black patches after long reading. rc105's `96b96e8` masked the Pico drive to the requested rectangle while still committing it, so skipped columns accumulated charge turn after turn. The rc102 contract is back: drive, scan and commit all cover the same aligned segment (S31 untouched).
- 新增 `tools/display_commit_contract_host_test.py`，以源码断言锁住"驱动=回写"契约，并已接入 CI，防止裁剪逻辑再次回潮。
  Added `tools/display_commit_contract_host_test.py`, a source-level guard on the drive-equals-commit contract, wired into CI.

### 已知限制
- 恢复整段驱动后，沉浸阅读时页脚未读进度线在局部刷新中可能重新出现轻微闪动（rc105 曾以裁剪规避，代价就是这次的黑斑）。
  With segment-wide driving restored, the unread footer progress track may flicker slightly during partial refreshes in immersive reading — rc105 avoided that by masking, which caused these black patches.

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
