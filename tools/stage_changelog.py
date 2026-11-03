#!/usr/bin/env python3
"""Build the Anki flasher's changelog page from the two change logs.

中文：更新日志页 `/kiikoread-anki/changelog.html` 一页两段：
      ① **源仓库更新** —— 解析上游 `docs/CHANGELOG.md`，保留它自己的章节标题（版本号、日期、
         标题层级）与条目，只做 Markdown → HTML 的最小转换；
      ② **本仓库修复与新增** —— 解析 `docs/anki-changelog.md`（本 fork 的变更记录）。
      上游日志格式并不规整（章节标题有 `版本 / 日期`、`版本 — 日期`、`日期 · 代号` 几种，
      文末还有一段标题级别不同的附记），所以解析是保守的：认 `#`/`##` 为章节、`- ` 为条目、
      缩进续行并入上一条。**解析失败绝不让部署失败**——这时上游那一段降级为「日志解析失败，
      请看仓库里的 docs/CHANGELOG.md」，其余照常发布。
      本仓库的日志格式是固定的（版本标题 + 新增/修复/已知限制三组），并且第一条版本号必须
      等于 `boards/pico/board.json` 的 version，防的是「日志写了新版本而固件没跟上」。
English: The changelog page at `/kiikoread-anki/changelog.html` carries two sections:
      ① **upstream release notes** — parsed from upstream's `docs/CHANGELOG.md`, keeping its own
         section headings (version, date, heading level) and items, with only a minimal
         Markdown → HTML conversion;
      ② **this fork's fixes and additions** — parsed from `docs/anki-changelog.md`.
      Upstream's format is irregular (headings read `version / date`, `version — date`,
      `date · codename`, and the file ends with a differently-levelled appendix), so parsing is
      deliberately conservative: `#`/`##` start a section, `- ` starts an item, indented
      continuation lines fold into the item above. **A parse failure must never fail the
      deploy** — that section then degrades to "could not parse the upstream log, see
      docs/CHANGELOG.md in the repository" and the rest publishes normally.
      This fork's log has a fixed shape (version heading plus added/fixed/known-limits groups),
      and its first version must equal `version` in `boards/pico/board.json`, so the log can
      never advertise a version the firmware does not carry.
"""

from __future__ import annotations

import html
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UPSTREAM_CHANGELOG = ROOT / "docs" / "CHANGELOG.md"
FORK_CHANGELOG = ROOT / "docs" / "anki-changelog.md"
BOARD_JSON = ROOT / "boards" / "pico" / "board.json"

PAGE_NAME = "changelog.html"
UPSTREAM_TITLE = "源仓库更新"
FORK_TITLE = "本仓库修复与新增"
DEGRADED_NOTICE = "上游日志解析失败，请查看仓库 docs/CHANGELOG.md"

VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?$")
COMMENT_PATTERN = re.compile(r"<!--.*?-->", re.DOTALL)

# 中文：页顶导航与刷机页完全一致（四项、同一顺序），这样两个页面互跳时位置不会跳。
# English: The top navigation matches the flasher page exactly (same four items, same order), so
#          moving between the two pages never moves the links around.
SITE_ROOT_URL = "/"
UPSTREAM_FLASH_URL = "https://wegooo-cell.github.io/read-pico-reader/"
FLASHER_URL = "./"
CHANGELOG_URL = "./changelog.html"


def board_version(board_json: Path = BOARD_JSON) -> str:
    """Return the version the firmware declares."""
    return str(json.loads(board_json.read_text(encoding="utf-8"))["version"])


def _read(path: Path) -> str:
    return COMMENT_PATTERN.sub("", path.read_text(encoding="utf-8"))


def _split_title(title: str) -> tuple[str, str]:
    """Split a section heading into (version-ish name, date-ish suffix).

    中文：上游标题有三种写法（`版本 / 日期`、`版本 — 日期`、`日期 · 代号`），统一按第一个
          分隔符切开；切不出来就把整行当名字，日期留空。
    English: Upstream titles come in three shapes (`version / date`, `version — date`,
          `date · codename`); split on the first separator. When there is none, keep the whole
          line as the name and leave the date empty.
    """
    for separator in (" — ", " – ", " / ", " · ", "—", "–", "/"):
        if separator in title:
            left, right = title.split(separator, 1)
            return left.strip(), right.strip()
    return title.strip(), ""


def _slug(text: str, index: int) -> str:
    """Anchor id for one section: readable when the text is a version, unique regardless."""
    cleaned = re.sub(r"[^0-9A-Za-z._-]+", "-", text).strip("-")
    return f"{cleaned or 'entry'}-{index}"


def parse_upstream(text: str) -> list[dict]:
    """Parse upstream's changelog into version sections, conservatively.

    中文：`#`/`##` 开一节（版本），`###` 及更深是节内的分组小标题（上游用「### 阅读状态栏」
          这类中英双语小标题分组），`- ` 记一条，缩进续行并进上一条（上游每条都是「中文 +
          英文」两行，并成一条但不会丢内容）。没有条目的节（例如文件标题）被丢弃。
          分组是必要的：把 `###` 也当成新版本会让「阅读状态栏」这类小标题变成一条假版本记录。
    English: `#`/`##` opens a version section; `###` and deeper are group headings inside it
          (upstream groups items under bilingual sub-headings such as "### 阅读状态栏"). `- `
          records an item and indented continuation lines fold into the item above (upstream
          writes a Chinese line plus an English line per item; folding keeps both without
          losing anything). Sections without items — the file title, for instance — are
          dropped. Grouping matters: treating `###` as a new version would turn "阅读状态栏"
          into a fake release.
    """
    sections: list[dict] = []
    current: dict | None = None
    group: dict | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        if line.startswith("###"):
            if current is None:
                continue
            group = {"kind": line.lstrip("#").strip(), "items": []}
            current["groups"].append(group)
            continue
        if line.startswith("#"):
            title = line.lstrip("#").strip()
            current = None
            group = None
            if not title:
                continue
            group = {"kind": "", "items": []}
            current = {"title": title, "version": "", "date": "", "groups": [group]}
            current["version"], current["date"] = _split_title(title)
            sections.append(current)
            continue
        if current is None or group is None:
            continue
        if line.startswith(("- ", "* ")):
            item = line[2:].strip()
            if item:
                group["items"].append({"text": item, "note": ""})
        elif line[:1] in (" ", "\t") and group["items"]:
            previous = group["items"][-1]
            previous["note"] = f"{previous['note']} {line.strip()}".strip()
    parsed = []
    for index, section in enumerate(sections):
        if not any(entry["items"] for entry in section["groups"]):
            continue
        section["id"] = _slug(section["version"] or section["title"], index)
        parsed.append(section)
    return parsed


def parse_fork(text: str) -> list[dict]:
    """Parse this fork's changelog: `## <version> — <date>` plus `### 组` and `- 条目`."""
    entries: list[dict] = []
    current: dict | None = None
    group: dict | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        if line.startswith("## "):
            title = line[3:].strip()
            version, date = _split_title(title)
            current = {"title": title, "version": version, "date": date, "groups": []}
            entries.append(current)
            group = None
            continue
        if line.startswith("### "):
            if current is None:
                continue
            group = {"kind": line[4:].strip(), "items": []}
            current["groups"].append(group)
            continue
        if line.startswith("- ") and group is not None:
            item = line[2:].strip()
            if item:
                # 中文：与上游同样的条目结构（续行只在上面那套 Markdown 需要）。
                # English: Same item shape as upstream's (continuation lines only matter there).
                group["items"].append({"text": item, "note": ""})
            continue
        if line.startswith("#"):
            current, group = None, None
    return [entry for entry in entries if any(group["items"] for group in entry["groups"])]


PAGE_STYLE = """
  /* 中文：与刷机页同一套浅色版式，两页互跳不跳风格。 / Same light layout as the flasher page. */
  :root { color-scheme: light; }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 28px 20px 56px;
    background: #fff; color: #111;
    font: 16px/1.7 -apple-system, "Segoe UI", "Noto Sans SC", "Microsoft YaHei", sans-serif;
    -webkit-font-smoothing: antialiased;
  }
  main { max-width: 640px; margin: 0 auto; }
  a { color: #111; text-underline-offset: 3px; }
  nav.site {
    display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: baseline;
    max-width: 640px; margin: 0 auto 26px; padding-bottom: 12px;
    border-bottom: 1px solid #ececec; font-size: 13px;
  }
  nav.site a { color: #555; text-decoration: none; border-bottom: 1px solid transparent; padding-bottom: 2px; }
  nav.site a:hover { color: #111; border-color: #111; }
  nav.site a[aria-current="page"] { color: #111; font-weight: 600; border-color: #111; }
  .back { display: inline-block; font-size: 14px; color: #555; text-decoration: none; border-bottom: 1px solid #ccc; }
  .back:hover { color: #111; border-color: #111; }
  p.eyebrow {
    margin: 24px 0 6px; font-size: 12px; letter-spacing: .18em;
    text-transform: uppercase; color: #777;
  }
  h1 { margin: 0 0 14px; font-size: 28px; line-height: 1.25; letter-spacing: -.01em; }
  h1 span { color: #b9b9b9; }
  .lede { margin: 0 0 20px; color: #333; }
  .sections { margin: 0 0 30px; padding: 14px 16px; border: 1px solid #e2e2e2; border-radius: 10px; font-size: 14px; }
  .sections a { margin-right: 14px; }
  h2 { margin: 40px 0 6px; font-size: 21px; letter-spacing: -.01em; }
  h2 + p.section-note { margin: 0 0 18px; color: #777; font-size: 13px; }
  p.section-note a { margin-right: 12px; }
  article.entry { margin: 0 0 26px; padding: 0 0 22px; border-bottom: 1px solid #f0f0f0; }
  article.entry:last-child { border-bottom: 0; }
  article.entry h3 { margin: 0 0 8px; font-size: 16px; }
  article.entry h3 .date { color: #888; font-weight: 400; font-size: 13px; margin-left: 8px; }
  article.entry h4 { margin: 14px 0 4px; font-size: 13px; color: #555; letter-spacing: .04em; }
  ul { margin: 0; padding-left: 20px; color: #333; font-size: 14px; }
  li { margin: 6px 0; }
  li .note { display: block; color: #777; font-size: 13px; }
  .degraded { padding: 12px 14px; border: 1px dashed #ccc; border-radius: 8px; color: #555; font-size: 14px; }
  code { font: 13px/1.5 ui-monospace, "SFMono-Regular", Consolas, monospace; background: #f4f4f4; padding: 1px 5px; border-radius: 4px; }
  footer { margin-top: 40px; padding-top: 16px; border-top: 1px solid #eee; font-size: 13px; color: #777; }
"""


def _nav(current: str) -> str:
    """Top navigation shared with the flasher page; `current` marks this page."""
    items = (
        (SITE_ROOT_URL, "原版刷机", "site-root"),
        (FLASHER_URL, "kiikoread-anki 刷机", "flasher"),
        (UPSTREAM_FLASH_URL, "kiiko 在线刷机", "upstream-flash"),
        (CHANGELOG_URL, "更新日志", "changelog"),
    )
    links = []
    for href, label, key in items:
        current_attr = ' aria-current="page"' if key == current else ""
        external = ' rel="noopener noreferrer"' if href.startswith("http") else ""
        links.append(f'<a href="{href}"{current_attr}{external}>{label}</a>')
    return ('<nav class="site" aria-label="站点导航">\n  ' + "\n  ".join(links) + "\n</nav>")


def _inline(text: str) -> str:
    """Escape HTML, then turn `code` spans into real <code> elements."""
    return re.sub(r"`([^`]+)`", r"<code>\1</code>", html.escape(text))


def _items(items: list[dict]) -> str:
    lines = ["<ul>"]
    for item in items:
        note = f'<span class="note">{_inline(item["note"])}</span>' if item.get("note") else ""
        lines.append(f'  <li>{_inline(item["text"])}{note}</li>')
    lines.append("</ul>")
    return "\n".join(lines)


def _groups(groups: list[dict]) -> list[str]:
    """Render one entry's groups: a small heading when the log has one, then its list."""
    lines: list[str] = []
    for group in groups:
        if not group["items"]:
            continue
        if group["kind"]:
            lines.append(f"  <h4>{_inline(group['kind'])}</h4>")
        lines.append("  " + _items(group["items"]).replace("\n", "\n  "))
    return lines


def render(fork: list[dict], upstream: list[dict], degraded: bool) -> str:
    """Render the whole page as one self-contained document."""
    parts = [
        "<!doctype html>",
        '<html lang="zh-CN">',
        "<head>",
        '<meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width,initial-scale=1">',
        '<meta name="theme-color" content="#ffffff">',
        '<meta name="description" content="kiikoread · Anki 学习固件更新日志：源仓库（上游）的版本记录，以及本 fork 的修复与新增。">',
        "<title>kiikoread · Anki 学习固件 · 更新日志</title>",
        f"<style>{PAGE_STYLE}</style>",
        "</head>",
        "<body>",
        _nav("changelog"),
        "<main>",
        '<a class="back" href="./">← 返回刷机页</a>',
        '<p class="eyebrow">KIIKOREAD · ANKI FORK</p>',
        "<h1>更新日志<span>。</span></h1>",
        '<p class="lede">'
        "上半段是本 fork 自己的修复与新增，下半段是它所基于的源仓库（上游 kiikoread）的版本记录。"
        "刷机页与本页都由本仓库在部署时现场生成，版本号与本仓库的固件保持一致。"
        "</p>",
        '<div class="sections">',
        f'  <a href="#fork">{FORK_TITLE}</a>',
        f'  <a href="#upstream">{UPSTREAM_TITLE}</a>',
        "</div>",
        '<section id="fork">',
        f"<h2>{FORK_TITLE}</h2>",
        '<p class="section-note">本仓库（kiikoread · Anki fork）相对上游新增与修复的内容，来源：仓库的 <code>docs/anki-changelog.md</code>。</p>',
    ]
    for entry in fork:
        parts.append(f'<article class="entry" data-source="fork" data-version="{html.escape(entry["version"], quote=True)}">')
        date = f'<span class="date">{html.escape(entry["date"])}</span>' if entry["date"] else ""
        parts.append(f"  <h3>{html.escape(entry['version'])}{date}</h3>")
        parts.extend(_groups(entry["groups"]))
        parts.append("</article>")
    parts.append("</section>")
    parts.append('<section id="upstream">')
    parts.append(f"<h2>{UPSTREAM_TITLE}</h2>")
    parts.append('<p class="section-note">上游 kiikoread 的版本记录，保留其原有的版本号、日期与标题，来源：仓库的 <code>docs/CHANGELOG.md</code>。</p>')
    if degraded:
        parts.append(f'<p class="degraded" data-upstream-degraded="1">{html.escape(DEGRADED_NOTICE)}</p>')
    else:
        index = "".join(
            f'<a href="#{entry["id"]}">{_inline(entry["version"] or entry["title"])}</a>'
            for entry in upstream[:24])
        if index:
            parts.append(f'<p class="section-note">最近 {min(len(upstream), 24)} 条：{index}</p>')
        for entry in upstream:
            parts.append(f'<article class="entry" data-source="upstream" data-version="{html.escape(entry["version"] or entry["title"], quote=True)}" id="{entry["id"]}">')
            date = f'<span class="date">{html.escape(entry["date"])}</span>' if entry["date"] else ""
            heading = html.escape(entry["version"] or entry["title"]) + date
            parts.append(f"  <h3>{heading}</h3>")
            parts.extend(_groups(entry["groups"]))
            parts.append("</article>")
    parts.append("</section>")
    parts.extend([
        "<footer>",
        "  kiikoread · Anki fork · 第三方阅读固件，非官方发布。",
        "  本页由 <code>tools/stage_changelog.py</code> 在部署时生成：上游记录解析自 <code>docs/CHANGELOG.md</code>，"
        "本仓库记录来自 <code>docs/anki-changelog.md</code>，第一条版本号必须与固件版本一致才会发布。",
        "</footer>",
        "</main>",
        "</body>",
        "</html>",
        "",
    ])
    return "\n".join(parts)


def load_upstream(path: Path = UPSTREAM_CHANGELOG) -> tuple[list[dict], bool]:
    """Parse the upstream log; return (sections, degraded).

    中文：读不到或解析不出任何条目都只降级，不抛错——更新日志页不该因为上游的文件格式变了
          就把整个站点的部署卡住。
    English: An unreadable file or a parse that yields nothing only degrades. A changelog page
          must not block the whole deployment because upstream changed its file layout.
    """
    try:
        sections = parse_upstream(_read(path))
    except (OSError, UnicodeError, ValueError):
        return [], True
    return (sections, False) if sections else ([], True)


def verify(page: Path, expected_version: str, upstream: list[dict], degraded: bool) -> None:
    """Gate the generated page: both sections, both kinds of entries, versions in step."""
    assert page.is_file(), f"the changelog page was not written: {page}"
    text = page.read_text(encoding="utf-8")
    # 中文：两个分区标题必须都在（用户要看见「源仓库」与「本仓库」两段，而不是一坨混排）。
    # English: Both section headings must be there: readers must see "upstream" and "this fork"
    #      as two sections, not one undifferentiated list.
    assert 'id="fork"' in text and f">{FORK_TITLE}</h2>" in text, (
        f"the changelog page must carry the {FORK_TITLE} section")
    assert 'id="upstream"' in text and f">{UPSTREAM_TITLE}</h2>" in text, (
        f"the changelog page must carry the {UPSTREAM_TITLE} section")
    fork_entries = re.findall(r'<article class="entry" data-source="fork" data-version="([^"]+)"', text)
    assert fork_entries, "the changelog page lists none of this fork's entries"
    # 中文：页面第一条本仓日志必须就是固件当前版本，否则「日志写新、固件还是旧的」会一起上线。
    # English: The first fork entry must be the firmware's current version, or a page advertising
    #      a newer version than the firmware ships would go live.
    assert fork_entries[0] == expected_version, (
        f"the changelog page starts at {fork_entries[0]!r}, but the firmware is {expected_version!r}")
    if degraded:
        assert 'data-upstream-degraded="1"' in text, (
            "a degraded upstream section must say so on the page")
    else:
        upstream_entries = re.findall(r'<article class="entry" data-source="upstream" data-version="', text)
        assert upstream_entries, "the changelog page lists none of upstream's releases"
        assert len(upstream_entries) == len(upstream), (
            f"the page shows {len(upstream_entries)} upstream entries, {len(upstream)} were parsed")


def stage(destination: Path, upstream_changelog: Path = UPSTREAM_CHANGELOG,
          fork_changelog: Path = FORK_CHANGELOG, board_json: Path = BOARD_JSON) -> Path:
    """Write `changelog.html` into `destination` and gate it before anything is published."""
    expected_version = board_version(board_json)
    fork = parse_fork(_read(fork_changelog))
    assert fork, f"{fork_changelog} lists no dated entry"
    assert VERSION_PATTERN.match(fork[0]["version"]), (
        f"{fork_changelog}: the first entry must start with a version, got {fork[0]['version']!r}")
    # 中文：日志与固件版本脱节就拒绝发布，避免页面上写着新版本而刷机包还是旧的。
    # English: Refuse to publish when the log and the firmware disagree, so the page can never
    #      advertise a version the flashable bundle does not carry.
    assert fork[0]["version"] == expected_version, (
        f"{fork_changelog} starts at {fork[0]['version']!r} but boards/pico/board.json says "
        f"{expected_version!r}; update the changelog with the version bump")
    upstream, degraded = load_upstream(upstream_changelog)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    page = destination / PAGE_NAME
    page.write_text(render(fork, upstream, degraded), encoding="utf-8")
    verify(page, expected_version, upstream, degraded)
    print(f"Changelog page staged: {page} ({len(fork)} fork entries, "
          f"{len(upstream)} upstream entries{', degraded' if degraded else ''})")
    return page


if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "_site" / "kiikoread-anki"
    stage(target)
