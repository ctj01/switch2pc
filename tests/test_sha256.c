/* SHA-256 against the FIPS 180-4 / NIST example vectors. */
#include <stdlib.h>
#include <string.h>

#include "common/sha256.h"
#include "test.h"

static void hex(const uint8_t d[SHA256_DIGEST_SIZE], char out[65]) {
    static const char k[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i * 2] = k[d[i] >> 4];
        out[i * 2 + 1] = k[d[i] & 15];
    }
    out[64] = 0;
}

static int digest_is(const void *data, size_t len, const char *expect) {
    uint8_t d[SHA256_DIGEST_SIZE];
    char h[65];
    sha256(data, len, d);
    hex(d, h);
    return strcmp(h, expect) == 0;
}

TEST(sha256_empty) {
    ASSERT_TRUE(
        digest_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST(sha256_abc) {
    ASSERT_TRUE(
        digest_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

/* 56 bytes: the padding spills into a second block. */
TEST(sha256_two_blocks) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ASSERT_TRUE(digest_is(m, strlen(m),
                          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

/* One million 'a', fed in uneven pieces to exercise the buffering. */
TEST(sha256_million_a_incremental) {
    size_t n = 1000000;
    char *m = malloc(n);
    memset(m, 'a', n);
    sha256_ctx c;
    sha256_init(&c);
    size_t off = 0, step = 1;
    while (off < n) {
        size_t take = step < n - off ? step : n - off;
        sha256_update(&c, m + off, take);
        off += take;
        step = step * 7 % 1013 + 1;
    }
    uint8_t d[SHA256_DIGEST_SIZE];
    char h[65];
    sha256_final(&c, d);
    hex(d, h);
    ASSERT_TRUE(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    ASSERT_TRUE(
        digest_is(m, n, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    free(m);
}
