#include <stdlib.h>

#include "nso/lz4.h"
#include "test.h"

/* token 0x44: 4 literals, match length 4+4; literals "ABCD"; offset 4; then the
 * closing literals-only sequence (token 0x00, no literals). */
static const uint8_t k_vector[] = {0x44, 'A', 'B', 'C', 'D', 0x04, 0x00, 0x00};

TEST(lz4_decode_hand_vector) {
    uint8_t out[32];
    int64_t n = lz4_block_decode(k_vector, sizeof(k_vector), out, sizeof(out));
    ASSERT_EQ(n, 12);
    ASSERT_MEM_EQ(out, "ABCDABCDABCD", 12);
}

TEST(lz4_decode_literals_only) {
    static const uint8_t src[] = {0x30, 'x', 'y', 'z'};
    uint8_t out[8];
    int64_t n = lz4_block_decode(src, sizeof(src), out, sizeof(out));
    ASSERT_EQ(n, 3);
    ASSERT_MEM_EQ(out, "xyz", 3);
}

TEST(lz4_decode_rejects_bad_offset) {
    static const uint8_t src[] = {0x44, 'A', 'B', 'C', 'D', 0x00, 0x00};
    uint8_t out[32];
    ASSERT_EQ(lz4_block_decode(src, sizeof(src), out, sizeof(out)), -1);
    static const uint8_t far[] = {0x44, 'A', 'B', 'C', 'D', 0x08, 0x00};
    ASSERT_EQ(lz4_block_decode(far, sizeof(far), out, sizeof(out)), -1);
}

TEST(lz4_decode_rejects_truncated) {
    uint8_t out[32];
    ASSERT_EQ(lz4_block_decode(k_vector, 3, out, sizeof(out)), -1);
    ASSERT_EQ(lz4_block_decode(k_vector, 6, out, sizeof(out)), -1);
}

/* The format ends every block with a literals-only sequence. A block cut right
 * after a match is truncated and must be rejected, not decoded short. */
TEST(lz4_decode_rejects_block_ending_in_match) {
    uint8_t out[32];
    ASSERT_EQ(lz4_block_decode(k_vector, sizeof(k_vector) - 1, out, sizeof(out)), -1);
    static const uint8_t ok[] = {0x44, 'A', 'B', 'C', 'D', 0x04, 0x00, 0x10, 'E'};
    ASSERT_EQ(lz4_block_decode(ok, sizeof(ok), out, sizeof(out)), 13);
    ASSERT_MEM_EQ(out, "ABCDABCDABCDE", 13);
    ASSERT_EQ(lz4_block_decode(ok, 0, out, sizeof(out)), -1);
    static const uint8_t empty[] = {0x00};
    ASSERT_EQ(lz4_block_decode(empty, sizeof(empty), out, sizeof(out)), 0);
}

TEST(lz4_decode_rejects_small_output) {
    uint8_t out[8];
    ASSERT_EQ(lz4_block_decode(k_vector, sizeof(k_vector), out, sizeof(out)), -1);
}

static void fill_pattern(uint8_t *p, size_t n) {
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) {
        /* repetitive with some noise: compressible, but not trivially */
        if ((i / 64) % 3 == 0) {
            p[i] = (uint8_t)(i & 0x0F);
        } else {
            x = x * 1103515245u + 12345u;
            p[i] = (uint8_t)((x >> 16) & 0x07);
        }
    }
}

TEST(lz4_round_trip_64k) {
    size_t n = 65536;
    uint8_t *src = malloc(n);
    uint8_t *enc = malloc(lz4_block_bound(n));
    uint8_t *dec = malloc(n);
    fill_pattern(src, n);

    int64_t e = lz4_block_encode(src, n, enc, lz4_block_bound(n));
    ASSERT_TRUE(e > 0);
    ASSERT_TRUE((size_t)e < n);
    int64_t d = lz4_block_decode(enc, (size_t)e, dec, n);
    ASSERT_EQ(d, (int64_t)n);
    ASSERT_MEM_EQ(src, dec, n);

    free(src);
    free(enc);
    free(dec);
}

TEST(lz4_round_trip_tiny_inputs) {
    uint8_t enc[64], dec[64];
    for (size_t n = 0; n < 20; n++) {
        uint8_t src[20];
        for (size_t i = 0; i < n; i++) {
            src[i] = (uint8_t)('a' + (i % 3));
        }
        int64_t e = lz4_block_encode(src, n, enc, sizeof(enc));
        ASSERT_TRUE(e >= 0);
        int64_t d = lz4_block_decode(enc, (size_t)e, dec, sizeof(dec));
        ASSERT_EQ(d, (int64_t)n);
        ASSERT_MEM_EQ(src, dec, n);
    }
}

TEST(lz4_encode_rejects_small_output) {
    uint8_t src[1000];
    uint8_t enc[8];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i * 7);
    }
    ASSERT_EQ(lz4_block_encode(src, sizeof(src), enc, sizeof(enc)), -1);
}
