/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：卡组导入用的最小 JSON 只读解析器。只建值树，不做指针回填。
 * English: Minimal read-only JSON parser for deck import. Builds a value tree
 * only; no pointer fixups.
 *
 * 冻结：只读；深度与节点数有界；字符串完整反转义（含代理对）。
 * Frozen: read-only; bounded depth and node count; strings fully unescaped
 * including surrogate pairs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANKI_JSON_MAX_DEPTH 32
#define ANKI_JSON_MAX_NODES 200000

typedef struct anki_json_value anki_json_value_t;

typedef enum {
    ANKI_JSON_NULL = 0,
    ANKI_JSON_BOOL,
    ANKI_JSON_NUMBER,
    ANKI_JSON_STRING,
    ANKI_JSON_ARRAY,
    ANKI_JSON_OBJECT,
} anki_json_type_t;

/// 解析 length 字节的 JSON；成功返回根节点，失败返回 NULL（含空输入）。
/// Parse len bytes of JSON; root on success, NULL on failure (empty input included).
anki_json_value_t* anki_json_parse(const char* text, size_t len);
/// 释放整棵树，允许 NULL。/ Release the whole tree; accepts NULL.
void anki_json_free(anki_json_value_t* root);

anki_json_type_t anki_json_type(const anki_json_value_t* v);

/// 对象按键取值，缺失或类型不符返回 NULL。/ Object member by key; NULL when absent.
const anki_json_value_t* anki_json_get(const anki_json_value_t* obj, const char* key);
/// 对象成员数。/ Member count of an object.
size_t anki_json_member_count(const anki_json_value_t* obj);
/// 第 index 个成员的键（NUL 结尾）；越界返回 NULL。/ Key of member index; NULL when out of range.
const char* anki_json_member_key(const anki_json_value_t* obj, size_t index, size_t* key_len);
/// 第 index 个成员的值；越界返回 NULL。/ Value of member index; NULL when out of range.
const anki_json_value_t* anki_json_member_value(const anki_json_value_t* obj, size_t index);
/// 数组元素数。/ Element count of an array.
size_t anki_json_size(const anki_json_value_t* arr);
/// 数组第 index 个元素，越界返回 NULL。/ Element at index; NULL when out of range.
const anki_json_value_t* anki_json_at(const anki_json_value_t* arr, size_t index);

/// 字符串值（已反转义、NUL 结尾）；非字符串返回 NULL。
/// String value (unescaped, NUL-terminated); NULL for other types.
const char* anki_json_string(const anki_json_value_t* v, size_t* len);
/// 数值；非数值返回 0。/ Number; 0 for other types.
double anki_json_number(const anki_json_value_t* v);
/// 布尔；非布尔返回 false。/ Boolean; false for other types.
bool anki_json_bool(const anki_json_value_t* v);

#ifdef __cplusplus
}
#endif
