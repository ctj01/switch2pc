#ifndef HOS_GPU_SWIZZLE_H
#define HOS_GPU_SWIZZLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Tegra block-linear layout ("16Bx2" GOBs). A GOB is 64 bytes wide and 8 rows
 * tall (512 bytes) with a fixed internal sector order; GOBs stack vertically
 * into blocks of `block_height` GOBs, and blocks are laid out row-major over
 * the surface. Display buffers use 16 GOBs per block.
 */

#define SWIZZLE_GOB_WIDTH  64
#define SWIZZLE_GOB_HEIGHT 8
#define SWIZZLE_GOB_SIZE   512

/**
 * Offset of byte (x, y) inside a block-linear surface.
 *
 * @param x             Byte column (pixel x * bytes per pixel)
 * @param y             Row
 * @param stride        Surface pitch in bytes (multiple of 64)
 * @param block_height  GOBs per block (16 for framebuffers)
 */
uint64_t swizzle_offset(uint32_t x, uint32_t y, uint32_t stride, uint32_t block_height);

/**
 * Convert a block-linear surface into a tightly packed linear image.
 *
 * @param src           Swizzled surface
 * @param src_size      Bytes available at src
 * @param dst           row_bytes * height bytes
 * @param row_bytes     Bytes per linear row (width * bytes per pixel)
 * @param height        Rows
 * @param stride        Swizzled pitch in bytes
 * @param block_height  GOBs per block
 * @return 0 on success, -1 if the geometry is invalid or src is too small
 */
int swizzle_decode(const uint8_t *src, size_t src_size, uint8_t *dst, uint32_t row_bytes,
                   uint32_t height, uint32_t stride, uint32_t block_height);

#endif /* HOS_GPU_SWIZZLE_H */
