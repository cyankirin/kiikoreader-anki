/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 仅为主机测试把分段 raw inflate 适配到 zlib。
 * Adapt chunked raw inflate to zlib for host tests only.
 * 冻结：不替换 ZIP 解析，不用于验证 ROM 实现。/ Frozen: Do not replace ZIP parsing or claim ROM validation.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
typedef unsigned char mz_uint8;
typedef unsigned int mz_uint32;
// ROM miniz 的状态由调用方一次释放；前缀探测可在流结束前退出。
// ROM miniz owns no secondary heap allocations; prefix probes may stop before stream end.
typedef struct {
    z_stream stream;
    int initialized;
    size_t used;
    _Alignas(max_align_t) unsigned char arena[65536];
} tinfl_decompressor;
static inline voidpf host_inflate_alloc(voidpf opaque, uInt count, uInt size) {
    tinfl_decompressor* state = opaque;
    if (size && count > SIZE_MAX / size) return Z_NULL;
    size_t bytes = (size_t)count * size;
    size_t alignment = _Alignof(max_align_t);
    size_t at = (state->used + alignment - 1) & ~(alignment - 1);
    if (at > sizeof(state->arena) || bytes > sizeof(state->arena) - at) return Z_NULL;
    state->used = at + bytes;
    return state->arena + at;
}
static inline void host_inflate_free(voidpf opaque, voidpf address) {
    // 内存随整个状态释放，不依赖测试对象额外的清理API。/ The whole state owns the arena, matching the production cleanup API.
    (void)opaque; (void)address;
}
typedef enum { TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0,
               TINFL_STATUS_NEEDS_MORE_INPUT = 1, TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define TINFL_FLAG_HAS_MORE_INPUT 2
#define tinfl_init(r) memset((r), 0, sizeof(*(r)))
static inline tinfl_status tinfl_decompress(tinfl_decompressor* state, const mz_uint8* input,
    size_t* in_size, mz_uint8* start, mz_uint8* output, size_t* out_size, mz_uint32 flags) {
    (void)start; (void)flags;
    if (!state->initialized) {
        state->used = 0;
        state->stream.zalloc = host_inflate_alloc;
        state->stream.zfree = host_inflate_free;
        state->stream.opaque = state;
        if (inflateInit2(&state->stream, -MAX_WBITS) != Z_OK) return TINFL_STATUS_FAILED;
        state->initialized = 1;
    }
    z_stream* stream = &state->stream;
    unsigned char empty;
    size_t available_in = *in_size, available_out = *out_size;
    stream->next_in = (Bytef*)input;
    stream->avail_in = (uInt)available_in;
    stream->next_out = available_out ? output : &empty;
    stream->avail_out = available_out ? (uInt)available_out : 1;
    int result = inflate(stream, Z_NO_FLUSH);
    *in_size = available_in - stream->avail_in;
    *out_size = available_out ? available_out - stream->avail_out : 0;
    if (!available_out && stream->avail_out != 1) result = Z_DATA_ERROR;
    if (result == Z_STREAM_END) { inflateEnd(stream); state->initialized = 0; return TINFL_STATUS_DONE; }
    if (result == Z_OK || result == Z_BUF_ERROR)
        return stream->avail_out == 0 ? TINFL_STATUS_HAS_MORE_OUTPUT : TINFL_STATUS_NEEDS_MORE_INPUT;
    inflateEnd(stream); state->initialized = 0;
    return TINFL_STATUS_FAILED;
}
