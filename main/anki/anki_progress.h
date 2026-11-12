/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 学习进度备份的对外契约：行式 UTF-8 .akb、单份覆盖导出、
 *       整文件事务式恢复、两根备份枚举与启动自愈。
 * English: Public contract for Anki progress backup: line-oriented UTF-8
 *       .akb, single-file overwrite export, whole-file transactional restore,
 *       dual-root backup enumeration and startup recovery.
 *
 * 冻结：备份不是卡组包；恢复前提是设备已有同 deck_id 卡组；任何失败路径
 *       设备进度不变。/ Frozen: a backup is not a deck package; restore
 *       requires an existing deck with the same deck_id; no failure path
 *       ever changes on-device progress.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anki_store.h"
#include "esp_err.h"

#define ANKI_BACKUP_FORMAT_VERSION 1
#define ANKI_BACKUP_SUFFIX ".akb"
#define ANKI_BACKUP_TMP_SUFFIX ".akb.tmp"
#define ANKI_DECK_TMP_SUFFIX ".akd.tmp"
#define ANKI_DECK_BAK_SUFFIX ".akd.bak"
#define ANKI_BACKUP_LINE_MAX 192
#define ANKI_BACKUP_FILE_MAX (2U * 1024U * 1024U)
/* U1：恢复卡数上限，超出拒绝并提示「卡组过大，暂不支持恢复」。
   U1: restore card cap; exceeding it rejects with a clear hint. */
#define ANKI_RESTORE_CARD_MAX 20000

typedef struct {
    int64_t deck_id;
    char name[ANKI_DECK_NAME_MAX];
    int covered;  /* 覆盖 N 张 / cards covered */
    int skipped;  /* 跳过 M 张（U4：两侧对不上的都计入）/ skipped both sides */
} anki_progress_result_t;

/// 备份枚举回调；返回 false 提前结束。/ Backup visitor; false stops early.
typedef bool (*anki_backup_visit_fn)(const char* path, const char* filename, void* ctx);

/// 导出到活动根 <deck_id>.akb（单份覆盖，tmp+rename）；out_path 可选回传最终路径。
/// Export to the active root; tmp+rename, one file per deck, no TF gating (Q3).
esp_err_t anki_progress_export(int64_t deck_id, char* out_path, size_t out_path_len);

/// 两根扫描 *.akb，按文件名去重（活动根优先），回调；不自动挑最新（Q9）。
/// Scan both roots, dedup by filename (active first), never auto-pick newest.
esp_err_t anki_progress_list_backups(anki_backup_visit_fn visit, void* ctx);

/// 从指定备份恢复；调用方已完成二次确认；全量校验 + 原子替换；失败设备进度不变。
/// Restore one backup after UI confirmation; full validation + atomic commit;
/// any failure leaves on-device progress untouched.
esp_err_t anki_progress_restore(const char* backup_path, anki_progress_result_t* out);

/// 启动自愈：清理残留 .akd.tmp / 回滚 .akd.bak / 清理孤儿 .aks / 清 .akb.tmp。
/// 幂等，无残留时零写操作。/ Idempotent startup recovery; zero writes when clean.
void anki_progress_recover(void);
