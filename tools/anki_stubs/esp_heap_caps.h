/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 宿主测试的内存替身，PSRAM 请求映射到 malloc。
 * English: Memory stand-in for Anki host tests; PSRAM requests map to malloc.
 */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void* heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void* heap_caps_calloc(size_t n, size_t size, int caps) { (void)caps; return calloc(n, size); }
static inline void* heap_caps_realloc(void* p, size_t n, int caps) { (void)caps; return realloc(p, n); }
