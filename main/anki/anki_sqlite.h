/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡组导入用的只读 SQLite 子集。只解表 b-树与记录格式，不写库。
 * English: Read-only SQLite subset for Anki deck import. Decodes table b-trees
 * and the record format only; never writes to the database.
 *
 * 冻结：只读；拒绝 WAL、加密与 ZIP64 之外的未知页类型；所有长度有界。
 * Frozen: read-only; rejects WAL, encryption and unknown page types; every
 * length is bounded.
 *
 * 支持范围（与 Anki collection.anki2 / anki21 对齐）：
 * Scope (matches Anki collection.anki2 / anki21):
 *   - 页大小 512..65536，UTF-8 编码，回滚日志模式。
 *   - 表 b-树（页类型 5/13）遍历，含溢出页组装。
 *   - 记录序列类型 0..9 与 12+ 的 text/blob。
 *   - 从 sqlite_master 取表名、根页与 CREATE TABLE 列名。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ANKI_SQL_MAX_TABLES 24
#define ANKI_SQL_MAX_COLUMNS 48
#define ANKI_SQL_MAX_NAME 96
#define ANKI_SQL_SCRATCH_MAX (4U * 1024U * 1024U)

typedef struct anki_sqlite anki_sqlite_t;

///  borrowed from the assembled payload; valid only for the duration of the call.
typedef struct {
    enum {
        ANKI_SQL_NULL = 0,
        ANKI_SQL_INT,
        ANKI_SQL_FLOAT,
        ANKI_SQL_TEXT,
        ANKI_SQL_BLOB,
    } type;
    int64_t i;
    double f;
    const char* text;
    size_t text_len;
} anki_sql_value_t;

/// 返回 false 提前结束遍历。/ Return false to stop the scan early.
typedef bool (*anki_sql_row_fn)(int64_t rowid, const anki_sql_value_t* values,
                                int count, void* ctx);

/// 打开内存中的库映像；缓冲区由调用方持有，须比句述活得久。失败时 *out 为 NULL。
/// Open an in-memory database image; the caller owns the buffer and it must
/// outlive the handle. *out is NULL on failure.
esp_err_t anki_sql_open(const uint8_t* data, size_t size, anki_sqlite_t** out);
/// 释放句柄，允许 NULL。/ Release the handle; accepts NULL.
void anki_sql_close(anki_sqlite_t* db);

/// 按名字取表根页，未找到返回 0。/ Root page for a table name, 0 when absent.
uint32_t anki_sql_table_root(const anki_sqlite_t* db, const char* name);
/// 数据库头部的 user_version（Anki schema 版本）。/ The header user_version (Anki schema version).
uint32_t anki_sql_user_version(const anki_sqlite_t* db);
/// 取表的列名（按声明顺序）；未知表返回 0。/ Column names in declaration order; 0 for an unknown table.
int anki_sql_table_columns(const anki_sqlite_t* db, const char* name,
                           const char* const** names);

/// 遍历整表，每行回调一次；行内借用值仅在回调期间有效。
/// Visit every row once; borrowed values are valid only during the callback.
esp_err_t anki_sql_scan_table(anki_sqlite_t* db, const char* name,
                              anki_sql_row_fn fn, void* ctx);
