#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""The OTA manifest notes must describe the actual release, including upstream changes.

The device upgrade page used to show one fixed sentence. It now takes highlights from
this fork's changelog, with the upstream-merge bullet first, and must still fit the
device-side char[384] parser. This probe locks that contract so a future refactor
cannot silently bring back the fixed line or a half-cut sentence.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stage_anki_flash as flash  # noqa: E402

version = json.loads((ROOT / "boards/pico/board.json").read_text(encoding="utf-8"))["version"]
notes = flash.ota_notes_for(version)

assert notes != flash.OTA_NOTES, "notes fell back to the fixed one-liner"
assert any(hint in notes for hint in ("上游", "官方", "合并")), \
    f"notes do not describe upstream changes: {notes}"
assert "修复" in notes, f"notes do not mention the fixes: {notes}"
assert len(notes.encode("utf-8")) <= flash.OTA_NOTES_MAX, "notes exceed the device parser limit"
assert notes.endswith(("。", "…")), f"notes have no ending: {notes}"
assert "…" not in notes[:-1], f"notes were cut mid-sentence: {notes}"
assert flash.ota_notes_for("9.9.9-not-a-release") == flash.OTA_NOTES, \
    "an unknown version must fall back to the fixed line"

print(f"PASS: {version} OTA notes carry upstream changes and fixes "
      f"({len(notes.encode('utf-8'))}/{flash.OTA_NOTES_MAX} bytes)")
