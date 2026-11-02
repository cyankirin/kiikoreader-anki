#!/usr/bin/env python3
"""Validate the public kiikoread web-flashing bundle."""

from __future__ import annotations

import hashlib
import json
import sys
import struct
import re
from pathlib import Path


EXPECTED = {
    "bootloader.bin": (0x0, 0x8000),
    "partitions.bin": (0x8000, 0x10000),
    "firmware.bin": (0x10000, 0x410000),
    "ota_data_initial.bin": (0xD10000, 0xD12000),
}

# 中文：设备端 Anki 版 OTA 的站点路径。固件的 pico_release_url_valid() 只接受白名单前缀 +
#      站点根下的扁平 `.bin`，所以镜像必须放在站点根，不能放子目录、不能走 GitHub Release。
# English: Site layout for the device's Anki OTA channel. The firmware's
#      pico_release_url_valid() accepts only allowlisted prefixes plus a flat `.bin` at the
#      site root, so the image cannot live in a subdirectory or behind a GitHub Release.
ANKI_OTA_PREFIX = "https://cyankirin.github.io/kiikoreader-anki/"
ANKI_OTA_MANIFEST = "anki-update.json"
ANKI_OTA_BOARD = "RDP-G01-W"
ANKI_OTA_LAYOUT = "pico-dual-4m-v1"
ANKI_OTA_MINIMUM_BASE = "0.3.3-rc72"
ANKI_OTA_NOTES_MAX = 383          # char notes[384] on the device
ANKI_OTA_IMAGE_MAX = 0x400000     # PICO_OTA_IMAGE_MAX for the pico layout
ANKI_RELEASE_LIMIT = 4


def release_url_valid(url: str) -> bool:
    """Mirror `pico_release_url_valid()` in main/ota_release.c.

    中文：白名单前缀 + 站点根下的扁平 `.bin`（不含 `/`、`..`、反斜杠，字符集
          `[A-Za-z0-9._-]`，长度 < 256）。产出与发布两侧都跑这一条，线上不会出现设备
          必然拒绝的 URL。
    English: Allowlisted prefix plus a flat `.bin` at the site root (no `/`, no `..`, no
          backslash, charset `[A-Za-z0-9._-]`, shorter than 256 bytes). Both the producer and
          the deploy gate run this, so a URL the device would reject never goes live.
    """
    prefixes = ("https://kiikoread.com/", "https://wegooo-cell.github.io/read-pico-reader/",
                ANKI_OTA_PREFIX)
    if not url or len(url) >= 256:
        return False
    name = next((url[len(prefix):] for prefix in prefixes if url.startswith(prefix)), None)
    if not name or ".." in name or "/" in name or "\\" in name:
        return False
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        return False
    return len(name) > 4 and name.endswith(".bin")


def check_archived_releases(folder: Path, current_partitions: Path | None = None,
                            payload_folder: Path | None = None) -> list[dict]:
    # 历史版本只引用原正式固件，并核对每个完整刷机部件的大小、地址与校验值。
    # Historical choices use original releases; verify every base part's size, address and hash.
    catalog_path = folder / "releases.json"
    if not catalog_path.is_file():
        return []
    catalog = json.loads(catalog_path.read_text())
    assert catalog.get("schema") == 1 and isinstance(catalog.get("releases"), list)
    versions = set()
    for release in catalog["releases"]:
        version = release["version"]
        assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version)
        assert version not in versions, "duplicate archived version"
        versions.add(version)
        assert re.fullmatch(r"[0-9a-f]{40}", release["source_commit"]), "missing release provenance"
        assert release["manifest"] == f"releases/{version}/manifest.json"
        assert release["bin"] == f"Pico-update-{version}.bin"
        manifest_path = folder / release["manifest"]
        manifest = json.loads(manifest_path.read_text())
        assert manifest["version"] == version and manifest.get("new_install_prompt_erase") is True
        assert manifest["pico_ota"]["base_version"] == version
        assert manifest["pico_ota"]["layout"] == "pico-dual-4m-v1" and manifest["pico_ota"]["rollback"] is True
        builds = manifest["builds"]
        assert len(builds) == 1 and builds[0]["chipFamily"] == "ESP32-S3"
        paths = {name: name for name in EXPECTED}
        paths["firmware.bin"] = f"../../{release['bin']}"
        parts = builds[0]["parts"]
        assert len(parts) == len(EXPECTED) and set(release["files"]) == set(EXPECTED)
        seen = set()
        for part in parts:
            name = next((name for name, path in paths.items() if part["path"] == path), None)
            assert name and name not in seen, "unexpected historical part"
            seen.add(name)
            start, end = EXPECTED[name]
            assert part["offset"] == start, "wrong historical flash address"
            target = (manifest_path.parent / part["path"]).resolve()
            assert target.is_relative_to(folder.resolve()), "historical file escapes public bundle"
            # 中文：干净检出不保存Release大文件，应用从已校验下载的暂存区取原字节。
            # English: Clean checkouts keep large binaries in Releases; use original bytes hydrated into staging.
            if name == "firmware.bin" and payload_folder is not None:
                target = (payload_folder / release["bin"]).resolve()
                assert target.is_relative_to(payload_folder.resolve()), "historical payload escapes staging"
            data = target.read_bytes()
            assert data and len(data) <= end - start
            expected = release["files"][name]
            assert expected["size"] == len(data) and expected["sha256"] == hashlib.sha256(data).hexdigest(), (
                f"historical bytes changed: {version} {name}")
            if name == "firmware.bin":
                assert data[0] == 0xe9 and struct.unpack_from("<H", data, 12)[0] == 9
                assert struct.unpack_from("<I", data, 32)[0] == 0xabcd5432
                assert data[48:80].split(b"\0")[0].decode() == version
                assert data[80:112].split(b"\0")[0] == b"Read_Pico"
                assert b"PICO_HTTPS_OTA_V1" in data
            elif name == "partitions.bin":
                assert data == (current_partitions or folder / "partitions.bin").read_bytes(), "historical layout incompatible"
        print(f"Historical USB bundle verified: {version} (original bytes, preserve-data default)")
    assert (folder / "versions.js").is_file(), "missing version selector"
    return catalog["releases"]


def _partition_table(table: bytes) -> dict[str, tuple[int, int, int, int]]:
    """Decode the ESP32 partition table image into label -> (kind, subtype, address, size)."""
    entries: dict[str, tuple[int, int, int, int]] = {}
    for offset in range(0, len(table) - 31, 32):
        magic, kind, subtype, address, size, label, _ = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50aa: break
        entries[label.split(b"\0")[0].decode()] = (kind, subtype, address, size)
    return entries


def check(folder: Path) -> None:
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    assert manifest.get("new_install_prompt_erase") is True, "flasher must preserve user data"
    builds = manifest.get("builds")
    assert isinstance(builds, list) and len(builds) == 1
    assert builds[0].get("chipFamily") == "ESP32-S3"
    parts = builds[0].get("parts")
    assert isinstance(parts, list) and len(parts) == len(EXPECTED)
    seen: set[str] = set()
    for part in parts:
        name = part.get("path")
        assert name in EXPECTED and name not in seen, f"unexpected flash part: {name}"
        seen.add(name)
        start, end = EXPECTED[name]
        assert part.get("offset") == start, f"wrong offset for {name}"
        data = (folder / name).read_bytes()
        assert data and len(data) <= end - start, f"invalid size for {name}"
        if 'size' in part or 'sha256' in part:
            assert part.get('size') == len(data) and part.get('sha256') == hashlib.sha256(data).hexdigest(), f"manifest integrity mismatch: {name}"
        print(f"{name}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}")
    assert seen == set(EXPECTED)
    # 升级兼容性门禁：实际双槽分区、应用描述、基础包能力及升级 SHA 必须一致。
    # Upgrade gate: real slots, app description, base capability and upgrade SHA must agree.
    entries = _partition_table((folder / "partitions.bin").read_bytes())
    assert entries.get("ota_0") == (0, 16, 0x10000, 0x400000), "base OTA slot changed"
    assert entries.get("ota_1") == (0, 17, 0x910000, 0x400000), "inactive OTA slot changed"
    assert entries.get("otadata") == (1, 0, 0xd10000, 0x2000), "OTA selection metadata changed"
    assert entries.get("nvs") == (1, 2, 0x9000, 0x5000), "user settings partition changed"
    assert entries.get("storage") == (1, 129, 0x410000, 0x500000), "user storage partition changed"
    image = (folder / "firmware.bin").read_bytes()
    assert image[0] == 0xe9 and struct.unpack_from("<H", image, 12)[0] == 9, "wrong target chip"
    assert struct.unpack_from("<I", image, 32)[0] == 0xabcd5432, "missing app description"
    version = image[48:80].split(b"\0")[0].decode("ascii")
    project = image[80:112].split(b"\0")[0].decode("ascii")
    assert version == manifest["version"] and project == "Read_Pico", "base version mismatch"
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version), "invalid release version"
    capability = manifest.get("pico_ota", {})
    assert capability.get("layout") == "pico-dual-4m-v1"
    assert capability.get("base_version") == version and capability.get("rollback") is True
    assert capability.get("minimum_base_version") == "0.3.3-rc72"
    assert capability.get("online_ota_base_version") == "0.3.3-rc79"
    assert b"PICO_HTTPS_OTA_V1" in image, "base package cannot perform online upgrades"
    feed = json.loads((folder / "update.json").read_text())
    upgrade_name = f"Pico-update-{version}.bin"
    assert feed["schema"] == 1 and feed["version"] == version and feed["project"] == project
    assert feed["board"] == "RDP-G01-W" and feed["layout"] == capability["layout"]
    assert feed["minimum_base_version"] == capability["minimum_base_version"]
    assert feed["url"] in (f"https://wegooo-cell.github.io/read-pico-reader/{upgrade_name}", f"https://kiikoread.com/{upgrade_name}")
    assert feed["size"] == len(image) and feed["sha256"] == hashlib.sha256(image).hexdigest()
    assert (folder / "Pico-update.bin").read_bytes() == image
    assert (folder / upgrade_name).read_bytes() == image
    print(f"OTA base/TF/feed compatible: {version}, {capability['layout']}")
    assert (folder / "index.html").is_file()
    assert (folder / "vendor/web/install-button.js").is_file()
    assert (folder / "licenses/LUCIDE-ISC.txt").is_file()
    check_archived_releases(folder)


def _strip_comments(markup: str) -> str:
    """Return `markup` with HTML and JavaScript comments removed.

    中文：页面闸门必须匹配真正的语句，不能匹配注释里的字样 —— 页面的双语注释里就写着
          `./integrity.js` 与 `prepareManifest`。这里去掉 HTML 注释、块注释与整行 `//` 注释。
    English: Page gates must match real statements, never prose: the page's bilingual comments
         already mention `./integrity.js` and `prepareManifest`. HTML comments, block comments
         and whole-line `//` comments are removed before matching.
    """
    without_html = re.sub(r"<!--.*?-->", "", markup, flags=re.DOTALL)
    without_blocks = re.sub(r"/\*.*?\*/", "", without_html, flags=re.DOTALL)
    return re.sub(r"(?m)^[ \t]*//.*$", "", without_blocks)


def _check_anki_parts(folder: Path, manifest: dict) -> str:
    """Check one Anki bundle's manifest, bytes, layout and app identity; return its version.

    中文：当前版本与可回退版本共用这一套检查，因此每个版本都必须独立过闸门。
    English: The current bundle and every rollback bundle share this check, so each version
          passes the gate on its own.
    """
    assert manifest.get("new_install_prompt_erase") is True, (
        "Anki bundle must set new_install_prompt_erase true (ask first, checkbox unchecked = "
        "keep data); false erases the whole chip without asking")
    builds = manifest.get("builds")
    assert isinstance(builds, list) and len(builds) == 1, "one build expected"
    assert builds[0].get("chipFamily") == "ESP32-S3", "wrong target chip"
    parts = builds[0].get("parts")
    assert isinstance(parts, list) and len(parts) == len(EXPECTED), "wrong number of flash parts"
    seen: set[str] = set()
    for part in parts:
        name = part.get("path")
        assert name in EXPECTED and name not in seen, f"unexpected flash part: {name}"
        seen.add(name)
        start, end = EXPECTED[name]
        assert part.get("offset") == start, f"wrong offset for {name}"
        data = (folder / name).read_bytes()
        assert data and len(data) <= end - start, f"invalid size for {name}"
        assert part.get("size") == len(data), f"declared size mismatch: {name}"
        assert part.get("sha256") == hashlib.sha256(data).hexdigest(), f"manifest integrity mismatch: {name}"
    assert seen == set(EXPECTED), "missing flash parts"
    entries = _partition_table((folder / "partitions.bin").read_bytes())
    assert entries.get("ota_0") == (0, 16, 0x10000, 0x400000), "app slot changed"
    assert entries.get("ota_1") == (0, 17, 0x910000, 0x400000), "inactive OTA slot changed"
    assert entries.get("otadata") == (1, 0, 0xd10000, 0x2000), "OTA selection metadata changed"
    assert entries.get("nvs") == (1, 2, 0x9000, 0x5000), "user settings partition changed"
    assert entries.get("storage") == (1, 129, 0x410000, 0x500000), "user storage partition changed"
    image = (folder / "firmware.bin").read_bytes()
    assert image[0] == 0xe9 and struct.unpack_from("<H", image, 12)[0] == 9, "wrong target chip"
    assert struct.unpack_from("<I", image, 32)[0] == 0xabcd5432, "missing app description"
    version = image[48:80].split(b"\0")[0].decode("ascii")
    project = image[80:112].split(b"\0")[0].decode("ascii")
    assert version == manifest["version"] and project == "Read_Pico", "bundle version mismatch"
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version), "invalid release version"
    return version


def check_anki_bundle(folder: Path) -> dict:
    """Validate the fork's Anki web-flash bundle: same layout, user data preserved.

    中文：本 fork 的 Anki 版入口写同一套双槽布局，并且**必须**保留用户数据。注意
         `new_install_prompt_erase` 的极性反直觉：`true` 才会先弹「Erase device?」询问页，
         复选框默认不勾选 → 默认保留；`false` 是 install-dialog 里的「不询问、直接
         `_startInstall(true)`」，也就是立即整片 chip erase。偏移、大小、哈希、分区表与
         应用描述的校验与官网包同样严格。
    English: The fork's Anki entry writes the same dual-slot layout and must preserve user
         data. The `new_install_prompt_erase` polarity is counter-intuitive: `true` shows the
         "Erase device?" step whose checkbox starts unchecked, so the default is to keep data;
         `false` means install-dialog calls `_startInstall(true)` without asking, i.e. an
         immediate full-chip erase. Every offset, size, hash, partition-table and
         app-description check stays as strict as the official bundle.
    """
    manifest_path = folder / "manifest.json"
    assert manifest_path.is_file(), "missing Anki flash manifest"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = _check_anki_parts(folder, manifest)
    # 中文：页面承诺「刷机前逐个核对 SHA-256」，就必须真的挂上浏览器端校验：页面要加载
    #       integrity.js 并由它实现 prepareManifest，且校验模块里的偏移/容量不能与烧写计划漂移。
    # English: The page promises a per-part SHA-256 check before flashing, so it must actually
    #      wire one: the page loads integrity.js, that module implements the prepareManifest
    #      hook, and its offsets/capacity must not drift from the flash plan.
    verifier_path = folder / "integrity.js"
    assert verifier_path.is_file(), "missing the Anki browser-side verifier (integrity.js)"
    page = (folder / "index.html").read_text(encoding="utf-8")
    # 中文：**按语句匹配、且先去掉注释**。页面的双语注释里也写着 `./integrity.js` 与
    #      `prepareManifest`，用子串匹配会让「import 指向不存在的模块」（注释里仍留着正确
    #      路径）或「占位钩子被删掉」（其它地方仍有这个词）照样放行 —— 页面坏了而 CI 是绿的。
    # English: Match statements, on a comment-free view. The page's bilingual comments also
    #      mention `./integrity.js` and `prepareManifest`, so a substring test would pass a page
    #      whose import points at a missing module (the right path is still in a comment) or
    #      whose placeholder was deleted (the word survives elsewhere) — broken page, green CI.
    code = _strip_comments(page)
    assert re.search(r"import\s*\{[^}]*\bprepareBundle\b[^}]*\}\s*from\s*['\"]\./integrity\.js['\"]",
                     code), "Anki page must import prepareBundle from ./integrity.js"
    # 中文：install-button.js 把 prepareManifest 当可选钩子，模块加载失败就会静默跳过校验；
    #      页面必须带上只能抛错的占位钩子，否则 404/MIME/缓存/CSP 一失手就是写入未校验字节。
    # English: install-button.js treats prepareManifest as optional, so a failed module load
    #      would silently skip verification. The page must carry the throw-only placeholder;
    #      otherwise a 404/MIME/cache/CSP miss writes unverified bytes.
    assert re.search(r"button\.prepareManifest\s*=\s*window\.__ankiVerifyUnavailable\b", code), (
        "Anki page must install the throw-only placeholder (button.prepareManifest = "
        "window.__ankiVerifyUnavailable) when integrity.js does not load")
    # 中文：占位必须被真正的校验函数覆盖，否则页面永远只有「必然抛错」的占位。
    # English: The placeholder has to be replaced by the real check, or the page would only
    #      ever throw.
    assert re.search(r"installButton\.prepareManifest\s*=\s*async", code), (
        "Anki page must replace the placeholder with the real verification hook")
    # 中文：同样只认真正的代码：把某一项挪进注释里也不能满足下面的偏移/容量比对。
    # English: Same rule for the verifier: a part moved into a comment must not satisfy the
    #      offset/capacity comparison below.
    verifier = _strip_comments(verifier_path.read_text(encoding="utf-8"))
    for name, (start, end) in EXPECTED.items():
        located = re.search(
            rf"name: '{re.escape(name)}', offset: (0x[0-9a-fA-F]+), limit: (0x[0-9a-fA-F]+)",
            verifier)
        assert located, f"browser verifier does not declare {name}"
        assert int(located.group(1), 16) == start, f"browser verifier offset for {name} drifted"
        assert int(located.group(2), 16) == end - start, f"browser verifier capacity for {name} drifted"
    # 中文：版本选择必须是真代码：页面上有 <select id="version">、真的读了 releases.json、
    #       并在切换时改写安装按钮的 manifest，同时用当前选择驱动校验钩子。
    # English: The version picker must be real code: a <select id="version"> on the page, an
    #      actual releases.json fetch, the install button's manifest switched on change, and
    #      the verify hook driven by the current selection.
    assert re.search(r"<select[^>]*\bid=\"version\"", code), (
        "Anki page must expose a <select id=\"version\"> picker")
    assert re.search(r"fetch\(\s*['\"]\./releases\.json['\"]", code), (
        "Anki page must load ./releases.json for the picker")
    assert re.search(r"setAttribute\(\s*['\"]manifest['\"]", code), (
        "Anki page must point the install button at the selected manifest")
    assert re.search(r"currentManifest\(", code), (
        "Anki page must resolve the manifest through currentManifest() so the verifier follows the picker")
    for name in ("bootloader.bin", "partitions.bin", "firmware.bin", "ota_data_initial.bin"):
        data = (folder / name).read_bytes()
        print(f"anki/{name}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}")
    print(f"Anki web bundle verified: {version} (data preserved, dual-slot layout unchanged)")
    return manifest


def check_anki_ota(site: Path, anki: Path | None = None) -> dict:
    """Validate the device OTA pair published at the site root.

    中文：设备只接受站点根下的扁平 `.bin`，所以清单里的 url、清单本身的位置、以及那个 bin
          必须三者一致；bin 还必须与 `/anki/` 里烧写的应用镜像**同字节**，否则同一版本
          会出现「网页刷的」和「OTA 推的」两套固件。
    English: The device only accepts a flat `.bin` at the site root, so the manifest's url,
          the manifest's own location and that binary must agree. The binary must also be
          byte-identical to the application image flashed from `/anki/`, or one version would
          ship two different firmwares depending on the update path.
    """
    manifest_path = site / ANKI_OTA_MANIFEST
    assert manifest_path.is_file(), f"missing the Anki OTA manifest at {manifest_path}"
    feed = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = feed.get("version")
    assert feed.get("schema") == 1, "OTA manifest schema must be 1"
    assert isinstance(version, str) and re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version)
    assert feed.get("project") == "Read_Pico", "OTA manifest project must be Read_Pico"
    assert feed.get("board") == ANKI_OTA_BOARD, "OTA manifest board drifted"
    assert feed.get("layout") == ANKI_OTA_LAYOUT, "OTA manifest layout drifted"
    assert feed.get("minimum_base_version") == ANKI_OTA_MINIMUM_BASE, "minimum base version drifted"
    assert feed.get("url") == f"{ANKI_OTA_PREFIX}Pico-anki-{version}.bin", (
        "OTA url must be the flat site-root image name, not a Release or subdirectory URL")
    assert release_url_valid(feed["url"]), "OTA url would be rejected by pico_release_url_valid()"
    notes = feed.get("notes")
    assert isinstance(notes, str) and notes, "OTA manifest needs notes"
    assert len(notes.encode("utf-8")) <= ANKI_OTA_NOTES_MAX, "OTA notes overflow char notes[384]"
    image = site / f"Pico-anki-{version}.bin"
    assert image.is_file(), f"missing the OTA image at the site root: {image}"
    data = image.read_bytes()
    assert 288 <= len(data) <= ANKI_OTA_IMAGE_MAX, "OTA image does not fit the app slot"
    assert feed.get("size") == len(data), "OTA manifest size does not match the site-root image"
    assert feed.get("sha256") == hashlib.sha256(data).hexdigest(), (
        "OTA manifest sha256 does not match the site-root image")
    assert data[48:80].split(b"\0")[0].decode("ascii") == version, "OTA image version mismatch"
    if anki is not None:
        assert (anki / "firmware.bin").read_bytes() == data, (
            "the OTA image and the /anki/ application image must be the same bytes")
    print(f"Anki OTA feed verified: {version} -> {feed['url']}")
    return feed


def check_anki_releases(folder: Path, current_version: str) -> list[dict]:
    """Validate every rollback bundle the version picker offers.

    中文：每个版本都是一次完整的刷机：四件套、偏移、大小、SHA-256、分区表与镜像内版本
          逐项对齐，且第一项必须是当前版本（默认选项）。目录不存在时返回空表。
    English: Every version is a full flash: all four parts, offsets, sizes, SHA-256, the
          partition table and the version inside the image, all checked; the first entry must
          be the current version (the default choice). Returns an empty list when absent.
    """
    catalog_path = folder / "releases.json"
    if not catalog_path.is_file():
        return []
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    assert catalog.get("schema") == 1 and isinstance(catalog.get("releases"), list)
    releases = catalog["releases"]
    assert 1 <= len(releases) <= ANKI_RELEASE_LIMIT, "version picker allows one to four builds"
    seen: set[str] = set()
    for release in releases:
        version = release.get("version")
        assert isinstance(version, str) and re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?", version)
        assert version not in seen, f"duplicate offered version {version}"
        seen.add(version)
        assert release.get("manifest") == f"releases/{version}/manifest.json", "manifest path drifted"
        assert isinstance(release.get("label"), str) and release["label"], "missing version label"
        bundle = folder / "releases" / version
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        assert _check_anki_parts(bundle, manifest) == version, f"rollback bundle {version} mislabelled"
        print(f"anki/releases/{version}: four parts verified, data preserved")
    assert releases[0]["version"] == current_version, (
        "the version picker must default to the current build")
    print(f"Anki version picker verified: {len(releases)} builds, current {current_version}")
    return releases


if __name__ == "__main__":
    check(Path(sys.argv[1]) if len(sys.argv) > 1 else Path("flash"))
