#ifndef S2PC_NSO_LZ4_H
#define S2PC_NSO_LZ4_H

#include <stddef.h>
#include <stdint.h>

/**
 * Decode one raw LZ4 block (the "block format", no frame header). This is what
 * NSO segments use.
 *
 * @param src      Compressed bytes
 * @param src_len  Length of src
 * @param dst      Output buffer
 * @param dst_cap  Capacity of dst
 * @return Bytes written, or -1 if the block is malformed or does not fit
 */
int64_t lz4_block_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/**
 * Encode a buffer as one raw LZ4 block. Greedy, hash-table matcher. Output is
 * valid LZ4 and good enough for fixtures and tests; it is not tuned for ratio.
 *
 * @param src      Input bytes
 * @param src_len  Length of src
 * @param dst      Output buffer
 * @param dst_cap  Capacity of dst; use lz4_block_bound(src_len)
 * @return Bytes written, or -1 if dst is too small
 */
int64_t lz4_block_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/**
 * Worst-case encoded size for an input of `n` bytes.
 */
size_t lz4_block_bound(size_t n);

#endif /* S2PC_NSO_LZ4_H */
