#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：锁住 epdiy 的"驱动范围 == 回写范围"契约。

rc105（96b96e8）把 Pico 的驱动列裁剪到请求矩形，却仍按矩形回写；面板在跳列驱动下
逐次翻页累积电荷，阅读页时间长了就会出现成片黑色。rc102 没有这个问题。
这个探针做源码级断言，防止裁剪逻辑被再次引入。

English: Pin the epdiy "drive span == commit span" contract. rc105 masked the Pico
drive to the requested rectangle while still committing it; column-skipped driving
accumulated charge into black patches after long reading. rc102 was clean. This is a
source-level guard so the masking cannot come back unnoticed.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HIGHLEVEL = ROOT / "components/epdiy/src/highlevel.c"
RENDER = ROOT / "boards/pico/src/components/epdiy/src/render.c"


def fail(message: str) -> None:
    print("FAIL: " + message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    text = HIGHLEVEL.read_text(encoding="utf-8")
    start = text.index("static enum EpdDrawError hl_update_area(")
    end = text.index("\n}\n", start)
    body = text[start:end]

    # 回写范围必须来自差分对齐段，两个板一致。
    if not re.search(r"epd_difference_column_range\(area, &x_start, &x_stop\);", body):
        fail("hl_update_area must take the commit span from epd_difference_column_range(area)")
    if re.search(r"x_start\s*=\s*area\.x\s*;", body):
        fail("hl_update_area must not commit the raw requested rectangle")

    # 驱动列不得按请求矩形裁剪：跳列驱动会在面板上累积成片黑。
    # 只拦"清零裁剪"（memset(dirty_columns, 0, …)）；force_full 的 0xFF 置脏是合法用法。
    if re.search(r"memset\(\s*state->dirty_columns\s*,\s*0\s*,", body):
        fail("hl_update_area must not mask dirty_columns; drive the aligned segment as rc102 did")

    # 面板驱动范围与回写范围必须是同一段（render 侧只按段内的 col_dirtyness 扫描）。
    render = RENDER.read_text(encoding="utf-8")
    if "epd_difference_column_range(crop_to, &x_start, &x_stop);" not in render:
        fail("render must keep the aligned segment as the single source of the scan range")

    # 段内才置脏：段外的列既不驱动也不能回写。
    if "memset(col_dirtyness, 0, fb_width / 2);" not in render:
        fail("render must rebuild col_dirtyness from scratch on every difference")

    print("PASS: drive and commit cover the same aligned segment (rc102 contract)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
