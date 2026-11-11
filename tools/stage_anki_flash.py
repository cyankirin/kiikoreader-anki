#!/usr/bin/env python3
"""Generate the Anki web-flash bundle (esp-web-tools manifest plus its binaries).

中文：本脚本把构建产物整理成 `flash/anki/`：复制 bootloader / 分区表 / 应用 / otadata
      四个部件，逐个**重新计算** size 与 sha256 写回清单，并核对烧写偏移；清单里的
      `version` 必须与应用镜像里 esp_app_desc_t 的版本一致，因此脚本可以自证。
      生成分两步：先写同级临时目录，校验通过后才原子替换到目标目录，闸门失败不留半成品。
      重复运行结果相同；`--check` 只校验、不写盘，供 CI 与本地复核使用。
English: Turn a build package into `flash/anki/`: copy the bootloader, partition table,
      app and otadata parts, recompute every size/SHA-256 from the real bytes, and check
      the flash offsets. The manifest `version` must equal the version inside the app
      image's esp_app_desc_t, so the bundle proves itself. Generation has two steps: write
      a sibling temporary directory, verify it, then atomically replace the destination, so
      a failing gate never leaves a half-valid bundle behind. The run is idempotent and
      `--check` verifies without writing, for CI and local review.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

from verify_flash_bundle import EXPECTED, release_url_valid


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = ROOT / "build" / "boards" / "packages" / "pico"
DEFAULT_DESTINATION = ROOT / "flash" / "anki"
DEFAULT_BASE_DIR = ROOT / "flash"
DEFAULT_APP_DIR = ROOT / "firmware"
# 中文：CI 没有构建产物，但仍要与本地验证过的字节完全一致，因此这三个基础部件单独入库。
# English: CI has no build output, yet the published bytes must equal the locally verified
#          ones, so the three base parts are tracked here as well.
BASE_PARTS_DIR = ROOT / "firmware" / "anki-base"
BOARD_JSON = ROOT / "boards" / "pico" / "board.json"
OTA_HEADER = ROOT / "main" / "ota_online.h"

# 中文：部件顺序固定，页面与清单都按这个顺序展示。
# English: Fixed part order, shared by the manifest and the page.
PART_ORDER = ("bootloader.bin", "partitions.bin", "firmware.bin", "ota_data_initial.bin")
BASE_PARTS = ("bootloader.bin", "partitions.bin", "ota_data_initial.bin")
APP_PART = "firmware.bin"

BUNDLE_NAME = "kiikoread · Anki"
FORK_MARKER = "kiikoread-anki"
BOARD = "pico"
PROJECT = "Read_Pico"
CHIP_FAMILY = "ESP32-S3"
CHIP_ID = 9                    # ESP32-S3 image header chip id
APP_DESC_MAGIC = 0xABCD5432    # esp_app_desc_t magic_word
APP_DESC_MIN = 176             # 4 + 4 + 8 + 32 + 32 + 16 + 16 + 32 + 32 bytes

# 中文：设备端 Anki 版 OTA 的清单与镜像。清单固定发布在站点的站点根（与
#      PICO_OTA_ANKI_FEED_URL 完全一致），镜像同样是站点根下的扁平 `Pico-anki-<版本>.bin`：
#      固件的 pico_release_url_valid() 只接受白名单前缀 + 不带子目录的 .bin，
#      因此 GitHub Release 的下载地址（含子目录且会 302）不能直接给设备用。
# English: The device's Anki OTA manifest and image. The manifest is published at the site
#      root, exactly matching PICO_OTA_ANKI_FEED_URL, and the image is a flat
#      `Pico-anki-<version>.bin` at the same root: the firmware's pico_release_url_valid()
#      accepts allowlisted prefixes plus a flat .bin without any subdirectory, so a GitHub
#      Release download URL (nested, 302) cannot be handed to the device.
OTA_PREFIX = "https://cyankirin.github.io/kiikoreader-anki/"
OTA_MANIFEST = "anki-update.json"
OTA_IMAGE_PREFIX = "Pico-anki-"
# 中文：与官方清单一致的最低基础版本；低于它先整机刷机。
# English: Same minimum base version as the official feed; older bases must re-flash.
OTA_MINIMUM_BASE = "0.3.3-rc72"
# 中文：notes 进设备端 char[384]，超长会被解析器拒绝。
# English: notes go into the device's char[384]; anything longer is rejected by the parser.
OTA_NOTES_MAX = 383
OTA_NOTES = "Anki 版：导入 .apkg 复习，保留设置与阅读记录。"

# 中文：设备升级页约六行、120 个汉字。notes 从本仓变更日志当前版本条目里挑要点：先放
#       「新增/变更」里合并上游的条目（用户最想知道源仓库更新了什么），再放「修复」的简短
#       标题；按整条取舍而不是硬裁字符，避免把句子切成半截。
# English: The upgrade page fits ~120 CJK characters. Notes pick highlights from this fork's
#          changelog entry for the version: upstream-merge bullets first (that is what users
#          asked to see), then compact fix labels. Whole items are dropped when the page fills,
#          never cut mid-sentence.
OTA_NOTES_BULLETS = 4
OTA_NOTES_CHARS = 118
OTA_UPSTREAM_HINTS = ("上游", "官方", "合并", "rc1", "upstream", "Merged")

def changelog_path() -> Path:
    """Repository changelog, independent of where the site is staged to."""
    return Path(__file__).resolve().parents[1] / "docs/anki-changelog.md"


def _clean_bullet(text: str) -> str:
    """Strip markdown and keep the first Chinese sentence of one changelog bullet."""
    text = text.replace("**", "").replace("`", "").strip()
    cut = text.find("。")
    if cut != -1:
        text = text[:cut]
    return text.strip("；，、 ")


def _bullet_label(bullet: str) -> str:
    """A fix bullet is a long explanation; its part before the colon names the problem."""
    head = bullet.split("：")[0].split(":")[0].strip()
    return head if len(head) >= 6 else bullet


def _fit_sentence(text: str) -> str:
    """Cut one over-long item at a punctuation boundary instead of mid-word."""
    if len(text) <= OTA_NOTES_CHARS:
        return text
    window = text[:OTA_NOTES_CHARS]
    for mark in ("；", "，", "、", "："):
        at = window.rfind(mark)
        if at >= OTA_NOTES_CHARS // 2:
            return window[:at].rstrip("；，、 ")
    return window.rstrip("；，、 ") + "…"


def ota_notes_for(version: str) -> str:
    """Highlights of this fork's changelog entry for `version`, fitted to the device page."""
    path = changelog_path()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return OTA_NOTES
    start = None
    for index, line in enumerate(lines):
        if line.startswith("## ") and line[3:].strip().split(" ")[0] == version:
            start = index + 1
            break
    if start is None:
        return OTA_NOTES

    sections: dict[str, list[str]] = {}
    section = ""
    for line in lines[start:]:
        if line.startswith("## "):
            break
        if line.startswith("### "):
            section = line[4:].strip()
            continue
        if not line.startswith("- "):
            continue
        text = _clean_bullet(line[2:])
        if text:
            sections.setdefault(section, []).append(text)

    added = [b for key in ("新增", "Added", "变更", "Changed")
             for b in sections.get(key, [])]
    fixed = [b for key in ("修复", "Fixed") for b in sections.get(key, [])]

    picked: list[str] = []
    # The upstream merge line is what the device user asked to see.
    for bullet in added:
        if any(hint in bullet for hint in OTA_UPSTREAM_HINTS):
            picked.append(bullet)
            break
    labels = [_bullet_label(b) for b in fixed]
    if labels:
        picked.append("修复" + "、".join(labels))
    for bullet in added:
        if bullet not in picked and len(picked) < OTA_NOTES_BULLETS:
            picked.append(_bullet_label(bullet))
    if not picked:
        return OTA_NOTES

    notes = ""
    for item in picked[:OTA_NOTES_BULLETS]:
        candidate = f"{notes}；{item}" if notes else item
        if len(candidate) + 1 > OTA_NOTES_CHARS:  # leave room for the final full stop
            break
        notes = candidate
    if not notes:
        notes = _fit_sentence(picked[0])
    if not notes.endswith(("。", "…")):
        notes += "。"
    encoded = notes.encode("utf-8")
    if len(encoded) > OTA_NOTES_MAX:
        notes = encoded[:OTA_NOTES_MAX].decode("utf-8", "ignore")
    return notes
# 中文：网页刷机可回退的版本清单（最多四个），每版都从 firmware/ 里入库的镜像现场生成。
# English: Rollback choices for the web flasher (at most four); each bundle is generated
#          from a committed firmware/ image at deploy time.
RELEASES_CATALOG = DEFAULT_DESTINATION / "releases.json"
RELEASE_LIMIT = 4
RELEASE_DIRNAME = "releases"


def board_version(board_json: Path = BOARD_JSON) -> str:
    """Return the version declared by the board description."""
    return str(json.loads(board_json.read_text(encoding="utf-8"))["version"])


def read_app_description(image: bytes) -> dict[str, object]:
    """Parse the esp_app_desc_t fields the bundle has to agree with."""
    if len(image) < APP_DESC_MIN:
        raise SystemExit(f"application image is too small ({len(image)} bytes)")
    if image[0] != 0xE9:
        raise SystemExit("application image does not start with the ESP image magic 0xE9")
    chip = struct.unpack_from("<H", image, 12)[0]
    if chip != CHIP_ID:
        raise SystemExit(f"application image targets chip id {chip}, expected {CHIP_ID} (ESP32-S3)")
    magic = struct.unpack_from("<I", image, 32)[0]
    if magic != APP_DESC_MAGIC:
        raise SystemExit("application image has no esp_app_desc_t at offset 32")

    def field(offset: int, size: int) -> str:
        return image[offset:offset + size].split(b"\0")[0].decode("ascii", "replace")

    return {
        "version": field(48, 32),
        "project": field(80, 32),
        "time": field(112, 16),
        "date": field(128, 16),
    }


def _define(header: Path, name: str) -> str:
    """Last `#define <name>` value in ota_online.h, quotes stripped.

    中文：ota_online.h 里有金属板/Pico 两段常量，Pico 段在后面，因此取最后一次出现。
    English: ota_online.h carries both the Metalio and Pico constant blocks with Pico last,
          so the last occurrence is the one this fork's device compiles in.
    """
    text = header.read_text(encoding="utf-8")
    values = re.findall(rf"^#define\s+{re.escape(name)}\s+(\S+)", text, re.M)
    if not values:
        raise SystemExit(f"{header} declares no {name}")
    return values[-1].strip('"')


def firmware_ota(header: Path = OTA_HEADER) -> dict[str, str]:
    """The OTA identity the device builds in, read from the firmware header."""
    return {
        "project": _define(header, "PICO_OTA_PROJECT"),
        "board": _define(header, "PICO_OTA_BOARD"),
        "layout": _define(header, "PICO_OTA_LAYOUT"),
        "feed": _define(header, "PICO_OTA_ANKI_FEED_URL"),
        "image_max": _define(header, "PICO_OTA_IMAGE_MAX"),
    }


def ota_image_name(version: str) -> str:
    """Flat file name of the Anki OTA image at the site root."""
    return f"{OTA_IMAGE_PREFIX}{version}.bin"


def build_ota_manifest(version: str, parts: dict[str, bytes], notes: str | None = None) -> dict:
    """Compose `anki-update.json` from the real application bytes.

    中文：清单里的每个字段都要与设备端编译常量、实际镜像字节对得上，否则这里直接失败：
          版本 = 镜像内 esp_app_desc_t.version，size/sha256 = 镜像实际字节，
          project/board/layout = ota_online.h 的 PICO_OTA_*，url 过 pico_release_url_valid()。
    English: Every field must agree with the device's compile-time constants and the real
          image bytes, or this fails: version from esp_app_desc_t, size/sha256 from the
          bytes, project/board/layout from ota_online.h, and a url that passes
          pico_release_url_valid().
    """
    image = parts[APP_PART]
    desc = read_app_description(image)
    identity = firmware_ota()
    if desc["version"] != version:
        raise SystemExit(
            f"application image version {desc['version']!r} does not match {version!r}; "
            f"the OTA manifest must advertise the version inside the image")
    if desc["project"] != identity["project"]:
        raise SystemExit(
            f"application image project {desc['project']!r} is not {identity['project']!r}")
    limit = int(identity["image_max"], 0)
    if not 288 <= len(image) <= limit:
        raise SystemExit(f"application image is {len(image)} bytes, outside the OTA slot ({limit} bytes)")
    url = OTA_PREFIX + ota_image_name(version)
    if not release_url_valid(url):
        raise SystemExit(f"OTA url {url!r} would be rejected by pico_release_url_valid()")
    if identity["feed"] != OTA_PREFIX + OTA_MANIFEST:
        raise SystemExit(
            f"PICO_OTA_ANKI_FEED_URL is {identity['feed']!r}, but this tool publishes "
            f"{OTA_PREFIX + OTA_MANIFEST!r}; keep the firmware constant and the site path in step")
    if notes is None:
        notes = ota_notes_for(version)
    if len(notes.encode("utf-8")) > OTA_NOTES_MAX:
        raise SystemExit(f"OTA notes are {len(notes.encode('utf-8'))} bytes, over {OTA_NOTES_MAX}")
    return {
        "schema": 1,
        "version": version,
        "project": identity["project"],
        "board": identity["board"],
        "layout": identity["layout"],
        "minimum_base_version": OTA_MINIMUM_BASE,
        "url": url,
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "notes": notes,
    }


def stage_ota(destination: Path, version: str, parts: dict[str, bytes]) -> dict:
    """Publish `anki-update.json` and `Pico-anki-<version>.bin` at the site root."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    manifest = build_ota_manifest(version, parts)
    image = destination / ota_image_name(version)
    if not image.is_file() or image.read_bytes() != parts[APP_PART]:
        image.write_bytes(parts[APP_PART])
    text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    path = destination / OTA_MANIFEST
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
    verify_ota(destination, version, parts)
    return manifest


def verify_ota(destination: Path, version: str, parts: dict[str, bytes]) -> dict:
    """Check the published manifest against the bytes that are actually on the site."""
    destination = Path(destination)
    path = destination / OTA_MANIFEST
    if not path.is_file():
        raise SystemExit(f"no OTA manifest at {path}")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    expected = build_ota_manifest(version, parts)
    if manifest != expected:
        raise SystemExit(
            f"{path} does not match the staged bundle: {json.dumps(manifest, ensure_ascii=False)} "
            f"!= {json.dumps(expected, ensure_ascii=False)}")
    if not release_url_valid(manifest["url"]):
        raise SystemExit(f"OTA url {manifest['url']!r} fails pico_release_url_valid()")
    image = destination / ota_image_name(version)
    if not image.is_file():
        raise SystemExit(f"OTA image {image} is missing")
    data = image.read_bytes()
    if len(data) != manifest["size"] or hashlib.sha256(data).hexdigest() != manifest["sha256"]:
        raise SystemExit(f"OTA image {image} does not match the manifest size/sha256")
    if data != parts[APP_PART]:
        raise SystemExit(f"OTA image {image} is not the application image of the bundle")
    return manifest


def _package_offsets(manifest: dict) -> dict[str, int]:
    """Collect the offsets a build package declares, keyed by part file name."""
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        raise SystemExit("build package manifest must describe exactly one build")
    parts = builds[0].get("parts")
    if not isinstance(parts, list):
        raise SystemExit("build package manifest has no parts list")
    offsets: dict[str, int] = {}
    for part in parts:
        name = part.get("path")
        offset = part.get("offset")
        if not isinstance(name, str) or not isinstance(offset, int):
            raise SystemExit("build package manifest has a malformed part entry")
        offsets[name] = offset
    return offsets


def resolve_sources(
    package: Path | None,
    base_dir: Path | None,
    app_image: Path | None,
    version: str | None,
    use_default_package: bool = True,
) -> tuple[dict[str, bytes], str, Path | None, str]:
    """Load the four parts and the version they belong to.

    中文：取源优先级 = 本次构建包 → `firmware/anki-base/`（入库的基础部件）→ `--base-dir`。
          应用镜像优先级 = 构建包 → `--app-image` → `firmware/Read_Pico-<版本>.bin`。
          前两者都在版本库里，Pages 部署（没有构建产物）与本地跑出的是同一套字节。
    English: Base parts come from this build's package, else the tracked
          `firmware/anki-base/`, else `--base-dir`. The app image comes from the package,
          else `--app-image`, else the committed `firmware/Read_Pico-<version>.bin`. The
          first choices are in the repository, so a Pages deploy without any build output
          publishes exactly the bytes that were verified locally.
    """
    expected_version = version or board_version()
    chosen_package = package
    if chosen_package is None and use_default_package and DEFAULT_PACKAGE.is_dir():
        chosen_package = DEFAULT_PACKAGE

    sources: dict[str, Path] = {}
    notes: list[str] = []
    if chosen_package is not None:
        manifest_path = chosen_package / "manifest.json"
        if not manifest_path.is_file():
            raise SystemExit(f"build package has no manifest.json: {chosen_package}")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        package_version = manifest.get("version")
        # 中文：包里的版本号必须和目标版本一致，否则就是把旧 bin 贴上新版本号。
        # English: A stale package would silently publish old bytes under a new version.
        if package_version != expected_version:
            raise SystemExit(
                f"build package version {package_version!r} does not match {expected_version!r}")
        if manifest.get("project") not in (None, PROJECT):
            raise SystemExit(f"build package project is {manifest.get('project')!r}, expected {PROJECT!r}")
        # 中文：包里的偏移就是烧写计划；与站点校验表的偏移不一致时直接失败。
        # English: The package offsets are the flash plan; refuse to differ from the site table.
        declared = _package_offsets(manifest)
        for name in PART_ORDER:
            start = EXPECTED[name][0]
            if declared.get(name) != start:
                raise SystemExit(
                    f"build package offset for {name} is {declared.get(name)!r}, expected 0x{start:x}")
        for name in PART_ORDER:
            path = chosen_package / name
            if not path.is_file():
                raise SystemExit(f"build package is missing {name}: {chosen_package}")
            sources[name] = path
        notes.append(f"build package {chosen_package}")
    else:
        base: Path | None = None
        for candidate in (BASE_PARTS_DIR, base_dir, DEFAULT_BASE_DIR):
            if candidate is None:
                continue
            if all((candidate / name).is_file() for name in BASE_PARTS):
                base = candidate
                break
        if base is None:
            raise SystemExit(
                f"missing base parts ({', '.join(BASE_PARTS)}): neither the build package nor "
                f"{BASE_PARTS_DIR} nor --base-dir provides them; run the pico build first")
        for name in BASE_PARTS:
            sources[name] = base / name
        app = app_image if app_image is not None else DEFAULT_APP_DIR / f"{PROJECT}-{expected_version}.bin"
        if not app.is_file():
            raise SystemExit(
                f"missing application image {app}; build the pico firmware for {expected_version} first")
        sources[APP_PART] = app
        notes.append(f"base parts {base}")
        notes.append(f"app image {app}")

    parts = {name: sources[name].read_bytes() for name in PART_ORDER}
    for name, data in parts.items():
        if not data:
            raise SystemExit(f"part {name} is empty: {sources[name]}")
        start, end = EXPECTED[name]
        if len(data) > end - start:
            raise SystemExit(
                f"part {name} is {len(data)} bytes, larger than its {end - start} byte region")
    return parts, expected_version, chosen_package, " + ".join(notes)


def base_part_digests() -> dict[str, set[str]]:
    """SHA-256 of the base parts from every source a published bundle may use.

    中文：只有「本次构建包」与「firmware/anki-base/」是允许的来源。上游 Release 的
          bootloader 与我们的不同（日志等级裁到 ERROR 会重编引导程序），静默替换就等于
          发布的不是本地验证过的那套字节。
    English: Only this build's package and `firmware/anki-base/` are acceptable sources.
          The upstream Release bootloader differs from ours (trimming console logs to
          ERROR rebuilds it), and swapping it silently would publish bytes nobody verified.
    """
    allowed: dict[str, set[str]] = {name: set() for name in BASE_PARTS}
    candidates: list[Path] = [BASE_PARTS_DIR]
    if DEFAULT_PACKAGE.is_dir():
        candidates.append(DEFAULT_PACKAGE)
    for folder in candidates:
        for name in BASE_PARTS:
            path = folder / name
            if path.is_file():
                allowed[name].add(hashlib.sha256(path.read_bytes()).hexdigest())
    return allowed


def build_manifest(version: str, parts: dict[str, bytes]) -> dict:
    """Compose the esp-web-tools manifest from the real part bytes."""
    desc = read_app_description(parts[APP_PART])
    if desc["project"] != PROJECT:
        raise SystemExit(f"application image project is {desc['project']!r}, expected {PROJECT!r}")
    if desc["version"] != version:
        raise SystemExit(
            f"application image version {desc['version']!r} does not match {version!r}; "
            f"rebuild the firmware instead of relabelling an old image")
    entries = []
    for name in PART_ORDER:
        data = parts[name]
        entries.append({
            "path": name,
            "offset": EXPECTED[name][0],
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    return {
        # 中文：页面用 name 区分本 fork 的 Anki 版与官方站点入口。
        # English: The page uses `name` to tell this fork's Anki image from the official entry.
        "name": BUNDLE_NAME,
        "version": version,
        "board": BOARD,
        "project": PROJECT,
        # 中文：**极性反直觉，必须为 true**。install-dialog 的决策是
        #      `new_install_prompt_erase ? ASK_ERASE : _startInstall(true)`：
        #      - true  → 先弹「Erase device?」询问页，复选框**默认不勾选**，点 Next 才
        #                把勾选状态当作擦除标志；保持不勾 = 保留设置与阅读进度（本 fork 要的）。
        #      - false → 直接 `_startInstall(true)`，**立即整片 chip erase**，设置、阅读进度
        #                与 otadata 一起没掉，且不会询问。
        # English: The polarity is counter-intuitive and must be `true`. The dialog decides
        #      with `new_install_prompt_erase ? ASK_ERASE : _startInstall(true)`:
        #      true  → ask first; the "Erase device" checkbox starts **unchecked**, and only
        #              that checkbox becomes the erase flag. Leaving it unchecked preserves
        #              settings and reading progress (what this fork wants).
        #      false → `_startInstall(true)`, i.e. an immediate full-chip erase without asking,
        #              taking settings, reading progress and otadata with it.
        "new_install_prompt_erase": True,
        "anki_fork": {
            "fork": FORK_MARKER,
            "app_version": desc["version"],
            "build_date": desc["date"],
            "build_time": desc["time"],
        },
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": entries}],
    }


def write_bundle(destination: Path, manifest: dict, parts: dict[str, bytes]) -> None:
    """Write the binaries and the manifest; identical content is left untouched."""
    destination.mkdir(parents=True, exist_ok=True)
    for name in PART_ORDER:
        target = destination / name
        data = parts[name]
        if target.is_file() and target.read_bytes() == data:
            continue
        target.write_bytes(data)
    text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    manifest_path = destination / "manifest.json"
    if not manifest_path.is_file() or manifest_path.read_text(encoding="utf-8") != text:
        manifest_path.write_text(text, encoding="utf-8")


def verify(destination: Path, expected_version: str | None = None) -> dict:
    """Check an existing bundle: offsets, sizes, hashes and app description."""
    manifest_path = destination / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(f"no staged bundle at {destination}; run tools/stage_anki_flash.py first")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = expected_version or board_version()
    if manifest.get("version") != version:
        raise SystemExit(
            f"staged manifest version {manifest.get('version')!r} does not match {version!r}")
    if manifest.get("name") != BUNDLE_NAME:
        raise SystemExit(f"staged manifest name is {manifest.get('name')!r}, expected {BUNDLE_NAME!r}")
    if manifest.get("project") != PROJECT or manifest.get("board") != BOARD:
        raise SystemExit("staged manifest does not describe the pico board")
    # 中文：这里必须要求 true（询问页、复选框默认不勾 = 默认保留数据）；false 会触发
    #      install-dialog 的 `_startInstall(true)`，即不询问、直接整片擦除。写反就是丢数据。
    # English: `true` is mandatory here (ask first, checkbox unchecked by default = data
    #      preserved). `false` makes install-dialog call `_startInstall(true)`, i.e. a silent
    #      full-chip erase. Getting this backwards loses the user's data.
    if manifest.get("new_install_prompt_erase") is not True:
        raise SystemExit(
            "staged manifest must set new_install_prompt_erase true (ask first, keep data); "
            "false erases the whole chip without asking")
    fork = manifest.get("anki_fork")
    if not isinstance(fork, dict) or fork.get("fork") != FORK_MARKER:
        raise SystemExit("staged manifest is missing the Anki fork marker")
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        raise SystemExit("staged manifest must describe exactly one build")
    if builds[0].get("chipFamily") != CHIP_FAMILY:
        raise SystemExit(f"staged manifest chip family must be {CHIP_FAMILY}")
    parts = builds[0].get("parts")
    if not isinstance(parts, list) or len(parts) != len(PART_ORDER):
        raise SystemExit(f"staged manifest must list {len(PART_ORDER)} parts")

    binaries: dict[str, bytes] = {}
    for part in parts:
        name = part.get("path")
        if name not in PART_ORDER or name in binaries:
            raise SystemExit(f"unexpected flash part {name!r}")
        start, end = EXPECTED[name]
        if part.get("offset") != start:
            raise SystemExit(f"wrong offset for {name}: {part.get('offset')!r}, expected 0x{start:x}")
        path = destination / name
        if not path.is_file():
            raise SystemExit(f"staged bundle is missing {name}")
        data = path.read_bytes()
        if not data or len(data) > end - start:
            raise SystemExit(f"invalid size for {name}: {len(data)} bytes")
        if part.get("size") != len(data):
            raise SystemExit(f"size mismatch for {name}: manifest {part.get('size')}, file {len(data)}")
        digest = hashlib.sha256(data).hexdigest()
        if part.get("sha256") != digest:
            raise SystemExit(f"sha256 mismatch for {name}")
        binaries[name] = data
    missing = [name for name in PART_ORDER if name not in binaries]
    if missing:
        raise SystemExit(f"staged manifest is missing parts: {', '.join(missing)}")

    desc = read_app_description(binaries[APP_PART])
    if desc["version"] != version or desc["project"] != PROJECT:
        raise SystemExit(
            f"application image is {desc['project']} {desc['version']}, expected {PROJECT} {version}")
    if fork.get("app_version") != desc["version"]:
        raise SystemExit("staged manifest app_version does not match the application image")
    # 中文：基础部件必须来自「本次构建包」或入库的 firmware/anki-base，不接受别的来源。
    # English: Base parts must come from this build's package or the tracked
    #          firmware/anki-base; any other origin is refused, not silently accepted.
    allowed = base_part_digests()
    for name in BASE_PARTS:
        digest = hashlib.sha256(binaries[name]).hexdigest()
        if digest not in allowed[name]:
            raise SystemExit(
                f"{name} is not the verified one: SHA256 {digest} matches neither the build "
                f"package nor {BASE_PARTS_DIR}; refusing to publish a different base part")
    return manifest


def publish(staging: Path, destination: Path) -> None:
    """Move a verified staging bundle into place, one atomic rename per file.

    中文：每个文件只做一次 `os.replace`（同分区内的原子改名），所以目标目录里永远不会
          出现写了一半的 bin；不属于本 bundle 的文件（例如页面本身）原样保留。
    English: Each file is moved with a single same-filesystem `os.replace`, so the
          destination never shows a half-written binary, and files that are not part of
          the bundle (the page, for instance) are left untouched.
    """
    destination.mkdir(parents=True, exist_ok=True)
    for name in (*PART_ORDER, "manifest.json"):
        source = staging / name
        target = destination / name
        if target.is_file() and target.read_bytes() == source.read_bytes():
            # 中文：内容一样就不动它，重复运行的 mtime 保持稳定。
            # English: Identical content stays put, so repeated runs keep stable mtimes.
            source.unlink()
            continue
        os.replace(source, target)


def stage(destination: Path, package: Path | None = None, base_dir: Path | None = None,
          app_image: Path | None = None, version: str | None = None,
          use_default_package: bool = True, ota_dir: Path | None = None,
          releases: bool = True) -> dict:
    """Generate the bundle at `destination`, but only publish it once it verifies.

    中文：先在同级临时目录里生成 → 用 verify() 校验 → 通过后才逐文件原子替换到目标目录；
          任何一步失败都会删掉临时目录，目标目录保持原样，不会留下未通过闸门的半成品。
    English: Build into a sibling temporary directory, verify there, and only then move the
          files into the destination. Any failure removes the temporary directory and
          leaves the destination exactly as it was, so a failed gate never leaves a
          half-valid bundle behind.
    """
    parts, resolved_version, _used_package, provenance = resolve_sources(
        package, base_dir, app_image, version, use_default_package)
    manifest = build_manifest(resolved_version, parts)
    destination = Path(destination)
    # 中文：临时目录必须与目标同分区，os.replace 才是原子改名而不是跨盘复制。
    # English: The staging directory must share the destination's filesystem so that
    #          os.replace is an atomic rename instead of a cross-device copy.
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}-staging-",
                                    dir=destination.parent))
    try:
        write_bundle(staging, manifest, parts)
        verify(staging, resolved_version)
        publish(staging, destination)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    print(f"Anki bundle staged: {destination} (version {resolved_version}, from {provenance})")
    for part in manifest["builds"][0]["parts"]:
        print(f"  {part['path']}: {part['size']} bytes at 0x{part['offset']:x}, "
              f"SHA256 {part['sha256']}")
    fork = manifest["anki_fork"]
    print(f"  built {fork['build_date']} {fork['build_time']} (device time of the build)")
    # 中文：设备端 OTA 清单与镜像发布在站点根，和网页刷机用的 /anki/ 目录分开。
    # English: The device OTA manifest and image go to the site root, separate from /anki/.
    if ota_dir is not None:
        ota = stage_ota(Path(ota_dir), resolved_version, parts)
        print(f"OTA manifest staged: {Path(ota_dir) / OTA_MANIFEST} (version {ota['version']}, "
              f"{ota['size']} bytes, SHA256 {ota['sha256'][:16]}…)")
        print(f"  url {ota['url']}")
    # 中文：网页刷机的历史版本包按 flash/anki/releases.json 现场生成，仓库里只存清单。
    # English: Historical web-flash bundles are generated from flash/anki/releases.json, so
    #          only the catalog is tracked.
    if releases:
        for entry in stage_releases(destination, resolved_version):
            print(f"  release bundle {entry['version']}: {entry['manifest']}")
    return manifest


def read_release_catalog(catalog: Path = RELEASES_CATALOG) -> list[dict]:
    """Read and validate the rollback catalog the page renders as a version picker."""
    if not catalog.is_file():
        return []
    data = json.loads(catalog.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not isinstance(data.get("releases"), list):
        raise SystemExit(f"{catalog} must be {{'schema': 1, 'releases': [...]}}")
    releases = data["releases"]
    if not 1 <= len(releases) <= RELEASE_LIMIT:
        raise SystemExit(f"{catalog} lists {len(releases)} releases, allowed 1..{RELEASE_LIMIT}")
    seen: set[str] = set()
    for entry in releases:
        version = entry.get("version")
        if not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version):
            raise SystemExit(f"{catalog} has an invalid version: {version!r}")
        if version in seen:
            raise SystemExit(f"{catalog} repeats version {version}")
        seen.add(version)
        if entry.get("manifest") != f"{RELEASE_DIRNAME}/{version}/manifest.json":
            raise SystemExit(f"{catalog}: manifest for {version} must be "
                             f"{RELEASE_DIRNAME}/{version}/manifest.json")
        if not isinstance(entry.get("label"), str) or not entry["label"]:
            raise SystemExit(f"{catalog}: {version} needs a label")
    return releases


def stage_releases(destination: Path, current_version: str) -> list[dict]:
    """Generate one flashable bundle per catalog entry, newest first.

    中文：每版都用 `firmware/Read_Pico-<版本>.bin` + 入库的基础部件现场生成，并各自过一遍
          与当前版本相同的闸门（偏移/大小/SHA-256/版本对账）。基础部件统一用
          firmware/anki-base/，因此可回退包写的是同一套引导程序与分区表。
    English: Each version is assembled from its committed `firmware/Read_Pico-<version>.bin`
          plus the tracked base parts and must pass the same gate as the current build
          (offsets, sizes, SHA-256, version reconciliation). Base parts always come from
          firmware/anki-base/, so every rollback bundle writes the same bootloader and
          partition table.
    """
    entries = read_release_catalog()
    if not entries:
        return []
    if entries[0]["version"] != current_version:
        raise SystemExit(
            f"{RELEASES_CATALOG} must list {current_version} first; got {entries[0]['version']}")
    results = []
    for entry in entries:
        version = entry["version"]
        app = DEFAULT_APP_DIR / f"{PROJECT}-{version}.bin"
        if not app.is_file():
            raise SystemExit(f"missing rollback image {app}")
        folder = Path(destination) / RELEASE_DIRNAME / version
        # 中文：历史版本不使用本次构建包（包只属于当前版本），只用入库的基础部件。
        # English: Historical versions never use this build's package; tracked base parts only.
        stage(folder, None, None, app, version, use_default_package=False,
              ota_dir=None, releases=False)
        results.append({"version": version, "manifest": f"{RELEASE_DIRNAME}/{version}/manifest.json",
                        "label": entry["label"]})
    return results


def verify_releases(destination: Path) -> list[dict]:
    """Re-check every generated rollback bundle against its catalog entry."""
    entries = read_release_catalog()
    checked = []
    for entry in entries:
        version = entry["version"]
        folder = Path(destination) / RELEASE_DIRNAME / version
        manifest = verify(folder, version)
        if manifest["anki_fork"]["app_version"] != version:
            raise SystemExit(f"rollback bundle {version} advertises another version")
        checked.append({"version": version, "manifest": entry["manifest"]})
    return checked


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("destination", nargs="?", type=Path, default=DEFAULT_DESTINATION,
                        help="bundle directory (default: flash/anki)")
    parser.add_argument("--package", type=Path,
                        help="build package directory (default: build/boards/packages/pico when present)")
    parser.add_argument("--no-package", action="store_true",
                        help="ignore the build package and assemble from --base-dir/--app-image "
                             "(the same path CI takes when no build output is available)")
    parser.add_argument("--base-dir", type=Path,
                        help="last-resort source of bootloader/partitions/otadata "
                             "(default: flash/; only used when neither the build package nor "
                             "firmware/anki-base provides them)")
    parser.add_argument("--app-image", type=Path,
                        help="fallback application image (default: firmware/Read_Pico-<version>.bin)")
    parser.add_argument("--version", help="expected firmware version (default: boards/pico/board.json)")
    parser.add_argument("--ota-dir", type=Path,
                        help="site root that also receives anki-update.json and "
                             "Pico-anki-<version>.bin (the device OTA pair)")
    parser.add_argument("--check", action="store_true",
                        help="verify the staged bundle without writing anything")
    args = parser.parse_args(argv)

    expected_version = args.version or board_version()
    if args.check:
        manifest = verify(args.destination, expected_version)
        parts = manifest["builds"][0]["parts"]
        print(f"Anki bundle OK: {args.destination} (version {manifest['version']}, "
              f"{len(parts)} parts, data preserved)")
        for part in parts:
            print(f"  {part['path']}: {part['size']} bytes at 0x{part['offset']:x}, "
                  f"SHA256 {part['sha256']}")
        if args.ota_dir is not None:
            bundle = {name: (Path(args.destination) / name).read_bytes() for name in PART_ORDER}
            ota = verify_ota(args.ota_dir, expected_version, bundle)
            print(f"OTA manifest OK: {Path(args.ota_dir) / OTA_MANIFEST} (url {ota['url']}, "
                  f"{ota['size']} bytes, SHA256 {ota['sha256'][:16]}…)")
        for entry in verify_releases(args.destination):
            print(f"Rollback bundle OK: {entry['version']} ({entry['manifest']})")
        return 0
    stage(args.destination, None if args.no_package else args.package, args.base_dir,
          args.app_image, expected_version, use_default_package=not args.no_package,
          ota_dir=args.ota_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
