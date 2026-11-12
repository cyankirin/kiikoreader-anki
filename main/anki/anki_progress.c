/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：进度备份实现：行式文本读写、导出 tmp+rename、恢复整文件事务
 *       （.akd 全量读入 PSRAM → 内存 patch → .akd.tmp → .akd.bak 双文件提交）、
 *       快速/宽松两阶段按 card_id 匹配、两根枚举去重与启动自愈。
 * English: Progress backup: line text I/O, tmp+rename export, whole-file
 *       restore transaction, fast/loose two-phase card_id matching, dual-root
 *       enumeration and startup recovery.
 *
 * 依赖单向：本文件 -> anki_store.h；anki_store.c 绝不 include anki_progress.h。
 * One-way dependency: this file -> anki_store.h; anki_store.c never includes
 * anki_progress.h.
 */
#include "anki_progress.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_store.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define TAG "anki_progress"
#define PROGRESS_PATH_MAX 320
#define ANKI_BACKUP_DAY_MAX 1000000000LL
#define ANKI_BACKUP_NAME_MAX 65
#define ANKI_BACKUP_SEEN_MAX 96
#define ANKI_RECOVER_ID_MAX 64

static void* progress_alloc(size_t n) {
#ifdef ESP_PLATFORM
    // 整库副本与宽松匹配索引显式落 PSRAM，不吃内部 RAM。
    // Whole-file copies and the loose-match index must live in PSRAM.
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t rd_i64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= (uint64_t)p[i] << (8 * i);
    return (int64_t)u;
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_i64(uint8_t* p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((u >> (8 * i)) & 0xFFu);
}

/* 显式限宽，避免 GCC -Wformat-truncation 在 -Werror 下报错。
   / Explicit widths keep -Wformat-truncation quiet under -Werror. */
static void deck_file_path(const char* root, int64_t deck_id, const char* suffix,
                           char* out, size_t cap) {
    snprintf(out, cap, "%.200s/%lld%.16s", root, (long long)deck_id, suffix);
}

static uint32_t tag_of_root(const char* root) {
    return strcmp(root, ANKI_DIR_TF) == 0 ? ANKI_ROOT_TAG_TF : ANKI_ROOT_TAG_FLASH;
}

static bool file_size_of(const char* path, size_t* out) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return false;
    *out = (size_t)st.st_size;
    return true;
}

static bool suffix_eq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static uint8_t* read_whole_psram(const char* path, size_t* out_len) {
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > ANKI_DECK_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    uint8_t* buf = progress_alloc(size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, size, f);
    bool closed = fclose(f) == 0;
    if (got != size || !closed) {
        free(buf);
        return NULL;
    }
    *out_len = size;
    return buf;
}

typedef struct {
    int64_t deck_id;
    int64_t day;
    int64_t last_rtc_day;
    uint32_t card_count;
    uint32_t table_at;
    uint32_t name_len;
    const char* name;  /* 借用 buf / borrowed from buf */
} akd_header_t;

static esp_err_t akd_parse(const uint8_t* buf, size_t len, akd_header_t* out) {
    if (!buf || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (len < AKD_HEADER_BYTES) return ESP_ERR_INVALID_SIZE;
    if (memcmp(buf + AKD_OFF_MAGIC, AKD_MAGIC, 4) != 0) return ESP_ERR_INVALID_SIZE;
    if (rd_u32(buf + AKD_OFF_VERSION) != AKD_VERSION) return ESP_ERR_INVALID_VERSION;
    out->deck_id = rd_i64(buf + AKD_OFF_DECK_ID);
    out->day = rd_i64(buf + AKD_OFF_DAY);
    out->last_rtc_day = rd_i64(buf + AKD_OFF_LAST_RTC_DAY);
    out->card_count = rd_u32(buf + AKD_OFF_CARD_COUNT);
    out->name_len = rd_u32(buf + AKD_OFF_NAME_LEN);
    out->table_at = rd_u32(buf + AKD_OFF_TABLE_AT);
    if (out->name_len >= ANKI_DECK_NAME_MAX) return ESP_ERR_INVALID_SIZE;
    size_t table_end = (size_t)out->table_at + (size_t)out->card_count * AKD_CARD_RECORD;
    if (out->table_at < AKD_HEADER_BYTES || table_end > len) return ESP_ERR_INVALID_SIZE;
    if ((size_t)AKD_HEADER_BYTES + out->name_len + 1 > len) return ESP_ERR_INVALID_SIZE;
    out->name = (const char*)(buf + AKD_HEADER_BYTES);
    return ESP_OK;
}

static bool akd_file_valid(const char* path) {
    size_t len = 0;
    uint8_t* buf = read_whole_psram(path, &len);
    if (!buf) return false;
    akd_header_t h = {0};
    bool ok = akd_parse(buf, len, &h) == ESP_OK;
    free(buf);
    return ok;
}

/* 名称/来源净化：截断到 64 B，替换逗号与换行，避免破坏行格式（K8）。
   / Sanitize name/source: cap at 64 B, replace commas and newlines (K8). */
static void sanitize_text(const char* src, size_t src_len, char* out, size_t cap) {
    size_t n = src_len < cap - 1 ? src_len : cap - 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == ',' || c == '\r' || c == '\n' || c < 0x20) out[i] = ' ';
        else out[i] = (char)c;
    }
    out[n] = 0;
}

static size_t trim_eol(char* line) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    return n;
}

static bool parse_i64(const char* s, int64_t* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE || !end || *end != 0) return false;
    *out = (int64_t)v;
    return true;
}

static bool parse_u32(const char* s, uint32_t* out) {
    int64_t v = 0;
    if (!parse_i64(s, &v)) return false;
    if (v < 0 || v > 0xFFFFFFFFLL) return false;
    *out = (uint32_t)v;
    return true;
}

/* 按 ',' 就地切分；返回实际字段数，只保存前 max 段（多余字段忽略，Q1 向前兼容）。
   / Split on ',' in place; returns the real field count, keeps the first max
   parts, extra fields are ignored (Q1 forward compatibility). */
static int split_commas(char* line, char** parts, int max) {
    int n = 0;
    char* p = line;
    for (;;) {
        if (n < max) parts[n] = p;
        ++n;
        char* comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    return n;
}

typedef struct {
    int64_t id;
    uint32_t v[7];  /* type,queue,due,ivl,factor,reps,lapses */
} card_row_t;

static bool parse_c_line(char* line, card_row_t* out) {
    char* parts[10] = {0};
    int n = split_commas(line, parts, 10);
    if (n < 9) return false;
    memset(out, 0, sizeof(*out));
    if (!parse_i64(parts[1], &out->id) || out->id <= 0) return false;
    for (int i = 0; i < 7; ++i) {
        if (!parse_u32(parts[2 + i], &out->v[i])) return false;
    }
    return true;
}

static const uint32_t k_card_field_off[7] = {
    AKD_CARD_OFF_TYPE, AKD_CARD_OFF_QUEUE, AKD_CARD_OFF_DUE, AKD_CARD_OFF_IVL,
    AKD_CARD_OFF_FACTOR, AKD_CARD_OFF_REPS, AKD_CARD_OFF_LAPSES
};

/* 只 patch 7 个调度字段：id/nid/ord/reserved 与卡片表之外一切字节不动。
   / Patch only the 7 scheduling fields; id/nid/ord/reserved and everything
   outside the card table stay byte-identical. */
static void patch_record(uint8_t* rec, const card_row_t* row) {
    for (int i = 0; i < 7; ++i) wr_u32(rec + k_card_field_off[i], row->v[i]);
}

typedef struct {
    int64_t deck_id;
    int64_t day;
    int64_t last_rtc_day;
    int card_count;
    bool have_version;
    bool have_deck_id;
    bool have_day;
    bool have_rtc;
    bool have_count;
    bool have_end;
    bool have_sum;
    uint32_t sum_expected;
    uint32_t sum_calc;
    int c_rows;
    bool have_r;
    int r_count;
    int r_pos;
    int r_studied;
    int r_again;
    int r_good;
    int64_t r_day;
    uint32_t r_root_tag;
    uint32_t r_fingerprint;
    int64_t q_ids[ANKI_SESSION_MAX];
    int q_count;
} backup_meta_t;

/* 流式逐行解析 + 全量校验；不把整份文本读进内存。
   / Streaming line parse + full validation; the text never enters memory. */
static esp_err_t parse_backup(FILE* f, backup_meta_t* m) {
    if (!f || !m) return ESP_ERR_INVALID_ARG;
    memset(m, 0, sizeof(*m));
    m->sum_calc = ANKI_FNV1A_BASIS;
    char line[ANKI_BACKUP_LINE_MAX + 2];
    char tmp[ANKI_BACKUP_LINE_MAX + 2];
    size_t bytes = 0;
    bool ended = false;
    while (fgets(line, sizeof(line), f)) {
        size_t got = strlen(line);
        bytes += got;
        if (bytes > ANKI_BACKUP_FILE_MAX) return ESP_ERR_INVALID_SIZE;
        if (got > 0 && line[got - 1] != '\n' && !feof(f)) return ESP_ERR_INVALID_SIZE;
        trim_eol(line);
        if (line[0] == '#' || line[0] == 0) continue;
        if (ended) {
            if (strncmp(line, "SUM,", 4) != 0) return ESP_ERR_INVALID_SIZE;
        }
        if (strcmp(line, "END") == 0) {
            if (m->have_end) return ESP_ERR_INVALID_SIZE;
            m->have_end = true;
            ended = true;
            continue;
        }
        if (strncmp(line, "C,", 2) == 0) {
            if (ended) return ESP_ERR_INVALID_SIZE;
            // SUM 覆盖 C 行内容字节（不含换行），导出端用同一规则。
            // SUM covers the C line bytes without the newline; export matches.
            m->sum_calc = anki_fnv1a(line, strlen(line), m->sum_calc);
            snprintf(tmp, sizeof(tmp), "%s", line);
            card_row_t row = {0};
            if (!parse_c_line(tmp, &row)) return ESP_ERR_INVALID_SIZE;
            ++m->c_rows;
            if (m->c_rows > ANKI_RESTORE_CARD_MAX) return ESP_ERR_NOT_SUPPORTED;
            continue;
        }
        if (strncmp(line, "SUM,", 4) == 0) {
            snprintf(tmp, sizeof(tmp), "%s", line);
            char* parts[4] = {0};
            if (split_commas(tmp, parts, 4) < 2) return ESP_ERR_INVALID_SIZE;
            errno = 0;
            char* end = NULL;
            unsigned long v = strtoul(parts[1], &end, 16);
            if (errno == ERANGE || !end || *end != 0) return ESP_ERR_INVALID_SIZE;
            m->sum_expected = (uint32_t)v;
            m->have_sum = true;
            continue;
        }
        if (strncmp(line, "R,", 2) == 0) {
            snprintf(tmp, sizeof(tmp), "%s", line);
            char* parts[12] = {0};
            if (split_commas(tmp, parts, 12) < 9) return ESP_ERR_INVALID_SIZE;
            int64_t v[6] = {0};
            for (int i = 0; i < 6; ++i) {
                if (!parse_i64(parts[1 + i], &v[i])) return ESP_ERR_INVALID_SIZE;
            }
            uint32_t tag = 0;
            if (!parse_u32(parts[7], &tag) || tag > 1) return ESP_ERR_INVALID_SIZE;
            errno = 0;
            char* end = NULL;
            unsigned long fp = strtoul(parts[8], &end, 16);
            if (errno == ERANGE || !end || *end != 0) return ESP_ERR_INVALID_SIZE;
            if (v[0] <= 0 || v[0] > ANKI_SESSION_MAX) return ESP_ERR_INVALID_SIZE;
            if (v[1] < 0 || v[1] >= v[0]) return ESP_ERR_INVALID_SIZE;
            if (v[2] < 0 || v[3] < 0 || v[4] < 0) return ESP_ERR_INVALID_SIZE;
            if (v[5] < 0 || v[5] > ANKI_BACKUP_DAY_MAX) return ESP_ERR_INVALID_SIZE;
            m->have_r = true;
            m->r_count = (int)v[0];
            m->r_pos = (int)v[1];
            m->r_studied = (int)v[2];
            m->r_again = (int)v[3];
            m->r_good = (int)v[4];
            m->r_day = v[5];
            m->r_root_tag = tag;
            m->r_fingerprint = (uint32_t)fp;
            continue;
        }
        if (strncmp(line, "Q,", 2) == 0) {
            if (m->q_count >= ANKI_SESSION_MAX) return ESP_ERR_INVALID_SIZE;
            snprintf(tmp, sizeof(tmp), "%s", line);
            char* parts[4] = {0};
            if (split_commas(tmp, parts, 4) < 2) return ESP_ERR_INVALID_SIZE;
            int64_t id = 0;
            if (!parse_i64(parts[1], &id) || id <= 0) return ESP_ERR_INVALID_SIZE;
            m->q_ids[m->q_count++] = id;
            continue;
        }
        /* KV 头：按首个 '=' 切分；未知键一律忽略（Q1 向前兼容）。
           / KV header: split on the first '='; unknown keys are ignored. */
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char* key = line;
            const char* val = eq + 1;
            int64_t v = 0;
            if (strcmp(key, "akb_version") == 0) {
                if (!parse_i64(val, &v)) return ESP_ERR_INVALID_SIZE;
                if (v != ANKI_BACKUP_FORMAT_VERSION) {
                    return v > ANKI_BACKUP_FORMAT_VERSION ? ESP_ERR_NOT_SUPPORTED
                                                          : ESP_ERR_INVALID_VERSION;
                }
                m->have_version = true;
            } else if (strcmp(key, "deck_id") == 0) {
                if (!parse_i64(val, &v) || v <= 0) return ESP_ERR_INVALID_SIZE;
                m->deck_id = v;
                m->have_deck_id = true;
            } else if (strcmp(key, "day") == 0) {
                if (!parse_i64(val, &v) || v < 0 || v > ANKI_BACKUP_DAY_MAX) {
                    return ESP_ERR_INVALID_SIZE;
                }
                m->day = v;
                m->have_day = true;
            } else if (strcmp(key, "last_rtc_day") == 0) {
                if (!parse_i64(val, &v) || v < 0 || v > ANKI_BACKUP_DAY_MAX) {
                    return ESP_ERR_INVALID_SIZE;
                }
                m->last_rtc_day = v;
                m->have_rtc = true;
            } else if (strcmp(key, "card_count") == 0) {
                if (!parse_i64(val, &v) || v < 0) return ESP_ERR_INVALID_SIZE;
                if (v > ANKI_RESTORE_CARD_MAX) return ESP_ERR_NOT_SUPPORTED;
                m->card_count = (int)v;
                m->have_count = true;
            }
            /* deck_name / source / ext1 / ext2 与未知键：一律忽略，仅供人读。 */
            continue;
        }
        /* 未知行前缀一律忽略。/ Unknown line prefixes are ignored. */
    }
    if (ferror(f)) return ESP_FAIL;
    if (!m->have_version || !m->have_deck_id || !m->have_day || !m->have_rtc ||
        !m->have_count || !m->have_end || !m->have_sum) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (m->c_rows != m->card_count) return ESP_ERR_INVALID_SIZE;
    if (m->sum_calc != m->sum_expected) return ESP_ERR_INVALID_SIZE;
    if (m->have_r && m->q_count != m->r_count) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

typedef struct {
    int64_t deck_id;
    bool found;
} deck_lookup_t;

static bool deck_visit(const anki_deck_summary_t* s, void* ctx) {
    deck_lookup_t* l = (deck_lookup_t*)ctx;
    if (s->deck_id == l->deck_id) {
        l->found = true;
        return false;
    }
    return true;
}

/* 恢复前提：deck_id 必须命中设备已有卡组，绝不凭空创建卡组。
   / Restore precondition: deck_id must hit an existing deck; never create one. */
static esp_err_t resolve_deck_file(int64_t deck_id, char* out, size_t cap,
                                  char* out_root, size_t root_cap, uint32_t* out_tag) {
    deck_lookup_t lookup = {deck_id, false};
    esp_err_t err = anki_store_list(deck_visit, &lookup);
    if (err != ESP_OK) return err;
    if (!lookup.found) return ESP_ERR_NOT_FOUND;
    const char* active = anki_store_active_root();
    const char* other = strcmp(active, ANKI_DIR_TF) == 0 ? ANKI_DIR : ANKI_DIR_TF;
    const char* roots[2] = {active, other};
    for (int i = 0; i < 2; ++i) {
        char p[PROGRESS_PATH_MAX];
        deck_file_path(roots[i], deck_id, ".akd", p, sizeof(p));
        size_t sz = 0;
        if (!file_size_of(p, &sz) || sz == 0 || sz > ANKI_DECK_FILE_MAX) continue;
        snprintf(out, cap, "%s", p);
        if (out_root && root_cap > 0) snprintf(out_root, root_cap, "%s", roots[i]);
        if (out_tag) *out_tag = tag_of_root(roots[i]);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* 快速路径：备份 C 段按 .akd 卡片表顺序导出，逐格核对 card_id，零额外大数组。
   / Fast path: C rows follow the .akd table order; verify ids slot by slot. */
static bool fast_path_aligned(FILE* f, const uint8_t* buf, const akd_header_t* h) {
    rewind(f);
    char line[ANKI_BACKUP_LINE_MAX + 2];
    char tmp[ANKI_BACKUP_LINE_MAX + 2];
    int k = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (strncmp(line, "C,", 2) != 0) continue;
        if (k >= (int)h->card_count) return false;
        snprintf(tmp, sizeof(tmp), "%s", line);
        card_row_t row = {0};
        if (!parse_c_line(tmp, &row)) return false;
        const uint8_t* rec = buf + h->table_at + (size_t)k * AKD_CARD_RECORD;
        if (rd_i64(rec + AKD_CARD_OFF_ID) != row.id) return false;
        ++k;
    }
    return k == (int)h->card_count;
}

static esp_err_t patch_fast(FILE* f, uint8_t* buf, const akd_header_t* h, int* covered) {
    rewind(f);
    char line[ANKI_BACKUP_LINE_MAX + 2];
    char tmp[ANKI_BACKUP_LINE_MAX + 2];
    int k = 0;
    *covered = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (strncmp(line, "C,", 2) != 0) continue;
        snprintf(tmp, sizeof(tmp), "%s", line);
        card_row_t row = {0};
        if (!parse_c_line(tmp, &row)) return ESP_ERR_INVALID_SIZE;
        if (k >= (int)h->card_count) return ESP_ERR_INVALID_SIZE;
        uint8_t* rec = buf + h->table_at + (size_t)k * AKD_CARD_RECORD;
        if (rd_i64(rec + AKD_CARD_OFF_ID) != row.id) return ESP_ERR_INVALID_SIZE;
        patch_record(rec, &row);
        ++k;
        ++*covered;
    }
    return k == (int)h->card_count ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static int cmp_row(const void* a, const void* b) {
    int64_t x = ((const card_row_t*)a)->id;
    int64_t y = ((const card_row_t*)b)->id;
    return (x > y) - (x < y);
}

static card_row_t* find_row(card_row_t* rows, int n, int64_t id) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (rows[mid].id == id) return &rows[mid];
        if (rows[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* 宽松路径：一次性 card_id 索引 + qsort/二分，按 id 匹配覆盖，拒绝整库盲覆盖（Q8）。
   / Loose path: one-shot card_id index + qsort/binary search; never blind-overwrite. */
static esp_err_t patch_loose(FILE* f, uint8_t* buf, const akd_header_t* h,
                             int card_count, int* covered, int* skipped) {
    *covered = 0;
    *skipped = 0;
    if (card_count <= 0) return ESP_OK;
    card_row_t* rows = progress_alloc(sizeof(card_row_t) * (size_t)card_count);
    uint8_t* used = progress_alloc((size_t)card_count);
    if (!rows || !used) {
        free(rows);
        free(used);
        return ESP_ERR_NO_MEM;
    }
    // 0xAA 约定：索引与命中标记先整块清零再写。
    memset(rows, 0, sizeof(card_row_t) * (size_t)card_count);
    memset(used, 0, (size_t)card_count);
    rewind(f);
    char line[ANKI_BACKUP_LINE_MAX + 2];
    char tmp[ANKI_BACKUP_LINE_MAX + 2];
    int n = 0;
    esp_err_t err = ESP_OK;
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (strncmp(line, "C,", 2) != 0) continue;
        if (n >= card_count) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        snprintf(tmp, sizeof(tmp), "%s", line);
        if (!parse_c_line(tmp, &rows[n])) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        ++n;
    }
    if (err == ESP_OK && n != card_count) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        qsort(rows, (size_t)n, sizeof(card_row_t), cmp_row);
        int device_missed = 0;
        for (uint32_t k = 0; k < h->card_count; ++k) {
            uint8_t* rec = buf + h->table_at + (size_t)k * AKD_CARD_RECORD;
            card_row_t* hit = find_row(rows, n, rd_i64(rec + AKD_CARD_OFF_ID));
            if (!hit) {
                ++device_missed;
                continue;
            }
            patch_record(rec, hit);
            used[hit - rows] = 1;
            ++*covered;
        }
        int backup_missed = 0;
        for (int i = 0; i < n; ++i) {
            if (!used[i]) ++backup_missed;
        }
        // U4：设备有备份没有 + 备份有设备没有，两侧对不上的都计入 skipped。
        *skipped = device_missed + backup_missed;
    }
    free(rows);
    free(used);
    return err;
}

/* 与 store 层 sync_day/list 的日推进规则一致，用于把恢复后的 .aks 日基准对齐（R-1）。
   / Same day-advance rule as store's sync_day/list, aligning the rewritten
   .aks with the restored .akd header (R-1). */
static int64_t progress_effective_day(int64_t file_day, int64_t last_rtc_day) {
    int64_t today = anki_day_now();
    if (today > last_rtc_day && last_rtc_day > 0) return file_day + (today - last_rtc_day);
    if (last_rtc_day == 0 && today > 0) return today;
    return file_day;
}

/* E10：备份含 R/Q 段则重写 .aks，否则 clear；失败只记日志（便利数据可丢）。
   / E10: rewrite .aks from R/Q, or clear it; failure is non-fatal. */
static void restore_resume_state(const char* root, int64_t deck_id,
                                 uint32_t device_card_count, uint32_t root_tag,
                                 const backup_meta_t* m) {
    if (!m->have_r) {
        anki_store_resume_clear(deck_id);
        return;
    }
    uint8_t sb[ANKI_RESUME_BYTES];
    memset(sb, 0, sizeof(sb));  // 0xAA 约定：整块清零后再显式序列化
    memcpy(sb + 0, ANKI_STATE_MAGIC, 4);
    wr_u32(sb + 4, ANKI_STATE_VERSION);
    wr_u32(sb + 8, ANKI_RESUME_BYTES);
    wr_i64(sb + 16, deck_id);
    wr_u32(sb + 24, root_tag);
    wr_u32(sb + 28, device_card_count);
    wr_u32(sb + 32, (uint32_t)m->r_count);
    wr_u32(sb + 36, (uint32_t)m->r_pos);
    wr_u32(sb + 40, (uint32_t)m->r_studied);
    wr_u32(sb + 44, (uint32_t)m->r_again);
    wr_u32(sb + 48, (uint32_t)m->r_good);
    wr_i64(sb + 52, progress_effective_day(m->day, m->last_rtc_day));
    wr_u32(sb + 60, m->r_fingerprint);
    for (int i = 0; i < m->r_count; ++i) {
        wr_i64(sb + 64 + (size_t)i * 8, m->q_ids[i]);
    }
    wr_u32(sb + 12, anki_fnv1a(sb + 16, ANKI_RESUME_BYTES - 16, ANKI_FNV1A_BASIS));
    char path[PROGRESS_PATH_MAX];
    deck_file_path(root, deck_id, ANKI_STATE_SUFFIX, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "resume rewrite failed");
        return;
    }
    size_t wrote = fwrite(sb, 1, sizeof(sb), f);
    bool closed = fclose(f) == 0;
    if (wrote != sizeof(sb) || !closed) {
        ESP_LOGW(TAG, "resume rewrite truncated");
        remove(path);
    }
}

static bool emit_line(FILE* f, size_t* written, const char* text) {
    size_t n = strlen(text);
    if (*written + n > ANKI_BACKUP_FILE_MAX) return false;
    if (fputs(text, f) == EOF) return false;
    *written += n;
    return true;
}

esp_err_t anki_progress_export(int64_t deck_id, char* out_path, size_t out_path_len) {
    if (out_path && out_path_len > 0) out_path[0] = 0;
    if (deck_id <= 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = book_store_ensure_flash();
    if (err != ESP_OK) return err;
    // Q3：统一活动根，不实现「无 TF 卡禁用」分支。
    // Q3: always the active root; no TF-card gating branch.
    const char* root = anki_store_active_root();
    char akd_path[PROGRESS_PATH_MAX];
    char aks_path[PROGRESS_PATH_MAX];
    char tmp_path[PROGRESS_PATH_MAX];
    char final_path[PROGRESS_PATH_MAX];
    deck_file_path(root, deck_id, ".akd", akd_path, sizeof(akd_path));
    deck_file_path(root, deck_id, ANKI_STATE_SUFFIX, aks_path, sizeof(aks_path));
    deck_file_path(root, deck_id, ANKI_BACKUP_TMP_SUFFIX, tmp_path, sizeof(tmp_path));
    deck_file_path(root, deck_id, ANKI_BACKUP_SUFFIX, final_path, sizeof(final_path));

    size_t len = 0;
    uint8_t* buf = read_whole_psram(akd_path, &len);
    if (!buf) return ESP_ERR_NOT_FOUND;
    akd_header_t h = {0};
    err = akd_parse(buf, len, &h);
    if (err != ESP_OK || h.deck_id != deck_id) {
        free(buf);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    FILE* f = fopen(tmp_path, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }
    size_t written = 0;
    uint32_t sum = ANKI_FNV1A_BASIS;
    char line[ANKI_BACKUP_LINE_MAX + 2];
    char safe_name[ANKI_BACKUP_NAME_MAX];
    char safe_source[ANKI_BACKUP_NAME_MAX];
    sanitize_text(h.name, h.name_len, safe_name, sizeof(safe_name));
    uint32_t source_len = rd_u32(buf + AKD_OFF_SOURCE_LEN);
    size_t src_off = (size_t)AKD_HEADER_BYTES + h.name_len + 1;
    const char* source = "";
    if (src_off + source_len + 1 <= len) source = (const char*)(buf + src_off);
    sanitize_text(source, source_len, safe_source, sizeof(safe_source));

    bool emit_ok = true;
    emit_ok = emit_ok && emit_line(f, &written,
        "# kiikoread Anki progress backup (UTF-8)\n");
    snprintf(line, sizeof(line), "akb_version=%d\n", ANKI_BACKUP_FORMAT_VERSION);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "deck_id=%lld\n", (long long)deck_id);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "deck_name=%.64s\n", safe_name);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "source=%.64s\n", safe_source);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "day=%lld\n", (long long)h.day);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "last_rtc_day=%lld\n", (long long)h.last_rtc_day);
    emit_ok = emit_ok && emit_line(f, &written, line);
    snprintf(line, sizeof(line), "card_count=%u\n", (unsigned)h.card_count);
    emit_ok = emit_ok && emit_line(f, &written, line);
    emit_ok = emit_ok && emit_line(f, &written, "ext1=0\next2=0\n");
    emit_ok = emit_ok && emit_line(f, &written,
        "# C: id,type,queue,due,ivl,factor,reps,lapses\n");

    for (uint32_t k = 0; emit_ok && k < h.card_count; ++k) {
        const uint8_t* rec = buf + h.table_at + (size_t)k * AKD_CARD_RECORD;
        uint32_t v[7];
        for (int j = 0; j < 7; ++j) v[j] = rd_u32(rec + k_card_field_off[j]);
        int n = snprintf(line, sizeof(line), "C,%lld,%u,%u,%u,%u,%u,%u,%u\n",
                         (long long)rd_i64(rec + AKD_CARD_OFF_ID),
                         (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3],
                         (unsigned)v[4], (unsigned)v[5], (unsigned)v[6]);
        if (n < 0 || (size_t)n >= sizeof(line)) {
            emit_ok = false;
            break;
        }
        // SUM 覆盖 C 行内容字节（不含换行），与恢复端一致。
        sum = anki_fnv1a(line, (size_t)n - 1, sum);
        emit_ok = emit_ok && emit_line(f, &written, line);
    }

    // Q10：续学状态随导出一起走（R 段 + Q 段）；.aks 无效则整段省略，
    // 恢复端据此清除续学状态。
    FILE* sf = fopen(aks_path, "rb");
    if (emit_ok && sf) {
        uint8_t sb[ANKI_RESUME_BYTES];
        memset(sb, 0, sizeof(sb));
        size_t got = fread(sb, 1, sizeof(sb), sf);
        fclose(sf);
        if (got == sizeof(sb) && memcmp(sb, ANKI_STATE_MAGIC, 4) == 0 &&
            rd_u32(sb + 4) == ANKI_STATE_VERSION &&
            rd_u32(sb + 8) == ANKI_RESUME_BYTES &&
            rd_u32(sb + 12) ==
                anki_fnv1a(sb + 16, ANKI_RESUME_BYTES - 16, ANKI_FNV1A_BASIS) &&
            rd_i64(sb + 16) == deck_id) {
            int cnt = (int32_t)rd_u32(sb + 32);
            if (cnt > 0 && cnt <= ANKI_SESSION_MAX) {
                int pos = (int32_t)rd_u32(sb + 36);
                int studied = (int32_t)rd_u32(sb + 40);
                int again = (int32_t)rd_u32(sb + 44);
                int good = (int32_t)rd_u32(sb + 48);
                int64_t rday = rd_i64(sb + 52);
                uint32_t tag = rd_u32(sb + 24);
                uint32_t fp = rd_u32(sb + 60);
                snprintf(line, sizeof(line), "R,%d,%d,%d,%d,%d,%lld,%u,%08x\n",
                         cnt, pos, studied, again, good, (long long)rday,
                         (unsigned)tag, (unsigned)fp);
                emit_ok = emit_ok && emit_line(f, &written, line);
                for (int i = 0; emit_ok && i < cnt; ++i) {
                    snprintf(line, sizeof(line), "Q,%lld\n",
                             (long long)rd_i64(sb + 64 + (size_t)i * 8));
                    emit_ok = emit_ok && emit_line(f, &written, line);
                }
            }
        }
    } else if (sf) {
        fclose(sf);
    }

    if (emit_ok) emit_ok = emit_line(f, &written, "END\n");
    if (emit_ok) {
        snprintf(line, sizeof(line), "SUM,%08x\n", (unsigned)sum);
        emit_ok = emit_line(f, &written, line);
    }
    bool flushed = fflush(f) == 0;
    bool closed = fclose(f) == 0;
    free(buf);
    if (!emit_ok || !flushed || !closed || written == 0) {
        remove(tmp_path);
        return ESP_FAIL;
    }
    // tmp + rename 原子落成一个文件；失败不留会被误认成有效备份的半截文件。
    unlink(final_path);
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (out_path && out_path_len > 0) {
        snprintf(out_path, out_path_len, "%.270s", final_path);
    }
    return ESP_OK;
}

esp_err_t anki_progress_list_backups(anki_backup_visit_fn visit, void* ctx) {
    if (!visit) return ESP_ERR_INVALID_ARG;
    (void)book_store_ensure_flash();
    const char* active = anki_store_active_root();
    const char* other = strcmp(active, ANKI_DIR_TF) == 0 ? ANKI_DIR : ANKI_DIR_TF;
    const char* roots[2] = {active, other};
    // 按文件名的 FNV-1a 去重（活动根优先）；栈上 96×4 B，零堆分配。
    // Dedup by filename hash (active root first); 96x4 B on the stack.
    uint32_t seen[ANKI_BACKUP_SEEN_MAX] = {0};
    int seen_count = 0;
    for (int r = 0; r < 2; ++r) {
        DIR* dir = opendir(roots[r]);
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || !suffix_eq(dot, ANKI_BACKUP_SUFFIX)) continue;
            if (entry->d_name[0] == '.') continue;  // AppleDouble / hidden
            uint32_t key = anki_fnv1a(entry->d_name, strlen(entry->d_name),
                                      ANKI_FNV1A_BASIS);
            bool duplicate = false;
            for (int i = 0; i < seen_count; ++i) {
                if (seen[i] == key) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            char path[PROGRESS_PATH_MAX];
            // 用户可能自行改名备份文件；按整段文件名拼接并检查长度，不截断。
            // Users may rename backups; keep the whole filename and bound it.
            if (snprintf(path, sizeof(path), "%s/%s", roots[r], entry->d_name) >=
                (int)sizeof(path)) {
                continue;
            }
            size_t sz = 0;
            if (!file_size_of(path, &sz) || sz > ANKI_BACKUP_FILE_MAX) continue;
            if (seen_count < ANKI_BACKUP_SEEN_MAX) seen[seen_count++] = key;
            if (!visit(path, entry->d_name, ctx)) {
                closedir(dir);
                return ESP_OK;
            }
        }
        closedir(dir);
    }
    return ESP_OK;
}

esp_err_t anki_progress_restore(const char* backup_path, anki_progress_result_t* out) {
    if (!backup_path || !*backup_path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    esp_err_t err = book_store_ensure_flash();
    if (err != ESP_OK) return err;
    // 先打开再量尺寸：不存在的备份按 §2.5 返回 NOT_FOUND，且避免 TOCTOU。
    // Open before stat: a missing backup is NOT_FOUND (§2.5), no TOCTOU.
    FILE* f = fopen(backup_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t backup_size = 0;
    if (!file_size_of(backup_path, &backup_size) || backup_size == 0 ||
        backup_size > ANKI_BACKUP_FILE_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    backup_meta_t m = {0};
    err = parse_backup(f, &m);
    if (err != ESP_OK) {
        fclose(f);
        return err;  // 未写盘，设备进度原样保留
    }

    char akd_path[PROGRESS_PATH_MAX] = {0};
    char root[PROGRESS_PATH_MAX] = {0};
    uint32_t deck_root_tag = 0;
    err = resolve_deck_file(m.deck_id, akd_path, sizeof(akd_path), root, sizeof(root),
                            &deck_root_tag);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    size_t len = 0;
    uint8_t* buf = read_whole_psram(akd_path, &len);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    akd_header_t h = {0};
    err = akd_parse(buf, len, &h);
    if (err != ESP_OK || h.deck_id != m.deck_id) {
        free(buf);
        fclose(f);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    char deck_name[ANKI_DECK_NAME_MAX] = {0};
    if (h.name && h.name_len < ANKI_DECK_NAME_MAX) {
        memcpy(deck_name, h.name, h.name_len);
        deck_name[h.name_len] = 0;
    }

    /* R-1 / I1 的准确定义：除头部 day(@16)/last_rtc_day(@24) 与卡片表 7 个
       调度字段外，其余字节逐字节不变。理由：anki_store_session() 用
       card->due <= day 判断到期、anki_schedule() 用 day + ivl 生成新 due，而
       day 来自 .akd 头 @16（last_rtc_day @24）。只恢复 due 而不同步日基准，
       换机/重刷后若新设备 day=0（无 RTC），所有 due≈19748 的复习卡会永远
       不到期——正是本功能要防的失败模式。备份的 day/last_rtc_day 已通过
       parse_backup 范围校验，这里实际写回 PSRAM 副本。
       R-1/I1: only the header day/last_rtc_day and the 7 card scheduling
       fields change; everything else stays byte-identical. Without syncing
       the day base a fresh device (day=0) would make every review card
       permanently not-due. Backup values were range-checked at parse time. */
    wr_i64(buf + AKD_OFF_DAY, m.day);
    wr_i64(buf + AKD_OFF_LAST_RTC_DAY, m.last_rtc_day);

    int covered = 0, skipped = 0;
    if (m.card_count == 0) {
        err = ESP_OK;
    } else if (fast_path_aligned(f, buf, &h)) {
        err = patch_fast(f, buf, &h, &covered);
    } else {
        err = patch_loose(f, buf, &h, m.card_count, &covered, &skipped);
    }
    fclose(f);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    // 整块写 .akd.tmp；绝不逐格 patch 磁盘。
    // Write the whole buffer to .akd.tmp; never patch the disk slot by slot.
    char tmp_path[PROGRESS_PATH_MAX];
    char bak_path[PROGRESS_PATH_MAX];
    deck_file_path(root, m.deck_id, ANKI_DECK_TMP_SUFFIX, tmp_path, sizeof(tmp_path));
    deck_file_path(root, m.deck_id, ANKI_DECK_BAK_SUFFIX, bak_path, sizeof(bak_path));
    FILE* tf = fopen(tmp_path, "wb");
    if (!tf) {
        free(buf);
        return ESP_FAIL;
    }
    size_t wrote = fwrite(buf, 1, len, tf);
    bool closed = fclose(tf) == 0;
    free(buf);
    if (wrote != len || !closed) {
        remove(tmp_path);
        return ESP_FAIL;
    }

    // 双文件提交：不依赖 rename 覆盖语义（FatFs 目标存在时返回 FR_EXIST）。
    // Two-step commit; no reliance on rename-overwrite semantics.
    unlink(bak_path);
    if (rename(akd_path, bak_path) != 0) {
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, akd_path) != 0) {
        (void)rename(bak_path, akd_path);  // 回到完整旧副本
        remove(tmp_path);
        return ESP_FAIL;
    }
    (void)unlink(bak_path);  // 失败忽略：完整旧副本留待 recover() 清理

    restore_resume_state(root, m.deck_id, h.card_count, deck_root_tag, &m);

    out->deck_id = m.deck_id;
    snprintf(out->name, sizeof(out->name), "%s", deck_name);
    out->covered = covered;
    out->skipped = skipped;
    return ESP_OK;
}

void anki_progress_recover(void) {
    (void)book_store_ensure_flash();
    const char* active = anki_store_active_root();
    const char* other = strcmp(active, ANKI_DIR_TF) == 0 ? ANKI_DIR : ANKI_DIR_TF;
    const char* roots[2] = {active, other};
    // 1) 恢复事务中间态自愈（§1.5 掉电点表）。
    for (int r = 0; r < 2; ++r) {
        DIR* dir = opendir(roots[r]);
        if (!dir) continue;
        int64_t ids[ANKI_RECOVER_ID_MAX] = {0};  // 0xAA 约定：显式清零
        int n = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, ".akb.tmp")) {
                char stale[PROGRESS_PATH_MAX];
                snprintf(stale, sizeof(stale), "%.200s/%.80s", roots[r], entry->d_name);
                unlink(stale);
                continue;
            }
            if (!strstr(entry->d_name, ".akd.tmp") &&
                !strstr(entry->d_name, ".akd.bak")) {
                continue;
            }
            if (n >= ANKI_RECOVER_ID_MAX) continue;
            int64_t id = strtoll(entry->d_name, NULL, 10);
            if (id <= 0) continue;
            bool duplicate = false;
            for (int i = 0; i < n; ++i) {
                if (ids[i] == id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) ids[n++] = id;
        }
        closedir(dir);
        for (int i = 0; i < n; ++i) {
            char akd[PROGRESS_PATH_MAX], tmp[PROGRESS_PATH_MAX], bak[PROGRESS_PATH_MAX];
            deck_file_path(roots[r], ids[i], ".akd", akd, sizeof(akd));
            deck_file_path(roots[r], ids[i], ANKI_DECK_TMP_SUFFIX, tmp, sizeof(tmp));
            deck_file_path(roots[r], ids[i], ANKI_DECK_BAK_SUFFIX, bak, sizeof(bak));
            size_t dummy = 0;
            bool has_akd = file_size_of(akd, &dummy);
            bool has_tmp = file_size_of(tmp, &dummy);
            bool has_bak = file_size_of(bak, &dummy);
            if (has_akd) {
                // ④/⑥ 之后：.akd 完好，删掉中间态。
                if (has_tmp) unlink(tmp);
                if (has_bak) unlink(bak);
            } else if (has_bak) {
                // ⑤ 之后：.tmp 校验通过则提交新，否则回滚旧；绝不混合。
                if (has_tmp && akd_file_valid(tmp)) {
                    if (rename(tmp, akd) != 0) (void)rename(bak, akd);
                    else unlink(bak);
                } else {
                    (void)rename(bak, akd);
                    if (has_tmp) unlink(tmp);
                }
            } else if (has_tmp) {
                unlink(tmp);  // 孤立中间态
            }
        }
    }
    // 2) 孤儿 .aks：两根都无对应 .akd 才删，避免 E9 双根切换误删。
    for (int r = 0; r < 2; ++r) {
        DIR* dir = opendir(roots[r]);
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || !suffix_eq(dot, ANKI_STATE_SUFFIX)) continue;
            int64_t id = strtoll(entry->d_name, NULL, 10);
            if (id <= 0) continue;
            bool has_deck = false;
            for (int q = 0; q < 2; ++q) {
                char akd[PROGRESS_PATH_MAX];
                deck_file_path(roots[q], id, ".akd", akd, sizeof(akd));
                size_t dummy = 0;
                if (file_size_of(akd, &dummy)) {
                    has_deck = true;
                    break;
                }
            }
            if (!has_deck) {
                char stale[PROGRESS_PATH_MAX];
                snprintf(stale, sizeof(stale), "%.200s/%.80s", roots[r], entry->d_name);
                unlink(stale);
            }
        }
        closedir(dir);
    }
}
