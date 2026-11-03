#!/usr/bin/env python3
"""Stage only the files intended for the public HTTPS flashing site."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import shutil
import json
import re
import sys
from urllib.parse import urlparse

from verify_flash_bundle import (check, check_archived_releases, check_anki_bundle,
                                  check_anki_ota, check_anki_releases, check_anki_site)
from release_assets import hydrate
from metalio_release_assets import stage_metalio
from stage_anki_flash import stage as stage_anki, verify as verify_anki
from stage_changelog import stage as stage_changelog


ROOT = Path(__file__).resolve().parents[1]
FLASH = ROOT / "flash"

# 中文：Anki 版刷机页的发布目录。地址从 `/anki/` 迁到 `/kiikoread-anki/`：`anki` 这个名字
#      在本站根下与设备端的 `anki-update.json` 太像，用户也分不清「刷机页」与「OTA 清单」。
#      旧地址不删——那里改放一个跳转页，已经发出去的链接和书签都还能用。
# English: Where the Anki flasher lives on the published site. The address moved from
#      `/anki/` to `/kiikoread-anki/` because `anki` sat right next to the device's
#      `anki-update.json` and read like the OTA manifest. The old path is not removed: it now
#      carries a redirect page, so links and bookmarks that are already out there still work.
ANKI_DIR = "kiikoread-anki"
ANKI_LEGACY_DIR = "anki"
ANKI_REDIRECT_TARGET = "../kiikoread-anki/"
ANKI_CANONICAL_URL = "https://cyankirin.github.io/kiikoreader-anki/kiikoread-anki/"

# 中文：旧地址 /anki/ 的跳转页。`meta refresh` 负责自动跳，正文里的链接负责刷新被禁用的
#      情况，`canonical` 让搜索引擎把两台地址合成一个。三种手段都指向同一个新目录。
# English: The redirect page that keeps /anki/ alive. `meta refresh` moves the browser, the
#      body link covers readers with refresh disabled, and `canonical` folds the two addresses
#      into one for search engines. All three point at the same new directory.
LEGACY_REDIRECT_PAGE = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>kiikoread · Anki 刷机页已迁移</title>
<link rel="canonical" href="{canonical}">
<meta http-equiv="refresh" content="0; url={target}">
<meta name="robots" content="noindex">
<style>
  body {{ margin: 0; padding: 56px 20px; background: #fff; color: #111;
         font: 16px/1.7 -apple-system, "Segoe UI", "Noto Sans SC", "Microsoft YaHei", sans-serif; }}
  main {{ max-width: 520px; margin: 0 auto; }}
  a {{ color: #111; text-underline-offset: 3px; }}
</style>
</head>
<body>
<main>
  <p>Anki 版刷机页已经搬到 <a href="{target}">/kiikoread-anki/</a>，正在自动跳转。</p>
  <p>如果没有自动跳转，请点上面的链接；旧地址不再提供刷机清单，避免刷到过期的固件。</p>
</main>
</body>
</html>
"""


def write_legacy_redirect(directory: Path) -> Path:
    """Publish the redirect page that keeps the old `/anki/` address working.

    中文：只留下 index.html，其余旧文件一律不再发布——留着旧清单与 bin 会让人以为
          `/anki/` 仍然能刷机，而那一份已经不再跟随固件版本更新。
    English: Keep only index.html. Every other old file is dropped: leaving the stale manifest
          and binaries behind would suggest `/anki/` still flashes, while that copy no longer
          follows the firmware version.
    """
    directory.mkdir(parents=True, exist_ok=True)
    for stale in directory.iterdir():
        if stale.name == "index.html":
            continue
        if stale.is_dir():
            shutil.rmtree(stale)
        else:
            stale.unlink()
    page = directory / "index.html"
    page.write_text(LEGACY_REDIRECT_PAGE.format(target=ANKI_REDIRECT_TARGET,
                                               canonical=ANKI_CANONICAL_URL),
                    encoding="utf-8")
    return page


class LocalLinks(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for key, value in attrs:
            if key in {"href", "src", "manifest"} and value:
                self.links.append(value)


def stage(output: Path, ota_origin: str | None = None) -> None:
    output.mkdir(parents=True, exist_ok=True)
    for name in ("index.html", "notices.html", "manual.html", "site.css", "site.js", "installer-bridge.js", "flasher-integrity.js", "metalio-installer.js", "metalio-flash-core.js", "metalio-connection.js", "metalio-flash-reader.js", "metalio-transport.js", "metalio-native-stub.js", "_headers",
                 "manifest.json", "toc-preview.png", "firmware.bin",
                 "bootloader.bin", "partitions.bin", "ota_data_initial.bin", "Pico-update.bin", "update.json"):
        if (FLASH / name).is_file(): shutil.copy2(FLASH / name, output / name)
    # 官网仅发布展示资源与用户说明；内部构建及调试记录不进入网站。
    # Publish website assets and the user manual, excluding private build and debug files.
    shutil.copytree(FLASH / "assets", output / "assets", dirs_exist_ok=True)
    # 中文：只复制仓库中的公开说明书（正文、检索索引、图解、PDF）。
    # English: Copy the curated public manual, never its local rendering/debug workspace.
    shutil.copytree(FLASH / "manual", output / "manual", dirs_exist_ok=True)
    if (FLASH / "new-features").is_dir():
        shutil.copytree(FLASH / "new-features", output / "new-features", dirs_exist_ok=True)
    # 中文：只发布构建好的模拟器；宿主源码、个人书库和工具链留在站点外。
    # English: Publish built simulator assets only, excluding sources, personal files and toolchains.
    if (FLASH / "simulator").is_dir():
        shutil.copytree(FLASH / "simulator", output / "simulator", dirs_exist_ok=True)
    version = json.loads((FLASH / "manifest.json").read_text())["version"]
    versioned = FLASH / f"Pico-update-{version}.bin"
    if versioned.is_file(): shutil.copy2(versioned, output / versioned.name)
    hydrate(FLASH, output)
    stage_metalio(FLASH, output)
    # 中文：本 fork 的 Anki 版入口（/kiikoread-anki/）。清单与 bin 每次现场生成：优先用本次
    #      构建的包，没有构建包时用上面已校验过的站点镜像做基础部件、用 firmware/ 里入库的
    #      应用镜像，因此版本库中不需要保存重复的二进制。生成后仍要过两道校验才允许发布。
    # English: This fork's Anki entry (/kiikoread-anki/). The manifest and binaries are
    #      generated on every run: from this build's package when present, otherwise from the
    #      verified staging image (base parts) plus the committed app image in firmware/.
    #      Nothing duplicated has to live in git, and two verifiers gate the result.
    if (FLASH / "anki" / "index.html").is_file():
        anki_output = output / ANKI_DIR
        anki_output.mkdir(parents=True, exist_ok=True)
        shutil.copy2(FLASH / "anki" / "index.html", anki_output / "index.html")
        # 中文：页面用相对路径 import 浏览器端校验模块，必须一起发布，否则刷机按钮
        #      点下去只会报错（下面 stage_anki 的闸门也会拦）。
        # English: The page imports its browser-side verifier by relative path, so it has to
        #      be published too; without it the install button can only fail.
        shutil.copy2(FLASH / "anki" / "integrity.js", anki_output / "integrity.js")
        # 中文：版本选择清单是可回退版本的目录，页面按它渲染下拉框；每个版本的四件套由
        #       stage_anki 现场生成并逐版过闸门。
        # English: The catalog lists the rollback builds the page offers; stage_anki
        #      generates each bundle on the spot and gates every one of them.
        if (FLASH / "anki" / "releases.json").is_file():
            shutil.copy2(FLASH / "anki" / "releases.json", anki_output / "releases.json")
        # 中文：ota_dir 指站点根——设备端 Anki 版 OTA 的清单与镜像都要求放在站点根下的扁平
        #       文件名，任何子目录或 Release 地址都会被 pico_release_url_valid() 拒绝。
        # English: ota_dir is the site root: the device's Anki OTA manifest and image must sit
        #        as flat names there; a subdirectory or Release URL would be rejected by
        #        pico_release_url_valid().
        stage_anki(anki_output, base_dir=output, ota_dir=output)
        verify_anki(anki_output)
        anki_manifest = check_anki_bundle(anki_output)
        check_anki_ota(output, anki_output)
        check_anki_releases(anki_output, anki_manifest["version"])
        # 中文：更新日志页与本 fork 的刷机页同目录发布，页面顶部的「更新日志」由 ./changelog.html
        #      直接指向它；生成后立刻过闸门（版本对齐、两个分区、条目非空）。
        # English: The changelog page ships next to this fork's flasher, so the page's top-level
        #      "changelog" link can be a plain ./changelog.html; it is gated right after it is
        #      written (version in step, both sections present, entries non-empty).
        stage_changelog(anki_output)
        # 中文：旧地址 /anki/ 只留一个跳转页，把已经发出去的链接接到新目录上。
        # English: The old /anki/ path keeps a redirect page, so already-published links land
        #      on the new directory instead of a 404.
        write_legacy_redirect(output / ANKI_LEGACY_DIR)
        check_anki_site(output)
    # 保留已发布版本的固定下载地址，避免旧清单缓存或正在进行的更新突然遇到 404。
    # Keep immutable published URLs so cached manifests and in-flight updates do not suddenly get a 404.
    for upgrade in FLASH.glob("Pico-update-*.bin"):
        name = re.fullmatch(r"Pico-update-(\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?)\.bin", upgrade.name)
        if not name: continue
        with upgrade.open("rb") as source:
            header = source.read(112)
        assert header[48:80].split(b"\0")[0].decode("ascii") == name[1], "historical upgrade version mismatch"
        assert header[80:112].split(b"\0")[0] == b"Read_Pico", "historical upgrade project mismatch"
        shutil.copy2(upgrade, output / upgrade.name)
    # 按验证后的正式目录发布历史清单与基础包，不递归上传本地测试文件。
    # Publish only verified archived manifests/base parts, excluding local test files.
    # 最新基础包来自 Release；兼容性检查使用已经校验过的站点镜像。
    # The latest base comes from Release; compare archives with the verified staged image.
    archives = check_archived_releases(FLASH, output / "partitions.bin", output)
    previous = json.loads((FLASH / 'manifest.json').read_text()).get('previous_version')
    if previous:
        assert any(item['version'] == previous for item in archives), 'previous official release is missing'
    if (FLASH / "releases.json").is_file():
        shutil.copy2(FLASH / "releases.json", output / "releases.json")
        shutil.copy2(FLASH / "versions.js", output / "versions.js")
        for release in archives:
            destination = output / Path(release["manifest"]).parent
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(FLASH / release["manifest"], destination / "manifest.json")
            for name in ("bootloader.bin", "partitions.bin", "ota_data_initial.bin"):
                shutil.copy2(FLASH / Path(release["manifest"]).parent / name, destination / name)
    shutil.copytree(FLASH / "vendor/web", output / "vendor/web", dirs_exist_ok=True)
    shutil.copy2(FLASH / "vendor/LICENSE", output / "vendor/LICENSE")
    shutil.copytree(FLASH / "vendor/metalio", output / "vendor/metalio", dirs_exist_ok=True)
    licenses = {
        ROOT / "licenses/Apache-2.0.txt": "Apache-2.0.txt",
        ROOT / "licenses/kiikoread-Personal-Study-1.0.txt": "kiikoread-Personal-Study-1.0.txt",
        ROOT / "licenses/LUCIDE-ISC.txt": "LUCIDE-ISC.txt",
        ROOT / "components/jpegdec/LICENSE": "JPEGDEC-APACHE-2.0.txt",
        ROOT / "licenses/LGPL-3.0.txt": "LGPL-3.0.txt",
        ROOT / "licenses/GPL-3.0.txt": "GPL-3.0.txt",
        ROOT / "components/read_pico_search/LICENSE.pypinyin": "PYPINYIN-MIT.txt",
        ROOT / "main/assets/OFL-Noto.txt": "OFL-Noto.txt",
        ROOT / "licenses/CrossMux-MIT.txt": "CROSSMUX-MIT.txt",
        ROOT / "licenses/AnimateIcons-MIT.txt": "AnimateIcons-MIT.txt",
        ROOT / "licenses/FreeInk-SDK-MIT.txt": "FREEINK-SDK-MIT.txt",
        ROOT / "licenses/Metalio-MIT.txt": "METALIO-MIT.txt",
        ROOT / "licenses/WebP-BSD-3-Clause.txt": "WebP-BSD-3-Clause.txt",
        ROOT / "licenses/lowzag-MIT.txt": "lowzag-MIT.txt",
    }
    (output / "licenses").mkdir(exist_ok=True)
    for source, name in licenses.items():
        shutil.copy2(source, output / "licenses" / name)
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    (output / ".nojekyll").touch()

    # 中文：/kiikoread-anki/ 只在本次暂存过时才检查，避免上游没有该入口时误报。
    # English: Only inspect /kiikoread-anki/ when it was staged, so upstream checkouts stay valid.
    pages = ["index.html", "notices.html", "manual.html", "manual/index.html", "metalio/index.html"]
    if (output / ANKI_DIR / "index.html").is_file():
        pages.append(f"{ANKI_DIR}/index.html")
    if (output / ANKI_DIR / "changelog.html").is_file():
        pages.append(f"{ANKI_DIR}/changelog.html")
    if (output / ANKI_LEGACY_DIR / "index.html").is_file():
        pages.append(f"{ANKI_LEGACY_DIR}/index.html")
    for page in pages:
        links = LocalLinks()
        links.feed((output / page).read_text(encoding="utf-8"))
        for link in links.links:
            parsed = urlparse(link)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            # 中文：以 `/` 开头的是站点绝对路径，要相对**站点根**解析；按页面相对路径解析会
            #      跑到盘符根上（Windows 上 `Path('x') / '/'` 直接变成 `\`），把正确链接判成断链。
            # English: A leading `/` is a site-absolute path and must resolve against the site
            #      root. Resolving it like a page-relative path lands on the drive root (on
            #      Windows `Path('x') / '/'` is just `\`) and reports a good link as broken.
            if parsed.path.startswith("/"):
                target = (output / parsed.path.lstrip("/")).resolve()
            else:
                target = (output / Path(page).parent / parsed.path).resolve()
            if target.is_dir(): target = target / 'index.html'
            assert target.is_relative_to(output.resolve()) and target.is_file(), (
                f"broken local link in {page}: {link}")
    feed = json.loads((output / 'update.json').read_text())
    # 用户指定GitHub Pages为默认OTA源；只接受固件已允许的两个HTTPS来源。
    # The user selected GitHub Pages for OTA; retain the firmware's exact HTTPS allowlist.
    origin = ota_origin or feed['url'].rsplit('/', 1)[0]
    assert origin in ('https://kiikoread.com', 'https://wegooo-cell.github.io/read-pico-reader')
    feed['url'] = origin + '/' + f'Pico-update-{version}.bin'
    (output / 'update.json').write_text(json.dumps(feed, ensure_ascii=False, indent=2) + '\n')
    check(output)
    files = list(output.rglob("*"))
    count = sum(path.is_file() for path in files)
    size = sum(path.stat().st_size for path in files if path.is_file())
    print(f"Public site: {count} files, {size} bytes")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('output', nargs='?', type=Path, default=ROOT / '_site')
    parser.add_argument('--ota-origin', choices=['https://kiikoread.com', 'https://wegooo-cell.github.io/read-pico-reader'])
    args = parser.parse_args()
    stage(args.output, args.ota_origin)
