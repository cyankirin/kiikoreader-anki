#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
#
# Anki 学习页排版单元的宿主探针。探针直接 include 真机的排版头文件
# （main/apps/anki_layout.h），只把字体度量与字宽换成确定性假实现，所以测的
# 就是设备上跑的那份代码。
# Host probe for the Anki study-page layout unit. The probe includes the real
# layout header (main/apps/anki_layout.h) and only swaps the font metrics and
# text widths for deterministic fakes, so it exercises the device's own code.
#
# 在仓库根目录运行；产物落在 build/。/ Run from the repository root; artifacts
# stay under build/.
set -euo pipefail
mkdir -p build/anki-tests
flags=(-std=c11 -Wall -Wextra -Werror -g)
# CI 与常规 Linux 宿主带 ASan/UBSan；运行时缺失时退化为普通构建。
# CI and regular Linux hosts carry ASan/UBSan; fall back to a plain build when
# the runtime is missing.
if echo 'int main(void){return 0;}' | gcc -fsanitize=address,undefined -x c -o /dev/null - 2>/dev/null; then
    flags+=(-fsanitize=address,undefined)
fi
gcc "${flags[@]}" -Imain/apps tools/anki_layout_probe.c -o build/anki-tests/anki_layout_probe
# Windows 宿主上 gcc 会在 -o 目标后补 .exe；按实际产物运行，避免误跑上一次的
# 旧二进制。/ On Windows hosts gcc appends .exe to the -o target; run the
# artifact actually produced so a stale leftover binary is not executed.
test_bin=build/anki-tests/anki_layout_probe
if [ -x build/anki-tests/anki_layout_probe.exe ]; then test_bin=build/anki-tests/anki_layout_probe.exe; fi
"$test_bin"
