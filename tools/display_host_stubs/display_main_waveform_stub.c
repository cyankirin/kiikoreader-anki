/* SPDX-License-Identifier: Apache-2.0
 * 中文：显示宿主测试的波形整形状替身（测试文件已自带 clean 版本，这里只补快刷入口）。上游把「按来源/目标整形成六或八相变白」的板级实现搬进
 *      独立编译单元，而本探针只验证刷新区域、模式与条带时序；这里返回原波形（不做相位整形），
 *      与合并前的行为一致，避免把生产波形表拖进宿主测试。
 * English: Waveform-shaping shim for the display host test. Upstream moved the board-level
 *      "six/eight changed-white phases" shaping into its own translation unit, while this probe only
 *      checks refresh areas, modes and band timing. Returning the source waveform (no shaping)
 *      matches the pre-merge behaviour without pulling the production tables into the host test.
 */
#include "epdiy.h"

const EpdWaveform *display_main_fast_background_waveform_with_phases(
    const EpdWaveform *source, enum EpdDrawMode mode, unsigned white_phases
) {
    (void)mode;
    (void)white_phases;
    return source;
}
