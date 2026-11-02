/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网更新的版本清单与可取消后台任务。/ English: release metadata and cancellable online update jobs.
 * 冻结：只写空闲应用槽，完整校验后由 UI 确认启动。/ Frozen: only write the inactive app slot; UI commits boot after verification.
 * 用户修订：固件擦写与屏幕推送互斥，进度只追加变化像素；不并行驱动 PSRAM 画面和闪存擦写。
 * User revision: serialize firmware flash operations with display output; append progress changes without concurrent PSRAM scans and flash writes.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#if KIIKO_BOARD_METALIO47
#define PICO_OTA_IMAGE_MAX 0x500000
#define PICO_OTA_PROJECT "Kiikoread_Metalio_S31"
#define PICO_OTA_LAYOUT "metalio-s31-dual-5m-v1"
#define PICO_OTA_BOARD "Metalio-EINK4-Plus-S31"
#define PICO_OTA_CAPABILITY "METALIO_S31_HTTPS_OTA_V1"
#define PICO_OTA_FEED_URL "https://wegooo-cell.github.io/read-pico-reader/metalio-s31-update.json"
#define PICO_OTA_FEED_FALLBACK_URL "https://kiikoread.com/metalio-s31-update.json"
#else
#define PICO_OTA_IMAGE_MAX 0x400000
#define PICO_OTA_PROJECT "Read_Pico"
#define PICO_OTA_LAYOUT "pico-dual-4m-v1"
#define PICO_OTA_BOARD "RDP-G01-W"
#define PICO_OTA_CAPABILITY "PICO_HTTPS_OTA_V1"
#define PICO_OTA_FEED_URL "https://wegooo-cell.github.io/read-pico-reader/update.json"
#define PICO_OTA_FEED_FALLBACK_URL "https://kiikoread.com/update.json"
// 中文：Anki 版清单走本 fork 的 Pages 镜像；官方清单常量原样保留，互不影响。
// English: The Anki manifest lives on this fork's Pages mirror; the official constants stay untouched.
#define PICO_OTA_ANKI_FEED_URL "https://cyankirin.github.io/kiikoreader-anki/anki-update.json"
#endif

// 中文：升级通道。官方通道完全沿用原有清单源与行为；Anki 通道只读本 fork 的清单，
//       两者共用同一套解析、校验、写入与提交路径。
// English: Update channels. The official channel keeps its original feed and behaviour; the
//          Anki channel reads this fork's manifest only. Both share parsing, verification,
//          writing and commit.
typedef enum { PICO_OTA_CHANNEL_OFFICIAL = 0, PICO_OTA_CHANNEL_ANKI = 1 } pico_ota_channel_t;

typedef enum { PICO_UPDATE_IDLE, PICO_UPDATE_CHECKING, PICO_UPDATE_AVAILABLE,
               PICO_UPDATE_LATEST, PICO_UPDATE_DOWNLOADING, PICO_UPDATE_READY,
               PICO_UPDATE_FAILED, PICO_UPDATE_CANCELLED } pico_update_state_t;
typedef struct {
    char version[33], project[33], board[24], layout[32], minimum_base_version[33];
    char url[256], sha256[65], notes[384];
    uint32_t size;
} pico_release_t;
typedef struct {
    pico_update_state_t state;
    pico_release_t release;
    uint32_t revision, received;
    char message[96];
    bool busy;
    // 中文：当前状态所属的通道，界面据此区分「官方版 / Anki 版」。
    // English: Channel the current status belongs to; the UI names it as official or Anki.
    pico_ota_channel_t channel;
} pico_update_status_t;

/// 有界解析、型号与版本验证；不接触网络。/ Bounded parsing, board and version validation without networking.
esp_err_t pico_release_parse(const char *json, size_t length, pico_release_t *release);
int pico_version_compare(const char *a, const char *b);
bool pico_release_url_valid(const char *url);
/// 检查或下载唯一后台任务；离页、锁屏先取消并等待。/ Start one worker; cancel and join on exit or lock.
esp_err_t pico_online_check(void);
esp_err_t pico_online_download(void);
#if !KIIKO_BOARD_METALIO47
/// 中文：按通道检查/下载；官方通道与 pico_online_check()/pico_online_download() 等价。
/// English: Check/download on a chosen channel; the official channel equals the plain calls.
esp_err_t pico_online_check_channel(pico_ota_channel_t channel);
esp_err_t pico_online_download_channel(pico_ota_channel_t channel);
#endif
bool pico_online_busy(void);
void pico_online_get_status(pico_update_status_t *status);
void pico_online_cancel_join(void);
/// 刷屏与 OTA 擦写共用递归锁；未初始化 OTA 时不分配内存。/ Recursive display/OTA exclusion; no allocation before OTA initialization.
bool pico_online_display_begin(void);
void pico_online_display_end(bool held);
/// 已验证镜像切换启动槽；失败不切换。/ Select a verified image; failures keep the existing boot slot.
esp_err_t pico_online_commit(void);
