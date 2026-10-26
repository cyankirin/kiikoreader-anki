#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
#
# 排版探针的变异验证：故意打断真机排版代码里的禁则分类与悬挂缩进，探针必须
# 失败。探针若在变异下仍然通过，说明它拿被测函数当判据，断言无效。
# Mutation check for the layout probe: break the real layout code on purpose
# (the break rules and the hanging indent) and the probe must fail. A probe
# that still passes under mutation is judging itself, not the code.
#
# 在仓库根目录运行；产物落在 build/。/ Run from the repository root; artifacts
# stay under build/.
set -euo pipefail
mkdir -p build/anki-tests/mutants
flags=(-std=c11 -Wall -Wextra -Werror -g)

mutate() {  # $1 = 变异名, $2 = python 替换脚本
    local name="$1"
    local script="$2"
    local dir="build/anki-tests/mutants/$name"
    rm -rf "$dir"
    mkdir -p "$dir/apps"
    cp main/apps/anki_layout.h "$dir/apps/anki_layout.h"
    python3 - "$dir/apps/anki_layout.h" "$script" <<'PY'
import io, sys
path, mode = sys.argv[1], sys.argv[2]
with io.open(path, "r", encoding="utf-8", newline="") as f:
    text = f.read()
if mode == "drop_close":
    text = text.replace("case 0x3001: case 0x3002:", "case 0x3001:", 1)
elif mode == "drop_open":
    text = text.replace("case 0x3008: case 0x300A:", "case 0x3008:", 1)
elif mode == "invert_close":
    text = text.replace("        case 0x2026: case 0x2030:                 // …‰\n            return true;",
                        "        case 0x2026: case 0x2030:                 // …‰\n            return false;", 1)
elif mode == "no_hang":
    text = text.replace("                hang = ui_text_fixed_width_px(px, prefix);",
                        "                hang = 0; (void)prefix;", 1)
elif mode == "first_line_only_invert":
    # 退回「只有首行反色」的旧行为 / revert to "first line only is inverted"
    text = text.replace("        bool seg_first = true;\n        while (seg_len > 0",
                        "        bool seg_first = true;\n        int seg_line = 0;\n"
                        "        while (seg_len > 0", 1)
    text = text.replace("            L->line_invert[L->line_count] = seg_invert;",
                        "            L->line_invert[L->line_count] = seg_invert && seg_line == 0;\n"
                        "            ++seg_line;", 1)
elif mode == "no_cjk_rules":
    text = text.replace("    const size_t original = cut;", "    const size_t original = cut;\n    (void)original;", 1)
    text = text.replace("            if (cjk_opening(cp)) {", "            if (false && cjk_opening(cp)) {", 1)
    text = text.replace("            if (used > 0 && cjk_closing(cp)) {",
                        "            if (false && used > 0 && cjk_closing(cp)) {", 1)
else:
    raise SystemExit("unknown mutation " + mode)
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(text)
PY
    if ! gcc "${flags[@]}" -I"$dir/apps" -Imain/apps tools/anki_layout_probe.c \
            -o "$dir/probe" 2>"$dir/build.log"; then
        echo "MUTATION $name: BUILD FAILED"
        tail -5 "$dir/build.log"
        return 1
    fi
    if "$dir/probe" >"$dir/run.log" 2>&1; then
        echo "MUTATION $name: PROBE STILL PASSED (probe is not effective)"
        return 1
    fi
    echo "MUTATION $name: probe failed as expected -> $(grep -c '^FAIL' "$dir/run.log") failing checks"
    return 0
}

failures=0
mutate drop_close drop_close || failures=$((failures + 1))
mutate drop_open drop_open || failures=$((failures + 1))
mutate invert_close invert_close || failures=$((failures + 1))
mutate no_hang no_hang || failures=$((failures + 1))
mutate no_cjk_rules no_cjk_rules || failures=$((failures + 1))
mutate first_line_only_invert first_line_only_invert || failures=$((failures + 1))

if [ "$failures" -ne 0 ]; then
    echo "anki_layout_probe mutations: FAIL ($failures mutation(s) survived)"
    exit 1
fi
echo "anki_layout_probe mutations: PASS (every mutation was caught)"
