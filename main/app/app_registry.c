/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页清单。四个主页面对应底部导航；传输由文件首页的直达按钮进入，工厂页独立。
 *
 * Product page table. Four main pages match the tab bar; file-home buttons open transfer directly.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_image;
extern const app_desc_t app_dashboard;
extern const app_desc_t app_files;
extern const app_desc_t app_device_settings;
// Anki 学习页只进 Pico 构建（源码在 main/anki/，仅在 main/CMakeLists.txt 的 Pico 分支
// 编译）；S31 的页面清单里没有它，否则链接期会缺 app_anki。/
// The Anki study page is Pico-only: its sources live in main/anki/ and only the Pico
// branch of main/CMakeLists.txt compiles them, so the S31 page table must not name it
// or the link fails on a missing app_anki.
#if !KIIKO_BOARD_METALIO47
extern const app_desc_t app_anki;
#endif

// 正式产品页面清单；工厂测试与历史示例不进入用户导航。
// Production page registry; factory diagnostics and legacy examples are not user-navigable.
static const app_desc_t* const s_apps[] = {
    &app_dashboard,
    &app_book,
    &app_files,
    &app_device_settings,
    &app_image,
#if !KIIKO_BOARD_METALIO47
    &app_anki,
#endif
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_dashboard;
}
