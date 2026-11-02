/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：只读 SQLite 子集实现：页访问、b-树遍历、溢出页组装与记录解码。
 * English: Read-only SQLite subset: page access, b-tree walk, overflow
 * assembly and record decoding.
 */
#include "anki_sqlite.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#define TAG "anki_sql"

#define PAGE_INTERIOR_TABLE 5
#define PAGE_LEAF_TABLE 13
#define LEAF_HEADER 8
#define INTERIOR_HEADER 12
#define DB_HEADER 100

typedef struct {
    char name[ANKI_SQL_MAX_NAME];
    uint32_t root;
    char columns[ANKI_SQL_MAX_COLUMNS][ANKI_SQL_MAX_NAME];
    /// 指向 columns 各行的指针表，供按索引取列名。/ Pointer table into the
    /// column rows so callers can index names directly.
    const char* column_ptrs[ANKI_SQL_MAX_COLUMNS];
    int column_count;
} anki_sql_table_t;

struct anki_sqlite {
    const uint8_t* data;
    size_t size;
    uint32_t page_size;
    uint32_t usable;
    anki_sql_table_t tables[ANKI_SQL_MAX_TABLES];
    int table_count;
    uint8_t* scratch;
    size_t scratch_cap;
};

static const uint8_t* page_at(const anki_sqlite_t* db, uint32_t page_no) {
    if (page_no == 0) return NULL;
    uint64_t offset = (uint64_t)(page_no - 1) * db->page_size;
    if (offset + db->page_size > db->size) return NULL;
    return db->data + offset;
}

// SQLite 头部与页内多字节字段均为大端。/ SQLite header and in-page multi-byte
// fields are big-endian.
static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/// SQLite 大端 varint，最长 9 字节；截断时返回 false。
/// Big-endian SQLite varint, at most 9 bytes; false when truncated.
static bool read_varint(const uint8_t* p, size_t len, uint64_t* out, size_t* used) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8 && i < len; ++i) {
        value = (value << 7) | (uint64_t)(p[i] & 0x7f);
        if (!(p[i] & 0x80)) {
            *out = value;
            *used = i + 1;
            return true;
        }
    }
    if (len < 9) return false;
    value = (value << 8) | p[8];
    *out = value;
    *used = 9;
    return true;
}

/// 记录头里的序列类型 -> 值字节数；无效返回 -1。
/// Serial type to value byte count; -1 when invalid.
static int serial_size(uint64_t serial) {
    switch (serial) {
        case 0: case 8: case 9: return 0;
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 6;
        case 6: case 7: return 8;
        default: break;
    }
    if (serial >= 12) {
        uint64_t n = (serial - 12) / 2;
        if (n > (uint64_t)ANKI_SQL_SCRATCH_MAX) return -1;
        return (int)n;
    }
    return -1; // 10/11 保留 / reserved
}

static void read_int_be(const uint8_t* p, int bytes, int64_t* out) {
    uint64_t raw = 0;
    for (int i = 0; i < bytes; ++i) raw = (raw << 8) | p[i];
    if (bytes < 8) {
        uint64_t mask = (UINT64_C(1) << (bytes * 8)) - 1;
        if (raw & (UINT64_C(1) << (bytes * 8 - 1))) raw |= ~mask;
    }
    *out = (int64_t)raw;
}

/// 组装一条记录的完整 payload（含溢出页）；失败返回错误码。
/// Assemble one record payload including overflow pages; error code on failure.
static esp_err_t assemble_payload(anki_sqlite_t* db, const uint8_t* cell,
                                  size_t cell_len, uint64_t payload_size,
                                  size_t* out_len) {
    const uint32_t usable = db->usable;
    const uint64_t max_local = usable > 35 ? usable - 35 : 0;
    const uint64_t min_local_base = usable > 12 ? usable - 12 : 0;
    const uint64_t min_local = min_local_base * 32 / 255 > 23 ? min_local_base * 32 / 255 - 23 : 0;
    uint64_t local = payload_size;
    if (payload_size > max_local) {
        uint64_t k = min_local + (payload_size - min_local) % (usable - 4);
        local = k <= max_local ? k : min_local;
    }
    if (payload_size > (uint64_t)ANKI_SQL_SCRATCH_MAX) return ESP_ERR_INVALID_SIZE;
    if (payload_size > db->scratch_cap) return ESP_ERR_NO_MEM;
    if (payload_size > max_local && cell_len < (size_t)local + 4) return ESP_ERR_INVALID_SIZE;
    if (cell_len < (size_t)local) return ESP_ERR_INVALID_SIZE;
    memcpy(db->scratch, cell, (size_t)local);
    *out_len = (size_t)payload_size;
    if (payload_size <= max_local) return ESP_OK;

    uint32_t next = rd_u32(cell + local);
    size_t done = (size_t)local;
    while (done < payload_size) {
        const uint8_t* page = page_at(db, next);
        if (!page) return ESP_ERR_INVALID_SIZE;
        next = rd_u32(page);
        size_t chunk = usable - 4;
        if (chunk > (size_t)(payload_size - done)) chunk = (size_t)(payload_size - done);
        memcpy(db->scratch + done, page + 4, chunk);
        done += chunk;
        if (next == 0 && done < payload_size) return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/// 解码 scratch 里的记录并回调；值借用 scratch，仅在回调期间有效。
/// Decode the record in scratch and invoke the callback; values borrow scratch
/// and are valid only for the duration of the call.
static bool decode_row(anki_sqlite_t* db, int64_t rowid, size_t payload_len,
                       anki_sql_row_fn fn, void* ctx) {
    const uint8_t* p = db->scratch;
    uint64_t header_size = 0;
    size_t used = 0;
    if (!read_varint(p, payload_len, &header_size, &used)) return false;
    if (header_size < used || header_size > payload_len) return false;
    size_t header_end = (size_t)header_size;
    size_t body = header_end;

    anki_sql_value_t values[ANKI_SQL_MAX_COLUMNS];
    int count = 0;
    size_t pos = used;
    while (pos < header_end && count < ANKI_SQL_MAX_COLUMNS) {
        uint64_t serial = 0;
        if (!read_varint(p + pos, header_end - pos, &serial, &used)) return false;
        pos += used;
        int size = serial_size(serial);
        if (size < 0) return false;
        anki_sql_value_t* v = &values[count++];
        memset(v, 0, sizeof(*v));
        if (serial == 0) {
            v->type = ANKI_SQL_NULL;
        } else if (serial <= 6) {
            if (body + (size_t)size > payload_len) return false;
            v->type = ANKI_SQL_INT;
            read_int_be(p + body, size, &v->i);
            body += size;
        } else if (serial == 7) {
            if (body + 8 > payload_len) return false;
            uint64_t raw = 0;
            for (int i = 0; i < 8; ++i) raw = (raw << 8) | p[body + i];
            memcpy(&v->f, &raw, sizeof(v->f));
            v->type = ANKI_SQL_FLOAT;
            body += 8;
        } else if (serial == 8 || serial == 9) {
            v->type = ANKI_SQL_INT;
            v->i = serial == 8 ? 0 : 1;
        } else {
            // 12+ 偶数 blob，13+ 奇数 text。/ Even >=12 blob, odd >=13 text.
            if (body + (size_t)size > payload_len) return false;
            v->type = serial % 2 == 0 ? ANKI_SQL_BLOB : ANKI_SQL_TEXT;
            v->text = (const char*)(p + body);
            v->text_len = (size_t)size;
            body += size;
        }
    }
    if (pos != header_end) return false; // 序列类型未对齐到头尾 / serial types misaligned
    return fn(rowid, values, count, ctx);
}

static esp_err_t walk_table(anki_sqlite_t* db, uint32_t page_no, anki_sql_row_fn fn, void* ctx) {
    const uint8_t* base = page_at(db, page_no);
    if (!base) return ESP_ERR_INVALID_SIZE;
    // 第 1 页前部是数据库头，b-树头从 100 开始。/ Page 1 leads with the
    // database header; its b-tree header starts at 100.
    const size_t head = page_no == 1 ? DB_HEADER : 0;
    const uint8_t* page = base + head;
    uint8_t type = page[0];
    uint16_t cells = rd_u16(page + 3);
    if (type == PAGE_LEAF_TABLE) {
        for (uint16_t i = 0; i < cells; ++i) {
            uint16_t offset = rd_u16(page + LEAF_HEADER + i * 2);
            if (offset < head + LEAF_HEADER || offset >= db->page_size) return ESP_ERR_INVALID_SIZE;
            const uint8_t* cell = base + offset;
            size_t cell_len = db->page_size - offset;
            uint64_t payload_size = 0, rowid = 0;
            size_t used = 0;
            if (!read_varint(cell, cell_len, &payload_size, &used)) return ESP_ERR_INVALID_SIZE;
            cell += used;
            cell_len -= used;
            if (!read_varint(cell, cell_len, &rowid, &used)) return ESP_ERR_INVALID_SIZE;
            cell += used;
            cell_len -= used;
            size_t payload_len = 0;
            esp_err_t err = assemble_payload(db, cell, cell_len, payload_size, &payload_len);
            if (err != ESP_OK) return err;
            if (!decode_row(db, (int64_t)rowid, payload_len, fn, ctx)) return ESP_OK;
        }
        return ESP_OK;
    }
    if (type == PAGE_INTERIOR_TABLE) {
        uint32_t rightmost = rd_u32(page + 8);
        for (uint16_t i = 0; i < cells; ++i) {
            uint16_t offset = rd_u16(page + INTERIOR_HEADER + i * 2);
            if (offset < head + INTERIOR_HEADER || offset >= db->page_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (db->page_size - offset < 4) return ESP_ERR_INVALID_SIZE;
            uint32_t child = rd_u32(base + offset);
            esp_err_t err = walk_table(db, child, fn, ctx);
            if (err != ESP_OK) return err;
        }
        return walk_table(db, rightmost, fn, ctx);
    }
    ESP_LOGW(TAG, "unsupported page type %u in table walk", type);
    return ESP_ERR_INVALID_ARG;
}

/// 从 CREATE TABLE 语句里按顶层逗号切出列名，跳过约束关键字。
/// Split a CREATE TABLE statement on top-level commas, skipping constraint
/// keywords, to recover column names.
static int parse_columns(const char* sql, size_t len, char out[ANKI_SQL_MAX_COLUMNS][ANKI_SQL_MAX_NAME]) {
    const char* end = sql + len;
    const char* open = memchr(sql, '(', len);
    if (!open) return 0;
    int depth = 0;
    int count = 0;
    const char* start = open + 1;
    const char* seg_end = NULL; // 收尾段（最后一个逗号到右括号）。
    for (const char* p = open; p < end && count < ANKI_SQL_MAX_COLUMNS; ++p) {
        if (*p == '(') {
            ++depth;
        } else if (*p == ')') {
            --depth;
            if (depth == 0) {
                seg_end = p; // 右括号即最后一列的结束 / the closing paren ends the last column
                break;
            }
        } else if (*p == '\'' || *p == '"' || *p == '`') {
            char quote = *p;
            for (++p; p < end && *p != quote; ++p) {}
            if (p >= end) break;
        } else if (*p == ',' && depth == 1) {
            seg_end = p;
            // 逗号段在下方统一处理。/ Comma segments are handled below.
            const char* seg = start;
            size_t seg_len = (size_t)(p - start);
            while (seg_len && (*seg == ' ' || *seg == '\t' || *seg == '\n' || *seg == '\r')) {
                ++seg;
                --seg_len;
            }
            while (seg_len && (seg[seg_len - 1] == ' ' || seg[seg_len - 1] == '\t' ||
                               seg[seg_len - 1] == '\n' || seg[seg_len - 1] == '\r')) {
                --seg_len;
            }
            if (seg_len && strncasecmp(seg, "primary", 7) != 0 && strncasecmp(seg, "unique", 6) != 0 &&
                strncasecmp(seg, "check", 5) != 0 && strncasecmp(seg, "foreign", 7) != 0 &&
                strncasecmp(seg, "constraint", 10) != 0) {
                size_t name_len = 0;
                while (name_len < seg_len && seg[name_len] != ' ' && seg[name_len] != '\t' &&
                       seg[name_len] != '\n' && seg[name_len] != '\r') {
                    ++name_len;
                }
                if (name_len >= ANKI_SQL_MAX_NAME) name_len = ANKI_SQL_MAX_NAME - 1;
                memcpy(out[count], seg, name_len);
                out[count][name_len] = 0;
                ++count;
            }
            start = p + 1;
            seg_end = NULL;
        }
    }
    // 收尾段：最后一个逗号（或首个括号）到右括号。/ Trailing segment: last comma
    // (or the opening paren) to the closing paren.
    if (seg_end && start < seg_end && count < ANKI_SQL_MAX_COLUMNS) {
        const char* seg = start;
        size_t seg_len = (size_t)(seg_end - start);
        while (seg_len && (*seg == ' ' || *seg == '\t' || *seg == '\n' || *seg == '\r')) {
            ++seg;
            --seg_len;
        }
        while (seg_len && (seg[seg_len - 1] == ' ' || seg[seg_len - 1] == '\t' ||
                           seg[seg_len - 1] == '\n' || seg[seg_len - 1] == '\r')) {
            --seg_len;
        }
        if (seg_len && strncasecmp(seg, "primary", 7) != 0 && strncasecmp(seg, "unique", 6) != 0 &&
            strncasecmp(seg, "check", 5) != 0 && strncasecmp(seg, "foreign", 7) != 0 &&
            strncasecmp(seg, "constraint", 10) != 0) {
            size_t name_len = 0;
            while (name_len < seg_len && seg[name_len] != ' ' && seg[name_len] != '\t' &&
                   seg[name_len] != '\n' && seg[name_len] != '\r') {
                ++name_len;
            }
            if (name_len >= ANKI_SQL_MAX_NAME) name_len = ANKI_SQL_MAX_NAME - 1;
            memcpy(out[count], seg, name_len);
            out[count][name_len] = 0;
            ++count;
        }
    }
    return count;
}

typedef struct {
    anki_sqlite_t* db;
    bool failed;
} master_scan_t;

static bool text_is(const anki_sql_value_t* v, const char* lit) {
    size_t n = strlen(lit);
    return v->type == ANKI_SQL_TEXT && v->text_len == n && memcmp(v->text, lit, n) == 0;
}

static bool on_master_row(int64_t rowid, const anki_sql_value_t* values, int count, void* ctx) {
    (void)rowid;
    master_scan_t* scan = (master_scan_t*)ctx;
    if (count < 5 || !text_is(&values[0], "table") || values[1].type != ANKI_SQL_TEXT) return true;
    anki_sqlite_t* db = scan->db;
    if (db->table_count >= ANKI_SQL_MAX_TABLES) {
        scan->failed = true;
        return false;
    }
    anki_sql_table_t* table = &db->tables[db->table_count];
    size_t name_len = values[1].text_len;
    if (name_len >= ANKI_SQL_MAX_NAME) name_len = ANKI_SQL_MAX_NAME - 1;
    memcpy(table->name, values[1].text, name_len);
    table->name[name_len] = 0;
    table->root = values[3].type == ANKI_SQL_INT ? (uint32_t)values[3].i : 0;
    table->column_count = values[4].type == ANKI_SQL_TEXT
                              ? parse_columns(values[4].text, values[4].text_len, table->columns)
                              : 0;
    for (int c = 0; c < table->column_count && c < ANKI_SQL_MAX_COLUMNS; ++c) {
        table->column_ptrs[c] = table->columns[c];
    }
    ++db->table_count;
    return true;
}

esp_err_t anki_sql_open(const uint8_t* data, size_t size, anki_sqlite_t** out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!data || size < DB_HEADER + LEAF_HEADER) return ESP_ERR_INVALID_SIZE;
    if (memcmp(data, "SQLite format 3\0", 16) != 0) return ESP_ERR_INVALID_ARG;
    // 只接受回滚日志模式（1）；WAL（2/3）的数据可能不完整。
    // Rollback journal mode (1) only; WAL (2/3) images may be incomplete.
    if (data[18] != 1 || data[19] != 1) {
        ESP_LOGW(TAG, "unsupported journal mode %u/%u", data[18], data[19]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (rd_u32(data + 56) != 1) {
        ESP_LOGW(TAG, "unsupported text encoding");
        return ESP_ERR_NOT_SUPPORTED;
    }
    anki_sqlite_t* db = calloc(1, sizeof(*db));
    if (!db) return ESP_ERR_NO_MEM;
    db->data = data;
    db->size = size;
    uint16_t raw = rd_u16(data + 16);
    db->page_size = raw == 1 ? 65536 : raw;
    db->usable = db->page_size - data[20];
    if (db->page_size < 512 || (db->page_size & (db->page_size - 1)) != 0 || db->usable < 480) {
        free(db);
        return ESP_ERR_INVALID_SIZE;
    }
    db->scratch_cap = 64 * 1024;
    db->scratch = malloc(db->scratch_cap);
    if (!db->scratch) {
        free(db);
        return ESP_ERR_NO_MEM;
    }
    master_scan_t scan = {.db = db, .failed = false};
    esp_err_t err = walk_table(db, 1, on_master_row, &scan);
    if (err != ESP_OK || scan.failed) {
        ESP_LOGW(TAG, "sqlite_master scan failed: %s", esp_err_to_name(err));
        anki_sql_close(db);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    *out = db;
    return ESP_OK;
}

void anki_sql_close(anki_sqlite_t* db) {
    if (!db) return;
    free(db->scratch);
    free(db);
}

uint32_t anki_sql_table_root(const anki_sqlite_t* db, const char* name) {
    if (!db || !name) return 0;
    for (int i = 0; i < db->table_count; ++i) {
        if (strcasecmp(db->tables[i].name, name) == 0) return db->tables[i].root;
    }
    return 0;
}

uint32_t anki_sql_user_version(const anki_sqlite_t* db) {
    if (!db || db->size < 64) return 0;
    return rd_u32(db->data + 60);
}

int anki_sql_table_columns(const anki_sqlite_t* db, const char* name, const char* const** names) {
    if (!db || !name) return 0;
    for (int i = 0; i < db->table_count; ++i) {
        if (strcasecmp(db->tables[i].name, name) == 0) {
            *names = db->tables[i].column_ptrs;
            return db->tables[i].column_count;
        }
    }
    return 0;
}

esp_err_t anki_sql_scan_table(anki_sqlite_t* db, const char* name, anki_sql_row_fn fn, void* ctx) {
    if (!db || !name || !fn) return ESP_ERR_INVALID_ARG;
    uint32_t root = anki_sql_table_root(db, name);
    if (root == 0) return ESP_ERR_NOT_FOUND;
    return walk_table(db, root, fn, ctx);
}
