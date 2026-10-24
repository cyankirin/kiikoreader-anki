#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# Anki 导入、渲染与存储的宿主测试。/ Host tests for Anki import, rendering and storage.
# 在仓库根目录运行；产物仅在 build。/ Run from the repository root; artifacts stay in build.
set -euo pipefail
mkdir -p build/anki-tests
python3 tools/gen_anki_fixtures.py
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fno-omit-frame-pointer)
# CI 与常规 Linux 宿主带 ASan/UBSan；运行时缺失时退化为普通构建（部分 Windows 宿主）。
# CI and regular Linux hosts carry ASan/UBSan; fall back to a plain build when the
# runtime is missing (some Windows hosts).
if echo 'int main(void){return 0;}' | gcc -fsanitize=address,undefined -x c -o /dev/null - 2>/dev/null; then
    flags+=(-fsanitize=address,undefined)
fi
gcc "${flags[@]}" -Imain/anki -Imain/book -Imain -Itools/anki_stubs \
    tools/anki_host_test.c \
    main/anki/anki_json.c main/anki/anki_sqlite.c main/anki/anki_apkg.c \
    main/anki/anki_render.c main/anki/anki_store.c \
    main/book/zip_reader.c main/book/html_text.c \
    tools/anki_stubs/miniz_tinfl.c \
    -DANKI_DIR="\"build/anki-tests/flash/anki\"" \
    -o build/anki-tests/anki
# Windows 宿主上 gcc 会在 -o 目标后补 .exe；按实际产物运行，避免误跑
# 上一次遗留的旧二进制。/ On Windows hosts gcc appends .exe to the -o target;
# run the artifact actually produced so a stale leftover binary is not executed.
test_bin=build/anki-tests/anki
if [ -x build/anki-tests/anki.exe ]; then test_bin=build/anki-tests/anki.exe; fi
"$test_bin"
