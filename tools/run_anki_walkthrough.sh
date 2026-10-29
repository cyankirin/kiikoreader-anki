#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
#
# Anki 端到端走查的宿主运行脚本：导入→列表→载入→会话→渲染→评分→落盘。
# 需要一个真实卡组做输入，所以卡组路径必须由调用方给出。
# Host runner for the Anki end-to-end walkthrough (import, list, load, session,
# render, grade, persist). It needs a real deck, so the path is always an
# argument — the script has no built-in default deck.
#
# 用法 / Usage:
#   bash tools/run_anki_walkthrough.sh <deck.apkg>
#
# 在仓库根目录运行；产物落在 build/anki-tests/。
# Run from the repository root; artifacts stay under build/anki-tests/.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") <deck.apkg>" >&2
}

if [ "$#" -lt 1 ] || [ -z "${1:-}" ]; then
    usage
    exit 2
fi

apkg=$1
if [ ! -f "$apkg" ]; then
    usage
    echo "error: no such .apkg: $apkg" >&2
    exit 2
fi

mkdir -p build/anki-tests
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fno-omit-frame-pointer)
# CI 与常规 Linux 宿主带 ASan/UBSan；运行时缺失时退化为普通构建（部分 Windows 宿主）。
# CI and regular Linux hosts carry ASan/UBSan; fall back to a plain build when the
# runtime is missing (some Windows hosts).
if echo 'int main(void){return 0;}' | gcc -fsanitize=address,undefined -x c -o /dev/null - 2>/dev/null; then
    flags+=(-fsanitize=address,undefined)
fi
gcc "${flags[@]}" -Imain/anki -Imain/book -Imain -Itools/anki_stubs \
    tools/anki_walkthrough.c \
    main/anki/anki_json.c main/anki/anki_sqlite.c main/anki/anki_apkg.c \
    main/anki/anki_render.c main/anki/anki_store.c \
    main/book/zip_reader.c main/book/html_text.c \
    tools/anki_stubs/miniz_tinfl.c \
    -DANKI_DIR="\"build/anki-tests/walkthrough/anki\"" \
    -o build/anki-tests/anki_walkthrough
# Windows 宿主上 gcc 会在 -o 目标后补 .exe；按实际产物运行，避免误跑上一次的
# 旧二进制。/ On Windows hosts gcc appends .exe to the -o target; run the
# artifact actually produced so a stale leftover binary is not executed.
test_bin=build/anki-tests/anki_walkthrough
if [ -x build/anki-tests/anki_walkthrough.exe ]; then test_bin=build/anki-tests/anki_walkthrough.exe; fi
"$test_bin" "$apkg"
