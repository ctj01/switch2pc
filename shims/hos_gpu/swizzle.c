#include "swizzle.h"

#include <string.h>

static uint32_t gob_offset(uint32_t x, uint32_t y) {
    /* x in [0, 64), y in [0, 8): the 16Bx2 sector order inside one GOB. */
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 +
           (x % 16);
}

uint64_t swizzle_offset(uint32_t x, uint32_t y, uint32_t stride, uint32_t block_height) {
    uint32_t gobs_per_row = stride / SWIZZLE_GOB_WIDTH;
    uint32_t gob_x = x / SWIZZLE_GOB_WIDTH;
    uint32_t gob_y = y / SWIZZLE_GOB_HEIGHT;
    uint32_t block_y = gob_y / block_height;
    uint64_t block_size = (uint64_t)SWIZZLE_GOB_SIZE * block_height;
    return ((uint64_t)block_y * gobs_per_row + gob_x) * block_size +
           (uint64_t)(gob_y % block_height) * SWIZZLE_GOB_SIZE +
           gob_offset(x % SWIZZLE_GOB_WIDTH, y % SWIZZLE_GOB_HEIGHT);
}

int swizzle_decode(const uint8_t *src, size_t src_size, uint8_t *dst, uint32_t row_bytes,
                   uint32_t height, uint32_t stride, uint32_t block_height) {
    if (stride < row_bytes || (stride % SWIZZLE_GOB_WIDTH) != 0 || block_height == 0) {
        return -1;
    }
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t xb = 0; xb < row_bytes; xb += 16) {
            uint64_t off = swizzle_offset(xb, y, stride, block_height);
            size_t n = (row_bytes - xb) < 16 ? (row_bytes - xb) : 16;
            if (off + n > src_size) {
                return -1;
            }
            memcpy(dst + (size_t)y * row_bytes + xb, src + off, n);
        }
    }
    return 0;
}
