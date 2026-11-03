#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""Run the real home-entry cache decision against changing last-read paths."""
from pathlib import Path
import re
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "main/apps/app_dashboard.c"


def function(name):
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", source, re.M)
    assert match, name
    start, cursor, depth = match.start(), match.end(), 1
    while depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[start:cursor]


UNIT = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BOOK_STORE_PATH_MAX 288
#define ESP_ERR_NOT_FINISHED 7
typedef struct { char path[BOOK_STORE_PATH_MAX]; } home_book_t;
typedef struct { bool present, mounted; } read_pico_sd_info_t;
typedef struct { int unused; } app_ctx_t;
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { unsigned start, last, steps; int64_t stamp; int view; bool valid; } clock_trail_t;
typedef enum { APP_HOME_FOLDER, APP_HOME_CLOCK, APP_HOME_KEHAI } app_home_component_t;
enum { HOME_ROOT, HOME_COMPONENT, HOME_GOAL, HOME_TIME };
typedef int esp_err_t;
static home_book_t s_current, s_recent[6];
static bool s_books_cache_valid,s_cached_sd_present,s_cached_sd_mounted,s_scan_pending,s_home_force_full_once;
static unsigned s_cached_store_revision;
static char s_cached_books_dir[128],s_cached_last_path[BOOK_STORE_PATH_MAX];
static int s_cover_next,scans,refreshes,probes;
static char latest[BOOK_STORE_PATH_MAX];
static esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *out){*out=(read_pico_sd_info_t){.present=true,.mounted=true};return 0;}
static unsigned book_store_revision(void){return 7;}
static const char *app_settings_books_dir(void){return "/sdcard/books";}
static bool app_settings_home_full_refresh(void){return false;}
static bool book_progress_last_path(char *out,size_t cap){snprintf(out,cap,"%s",latest);return latest[0]!=0;}
static void scan_books(void){++scans;snprintf(s_current.path,sizeof(s_current.path),"%s",latest);}
static void refresh_progress(void){++refreshes;}
static void restore_cover_cache(void){}
static void read_pico_sd_start_probe(void){++probes;}
// on_enter 在 rc58 之后长出了封面、表盘、摘要与暂停态等初始化步骤；这里为宿主
// 补齐它们的最小桩，让真实的缓存判定仍原样参与断言。/ on_enter grew cover,
// clock-base, excerpt and pause-state initialisation after rc58, so the host unit
// provides minimal stubs for those and keeps the real cache decision under test.
static EpdRect s_component_dirty;
static unsigned s_cover_generation;
static bool s_clock_base_attempted;
static clock_trail_t s_widget_trail,s_time_trail;
static int s_view;
static bool s_clock_pending,s_time_pending,s_widget_pending,s_component_pending,s_press_pending,s_save_error,s_folder_attempted;
static int64_t s_time_stamp=-1,s_widget_stamp=-1,s_motion_ms=-100;
static uint32_t s_cached_progress_revision=UINT32_MAX,s_folder_key;
static uint8_t *s_folder;
static bool s_widgets_paused;
static unsigned cover_jobs,progress_revision;
static unsigned book_cover_jobs_begin(void){return ++cover_jobs;}
static uint32_t book_progress_revision(void){return ++progress_revision;}
static void clock_base_prepare(void){}
static void avatar_prepare_state(void){}
static void rp_boot_mark(const char *stage){(void)stage;}
static uint32_t folder_key(void){return 0;}
static void refresh_excerpt(void){}
static void refresh_days(void){}
static app_home_component_t app_settings_home_component(void){return APP_HOME_FOLDER;}
static bool home_widgets_paused(void){return false;}
'''
UNIT += function("remember_scan_state") + "\n" + function("on_enter")
UNIT += r'''
int main(void){
    app_ctx_t ctx={0};
    strcpy(latest,"/sdcard/books/old.epub");on_enter(&ctx);
    assert(scans==1&&probes==1&&!strcmp(s_current.path,latest));
    strcpy(latest,"/sdcard/books/new.epub");on_enter(&ctx);
    assert(scans==2&&probes==1&&!s_scan_pending&&!strcmp(s_current.path,latest));
    on_enter(&ctx);
    assert(scans==2&&refreshes==1&&probes==1);
    latest[0]=0;on_enter(&ctx);
    assert(scans==3&&!s_current.path[0]);
    puts("home_recent: latest book invalidates the home cache without an SD reprobe");
}
'''

with tempfile.TemporaryDirectory(prefix="home-recent-") as temp:
    source = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source.write_text(UNIT, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
