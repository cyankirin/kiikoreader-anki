/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：最小 JSON 解析器实现：单遍解析进节点 arena，字符串原地反转义。
 * English: Minimal JSON parser: single pass into a node arena, strings
 * unescaped in place. All nodes are registered on the root so one free
 * releases the tree.
 */
#include "anki_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* key;
    size_t key_len;
    anki_json_value_t* value;
} json_member_t;

struct anki_json_value {
    anki_json_type_t type;
    // 仅根节点使用：解析器登记的全部节点，供一次释放。
    // Root only: every node the parser allocated, for a single release.
    anki_json_value_t** registry;
    size_t registry_count;
    union {
        bool boolean;
        double number;
        struct {
            char* text;
            size_t len;
        } string;
        struct {
            anki_json_value_t** items;
            size_t count;
        } array;
        struct {
            json_member_t* members;
            size_t count;
        } object;
    } as;
};

typedef struct {
    const char* p;
    const char* end;
    anki_json_value_t** nodes;
    size_t node_count;
    size_t node_cap;
    bool failed;
} json_parser_t;

static anki_json_value_t* node_new(json_parser_t* ps, anki_json_type_t type) {
    if (ps->failed || ps->node_count >= ANKI_JSON_MAX_NODES) {
        ps->failed = true;
        return NULL;
    }
    if (ps->node_count == ps->node_cap) {
        size_t cap = ps->node_cap ? ps->node_cap * 2 : 64;
        if (cap > ANKI_JSON_MAX_NODES) cap = ANKI_JSON_MAX_NODES;
        anki_json_value_t** grown = realloc(ps->nodes, cap * sizeof(*grown));
        if (!grown) {
            ps->failed = true;
            return NULL;
        }
        ps->nodes = grown;
        ps->node_cap = cap;
    }
    anki_json_value_t* v = calloc(1, sizeof(*v));
    if (!v) {
        ps->failed = true;
        return NULL;
    }
    v->type = type;
    ps->nodes[ps->node_count++] = v;
    return v;
}

/// 释放登记表中的全部节点及其挂载缓冲。/ Release every registered node and
/// its attached buffers.
static void free_registry(anki_json_value_t** nodes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        anki_json_value_t* v = nodes[i];
        if (!v) continue;
        switch (v->type) {
            case ANKI_JSON_STRING:
                free(v->as.string.text);
                break;
            case ANKI_JSON_OBJECT:
                for (size_t j = 0; j < v->as.object.count; ++j) {
                    free(v->as.object.members[j].key);
                }
                free(v->as.object.members);
                break;
            case ANKI_JSON_ARRAY:
                free(v->as.array.items);
                break;
            default:
                break;
        }
        free(v);
    }
    free(nodes);
}

static void skip_ws(json_parser_t* ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++ps->p;
        else break;
    }
}

static bool parse_value(json_parser_t* ps, int depth, anki_json_value_t** out);

static void utf8_encode(uint32_t cp, char** dst) {
    char* d = *dst;
    if (cp < 0x80) {
        *d++ = (char)cp;
    } else if (cp < 0x800) {
        *d++ = (char)(0xC0 | (cp >> 6));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *d++ = (char)(0xE0 | (cp >> 12));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *d++ = (char)(0xF0 | (cp >> 18));
        *d++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    }
    *dst = d;
}

static bool parse_hex4(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

/// 读一个 \uXXXX（含代理对）；ps->p 须指向 'u' 后的第一个十六进制位。
/// Read one \uXXXX escape including surrogate pairs; ps->p is at the first hex digit.
static bool parse_uescape(json_parser_t* ps, uint32_t* out) {
    if (ps->end - ps->p < 4) return false;
    uint32_t cp = 0;
    if (!parse_hex4(ps->p, &cp)) return false;
    ps->p += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') return false;
        uint32_t low = 0;
        if (!parse_hex4(ps->p + 2, &low)) return false;
        if (low < 0xDC00 || low > 0xDFFF) return false;
        ps->p += 6;
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return false; // 孤立低代理 / lone low surrogate
    }
    if (cp == 0) return false; // 内嵌 NUL 会破坏 C 字符串语义 / embedded NUL breaks C strings
    *out = cp;
    return true;
}

/// 解析 JSON 字符串字面量到调用方缓冲；*dst 前移。输入须以引号开头。
/// Parse a JSON string literal into the caller buffer; *dst advances. Input
/// must start at the opening quote.
static bool parse_string_raw(json_parser_t* ps, char** dst) {
    ++ps->p; // '"'
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ++ps->p;
            return true;
        }
        if (c < 0x20) return false;
        if (c != '\\') {
            *(*dst)++ = (char)c;
            ++ps->p;
            continue;
        }
        ++ps->p;
        if (ps->p >= ps->end) return false;
        char e = *ps->p++;
        switch (e) {
            case '"': *(*dst)++ = '"'; break;
            case '\\': *(*dst)++ = '\\'; break;
            case '/': *(*dst)++ = '/'; break;
            case 'b': *(*dst)++ = '\b'; break;
            case 'f': *(*dst)++ = '\f'; break;
            case 'n': *(*dst)++ = '\n'; break;
            case 'r': *(*dst)++ = '\r'; break;
            case 't': *(*dst)++ = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_uescape(ps, &cp)) return false;
                utf8_encode(cp, dst);
                break;
            }
            default: return false;
        }
    }
    return false;
}

static bool parse_string(json_parser_t* ps, char** out, size_t* out_len) {
    const char* start = ps->p;
    if (start >= ps->end || *start != '"') return false;
    const char* scan = start + 1;
    while (scan < ps->end) {
        unsigned char c = (unsigned char)*scan;
        if (c == '"') break;
        if (c < 0x20) return false;
        if (c == '\\') {
            ++scan;
            if (scan >= ps->end) return false;
            char e = *scan++;
            if (e == 'u') {
                if (ps->end - scan < 4) return false;
                uint32_t cp = 0;
                if (!parse_hex4(scan, &cp)) return false;
                scan += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - scan < 6 || scan[0] != '\\' || scan[1] != 'u') return false;
                    if (!parse_hex4(scan + 2, &cp)) return false;
                    scan += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                } else if (cp == 0) {
                    return false;
                }
            } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' &&
                       e != 'n' && e != 'r' && e != 't') {
                return false;
            }
        } else {
            ++scan;
        }
    }
    if (scan >= ps->end || *scan != '"') return false;
    size_t span = (size_t)(scan - start);
    char* buffer = malloc(span + 1); // 反转义只会缩短 / unescaping only shrinks
    if (!buffer) {
        ps->failed = true;
        return false;
    }
    char* dst = buffer;
    if (!parse_string_raw(ps, &dst)) {
        free(buffer);
        return false;
    }
    *dst = 0;
    *out = buffer;
    *out_len = (size_t)(dst - buffer);
    return true;
}

static bool parse_number(json_parser_t* ps, double* out) {
    const char* start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ++ps->p;
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return false;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    if (ps->p < ps->end && *ps->p == '.') {
        ++ps->p;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return false;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ++ps->p;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ++ps->p;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return false;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    }
    char tiny[64];
    size_t len = (size_t)(ps->p - start);
    if (len >= sizeof(tiny)) return false;
    memcpy(tiny, start, len);
    tiny[len] = 0;
    char* endp = NULL;
    *out = strtod(tiny, &endp);
    return endp == tiny + len;
}

static bool parse_array(json_parser_t* ps, int depth, anki_json_value_t* arr) {
    ++ps->p; // '['
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ++ps->p;
        return true;
    }
    size_t cap = 0;
    for (;;) {
        anki_json_value_t* item = NULL;
        if (!parse_value(ps, depth + 1, &item)) return false;
        if (arr->as.array.count == cap) {
            size_t grown_cap = cap ? cap * 2 : 8;
            anki_json_value_t** grown = realloc(arr->as.array.items, grown_cap * sizeof(*grown));
            if (!grown) {
                ps->failed = true;
                return false;
            }
            arr->as.array.items = grown;
            cap = grown_cap;
        }
        arr->as.array.items[arr->as.array.count++] = item;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ++ps->p;
            skip_ws(ps);
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ++ps->p;
            return true;
        }
        return false;
    }
}

static bool parse_object(json_parser_t* ps, int depth, anki_json_value_t* obj) {
    ++ps->p; // '{'
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ++ps->p;
        return true;
    }
    size_t cap = 0;
    for (;;) {
        char* key = NULL;
        size_t key_len = 0;
        if (!parse_string(ps, &key, &key_len)) return false;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(key);
            return false;
        }
        ++ps->p;
        skip_ws(ps);
        anki_json_value_t* value = NULL;
        if (!parse_value(ps, depth + 1, &value)) {
            free(key);
            return false;
        }
        if (obj->as.object.count == cap) {
            size_t grown_cap = cap ? cap * 2 : 8;
            json_member_t* grown = realloc(obj->as.object.members, grown_cap * sizeof(*grown));
            if (!grown) {
                free(key);
                ps->failed = true;
                return false;
            }
            obj->as.object.members = grown;
            cap = grown_cap;
        }
        json_member_t* m = &obj->as.object.members[obj->as.object.count++];
        m->key = key;
        m->key_len = key_len;
        m->value = value;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ++ps->p;
            skip_ws(ps);
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ++ps->p;
            return true;
        }
        return false;
    }
}

static bool parse_value(json_parser_t* ps, int depth, anki_json_value_t** out) {
    if (ps->failed) return false;
    if (depth > ANKI_JSON_MAX_DEPTH) return false;
    skip_ws(ps);
    if (ps->p >= ps->end) return false;
    char c = *ps->p;
    if (c == '{') {
        anki_json_value_t* v = node_new(ps, ANKI_JSON_OBJECT);
        if (!v) return false;
        if (!parse_object(ps, depth, v)) return false;
        *out = v;
        return true;
    }
    if (c == '[') {
        anki_json_value_t* v = node_new(ps, ANKI_JSON_ARRAY);
        if (!v) return false;
        if (!parse_array(ps, depth, v)) return false;
        *out = v;
        return true;
    }
    if (c == '"') {
        char* text = NULL;
        size_t len = 0;
        if (!parse_string(ps, &text, &len)) return false;
        anki_json_value_t* v = node_new(ps, ANKI_JSON_STRING);
        if (!v) {
            free(text);
            return false;
        }
        v->as.string.text = text;
        v->as.string.len = len;
        *out = v;
        return true;
    }
    if (c == 't' && ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        anki_json_value_t* v = node_new(ps, ANKI_JSON_BOOL);
        if (!v) return false;
        v->as.boolean = true;
        *out = v;
        return true;
    }
    if (c == 'f' && ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        anki_json_value_t* v = node_new(ps, ANKI_JSON_BOOL);
        if (!v) return false;
        v->as.boolean = false;
        *out = v;
        return true;
    }
    if (c == 'n' && ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        anki_json_value_t* v = node_new(ps, ANKI_JSON_NULL);
        if (!v) return false;
        *out = v;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        double number = 0;
        if (!parse_number(ps, &number)) return false;
        anki_json_value_t* v = node_new(ps, ANKI_JSON_NUMBER);
        if (!v) return false;
        v->as.number = number;
        *out = v;
        return true;
    }
    return false;
}

anki_json_value_t* anki_json_parse(const char* text, size_t len) {
    if (!text || len == 0) return NULL;
    json_parser_t ps = {
        .p = text,
        .end = text + len,
        .nodes = NULL,
        .node_count = 0,
        .node_cap = 0,
        .failed = false,
    };
    anki_json_value_t* root = NULL;
    if (!parse_value(&ps, 0, &root)) {
        free_registry(ps.nodes, ps.node_count);
        return NULL;
    }
    skip_ws(&ps);
    if (ps.failed || ps.p != ps.end) {
        free_registry(ps.nodes, ps.node_count);
        return NULL;
    }
    root->registry = ps.nodes;
    root->registry_count = ps.node_count;
    return root;
}

void anki_json_free(anki_json_value_t* root) {
    if (!root) return;
    free_registry(root->registry, root->registry_count);
}

anki_json_type_t anki_json_type(const anki_json_value_t* v) {
    return v ? v->type : ANKI_JSON_NULL;
}

const anki_json_value_t* anki_json_get(const anki_json_value_t* obj, const char* key) {
    if (!obj || obj->type != ANKI_JSON_OBJECT) return NULL;
    size_t n = strlen(key);
    for (size_t i = 0; i < obj->as.object.count; ++i) {
        const json_member_t* m = &obj->as.object.members[i];
        if (m->key_len == n && memcmp(m->key, key, m->key_len) == 0) return m->value;
    }
    return NULL;
}

size_t anki_json_member_count(const anki_json_value_t* obj) {
    if (!obj || obj->type != ANKI_JSON_OBJECT) return 0;
    return obj->as.object.count;
}

const char* anki_json_member_key(const anki_json_value_t* obj, size_t index, size_t* key_len) {
    if (!obj || obj->type != ANKI_JSON_OBJECT || index >= obj->as.object.count) return NULL;
    const json_member_t* m = &obj->as.object.members[index];
    if (key_len) *key_len = m->key_len;
    return m->key;
}

const anki_json_value_t* anki_json_member_value(const anki_json_value_t* obj, size_t index) {
    if (!obj || obj->type != ANKI_JSON_OBJECT || index >= obj->as.object.count) return NULL;
    return obj->as.object.members[index].value;
}

size_t anki_json_size(const anki_json_value_t* arr) {
    if (!arr || arr->type != ANKI_JSON_ARRAY) return 0;
    return arr->as.array.count;
}

const anki_json_value_t* anki_json_at(const anki_json_value_t* arr, size_t index) {
    if (!arr || arr->type != ANKI_JSON_ARRAY || index >= arr->as.array.count) return NULL;
    return arr->as.array.items[index];
}

const char* anki_json_string(const anki_json_value_t* v, size_t* len) {
    if (!v || v->type != ANKI_JSON_STRING) return NULL;
    if (len) *len = v->as.string.len;
    return v->as.string.text;
}

double anki_json_number(const anki_json_value_t* v) {
    if (!v || v->type != ANKI_JSON_NUMBER) return 0;
    return v->as.number;
}

bool anki_json_bool(const anki_json_value_t* v) {
    if (!v || v->type != ANKI_JSON_BOOL) return false;
    return v->as.boolean;
}
