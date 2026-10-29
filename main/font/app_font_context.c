/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：异常启动暂用内建字体，避免自动重载故障资源；不覆盖用户字体设置。
 * User revision: recovery starts use the built-in face without retrying faulty resources or overwriting saved font choices.
 * 中文：系统默认使用内建思源黑体子集，TF 卡黑体补足缺字；用户选择的字体覆盖默认外观。
 * English: The built-in Source Han Sans face is the default; the SD face fills missing glyphs and explicit choices override it.
 * 用户修订：票根始终借用系统字体，浅睡解锁恢复锁屏前的实际字体和上下文，不覆盖保存的选择。
 * User revision: tickets always borrow the system face; light-sleep wake restores the actual previous face/context without altering saved choices.
 */
#include "app_font_context.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "settings.h"
#include "boot_state.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_nav.h"

static bool s_reading;
// Failed unchanged files are not repeatedly opened (opening unloads the active slot).
static char s_rejected[TTF_FONT_PATH_MAX];
static time_t s_rejected_mtime;
static off_t s_rejected_size;
// 异常启动（崩溃/掉电）后系统字体先锁在内建字体上，避免重载可疑资源；用户在
// 设置里显式选过字体后解除这把锁——显式选择是用户的明确意图，应当立即生效，
// 否则「换字体没反应」会被误判成字体功能坏了，直到下一次干净启动才恢复。
// After an abnormal boot (crash / power loss) the system face starts locked to
// the built-in one so suspect resources are not reloaded. An explicit pick in
// Settings releases the lock: an explicit choice is the user's clear intent and
// must take effect at once, or "changing the font does nothing" reads as a
// broken font feature until the next clean boot.
static bool s_user_choice;
static bool font_file(const char *path, struct stat *st) {
    return path && stat(path, st) == 0 && S_ISREG(st->st_mode) && st->st_size > 0;
}

static const char* system_path(char fallback[TTF_FONT_PATH_MAX]) {
    if (pico_boot_recovery() && !s_user_choice) return TTF_FONT_BUILTIN;
    const char* chosen = app_settings_system_font_path();
    if (chosen[0]) return chosen;
    if (snprintf(fallback, TTF_FONT_PATH_MAX, "%s/Hei.ttf", app_settings_fonts_dir()) >= TTF_FONT_PATH_MAX)
        return TTF_FONT_BUILTIN;
    struct stat st;
    if (stat(fallback, &st) == 0 && S_ISREG(st.st_mode)) return fallback;
    if (!ttf_font_count()) ttf_font_scan();
    const ttf_font_item_t *item = ttf_font_item(0);
    return item && font_file(item->path, &st) ? item->path : TTF_FONT_BUILTIN;
}

static bool activate(const char* path) {
    struct stat st = {0};
    bool builtin = ttf_font_path_is_builtin(path);
    bool valid = builtin || font_file(path, &st);
    bool rejected = !builtin && !strcmp(s_rejected, path) &&
        s_rejected_mtime == st.st_mtime && s_rejected_size == st.st_size;
    if (!valid || rejected) path = TTF_FONT_BUILTIN;
    if (ttf_font_ready() && !strcmp(ttf_font_path(), path)) return false;
    esp_err_t err = ttf_font_open(path);
    if (err == ESP_OK) {
        if (!ttf_font_path_is_builtin(path)) s_rejected[0] = 0;
        return true;
    }
    // 失败不能静默：路径与错误码必须留在日志里，否则现场只能看到「字体变回
    // 内建」，无从判断是文件没了、字体坏了还是内存不够。
    // A failure must never be silent: the path and the error code belong in the
    // log, or the field only shows "the face went back to built-in" with no way
    // to tell a missing file, a broken face and a memory shortfall apart.
    ESP_LOGE("app_font", "activate %s failed: %s", path, esp_err_to_name(err));
    if (err == ESP_ERR_NO_MEM) {
        // 内存不足是暂时状态，绝不能记进「已拒绝」：那会让这个字体在整次开机
        // 内被永久跳过（用户看到的就是「换不动，重启才好」）。先退回已缓存的
        // 字形位图，再原样重试一次；仍失败就本次回落到内建字体，但下次调用
        // 还会重新尝试整个字库。
        // A shortfall is transient and must never be recorded as "rejected":
        // that would skip this face for the whole boot, which is exactly the
        // "cannot change font until a reboot" the user reported. Hand the
        // cached glyph bitmaps back first and retry the very same load once;
        // if it still fails this call falls back to the built-in face, but the
        // next call retries the real face again.
        size_t reclaimed = ttf_font_cache_clear_all();
        ESP_LOGE("app_font", "retry %s after reclaiming %u KB of glyph cache", path,
                 (unsigned)(reclaimed / 1024));
        err = ttf_font_open(path);
        if (err == ESP_OK) {
            if (!ttf_font_path_is_builtin(path)) s_rejected[0] = 0;
            return true;
        }
        ESP_LOGE("app_font", "retry %s failed: %s", path, esp_err_to_name(err));
    } else if (!ttf_font_path_is_builtin(path)) {
        snprintf(s_rejected, sizeof(s_rejected), "%s", path);
        s_rejected_mtime = st.st_mtime;
        s_rejected_size = st.st_size;
    }
    if (ttf_font_ready() && ttf_font_is_builtin()) return false;
    return ttf_font_open_builtin() == ESP_OK;
}

void app_font_allow_user_choice(void) {
    // 用户显式选过字体：解除异常启动加在系统字体上的内建锁，并清掉「已拒绝」
    // 记录——那可能是上一次内存不足留下的，重试才有机会成功。
    // The user picked a face explicitly: release the built-in lock that an
    // abnormal boot put on the system face, and clear the rejection record,
    // which may be a leftover from an earlier memory shortfall and would
    // otherwise block the retry forever.
    s_user_choice = true;
    s_rejected[0] = 0;
    s_rejected_mtime = 0;
    s_rejected_size = 0;
}

bool app_font_activate_system(void) {
    s_reading = false;
    ttf_set_stroke_adjust(0);
    ttf_set_antialias(true);
    const char* chosen = app_settings_system_font_path();
    char fallback[TTF_FONT_PATH_MAX];
    const char* path = system_path(fallback);
    bool changed = activate(path);
    // 未选字体时由内建思源黑体绘制界面；TTF 仍保留完整字库供文件名缺字回退。
    // Built-in Source Han Sans draws default UI; the full TTF remains loaded for missing glyphs.
    ui_text_set_system_font((s_user_choice || !pico_boot_recovery()) && chosen[0] &&
                           ttf_font_ready() && !strcmp(chosen, ttf_font_path()) &&
                           !ttf_font_is_builtin());
    ui_text_set_system_scale(true);
    ui_nav_status_font_context(false, changed);
    return changed;
}

bool app_font_activate_reading(void) {
    s_reading = true;
    ttf_set_stroke_adjust_fine(app_settings_book_weight_fine());
    ttf_set_antialias(app_settings_book_antialias());
    const char* saved = (pico_boot_recovery() && !s_user_choice) ? TTF_FONT_BUILTIN
                                                                 : app_settings_font_path();
    bool changed = activate(ttf_font_path_is_builtin(saved) ? TTF_FONT_BUILTIN : saved);
    ui_text_set_system_font(false);
    ui_text_set_system_scale(false);
    ui_nav_status_font_context(true, changed);
    return changed;
}

bool app_font_retry_active(void) {
    return s_reading ? app_font_activate_reading() : app_font_activate_system();
}

void app_font_begin_lock(app_lock_font_t *saved) {
    if (!saved) return;
    snprintf(saved->path, sizeof(saved->path), "%s", ttf_font_path());
    saved->reading = s_reading;
    app_font_activate_system();
}

void app_font_end_lock(const app_lock_font_t *saved) {
    if (!saved) return;
    (void)activate(saved->path);
    s_reading = saved->reading;
    ttf_set_stroke_adjust_fine(s_reading ? app_settings_book_weight_fine() : 0);
    ttf_set_antialias(!s_reading || app_settings_book_antialias());
    const char *chosen = app_settings_system_font_path();
    ui_text_set_system_font((s_user_choice || !pico_boot_recovery()) && !s_reading && chosen[0] &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(!s_reading);
    ui_nav_status_font_context(s_reading, false);
}
