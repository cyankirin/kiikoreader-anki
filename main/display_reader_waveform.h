/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "epdiy.h"
#include <stdbool.h>

/// 柔和文字序列：白底到中灰的少量回浅脉冲提前；具体阅读模式由板级出口选择。
/// Soft text phases advance a few gray-edge settling pulses; board output selects the reading modes that use them.
const EpdWaveform* display_reader_text_waveform(void);
#if !KIIKO_BOARD_METALIO47
/// Pico reading ripple retains its gray sequence plus four changed-white phases before settling.
const EpdWaveform* display_reader_water_waveform(void);
#endif

#include "epd_highlevel.h"
/// Pico速读保留原厂DU；S31保留所选DU原序，定稿前追加四相目标白，使用批量模式别名。
/// Pico FastBW retains native DU; S31 retains the selected DU order, inserts four white-target phases before settling and aliases the batch mode.
const EpdWaveform* display_reader_fast_text_waveform(void);
/// 两板普通翻页分别派生本板波形：Pico等灰保持、变白补四相；S31沿Moku GL37补六相目标白。
/// Ordinary uses board-native profiles: Pico holds equal grays and adds four changed-white phases; S31 retains Moku GL37 plus six white-target phases.
const EpdWaveform* display_reader_soft_turn_waveform(void);
/// 只归一文字区域目标，保留真实back和区域外灰阶。/ Normalize only the text target, retaining real history and outside grays.
bool display_reader_fast_text_prepare(EpdiyHighlevelState* hl,EpdRect area);
