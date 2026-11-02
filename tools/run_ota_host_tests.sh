#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 中文：联网升级的宿主测试：清单解析/版本比较（共享源），以及两块板各自的 OTA
#       任务（rc103 起 OTA 任务按板存放，没有共享的 main/ota_online.c）。
# / Host tests for online update: release parsing and version ordering (shared
#   source), plus each board's own OTA worker — since rc103 the worker lives per
#   board and there is no shared main/ota_online.c.
# 在仓库根目录运行；产物仅在 build。/ Run from the repository root; artifacts stay in build.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
mkdir -p build/ota-tests

cc_bin="${CC:-cc}"
cjson="managed_components/espressif__cjson/cJSON/cJSON.c"
common_flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-deprecated-declarations
              -Itools/ota_stubs -Imain -Imanaged_components/espressif__cjson/cJSON)

# CI 与常规 Linux 宿主带 ASan/UBSan；运行时缺失时退化为普通构建（部分 Windows 宿主）。
# CI and regular Linux hosts carry ASan/UBSan; fall back to a plain build when
# the runtime is missing (some Windows hosts).
san_flags=()
if echo 'int main(void){return 0;}' | "$cc_bin" -fsanitize=address,undefined -x c -o /dev/null - 2>/dev/null; then
    san_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

# Windows 宿主上 cc 会在 -o 目标后补 .exe；按实际产物运行，避免误跑旧二进制。
# On Windows hosts cc appends .exe to the -o target; run the artifact actually
# produced so a stale leftover binary is not executed.
run_built() {
    local target="$1"
    if [ -x "${target}.exe" ]; then
        "${target}.exe"
    else
        "${target}"
    fi
}

# 1) 清单解析与版本比较：两块板共用同一份 main/ota_release.c。
#    Release parsing and version ordering: both boards share main/ota_release.c.
echo "== ota_release (shared) =="
"$cc_bin" "${common_flags[@]}" "${san_flags[@]}" \
    tools/ota_release_host_test.c main/ota_release.c "$cjson" \
    -lm -o build/ota-tests/ota_release
run_built build/ota-tests/ota_release

# 2) 每块板各自的 OTA 任务实现。
#    Each board's own OTA worker implementation.
for board in pico metalio47; do
    if [ "$board" = "metalio47" ]; then
        define=1
    else
        define=0
    fi
    echo "== ota_online ($board) =="
    "$cc_bin" "${common_flags[@]}" "${san_flags[@]}" -DKIIKO_BOARD_METALIO47="$define" \
        tools/ota_online_host_test.c main/ota_release.c "$cjson" \
        -lm -pthread -o "build/ota-tests/ota_online-$board"
    run_built "build/ota-tests/ota_online-$board"
done
