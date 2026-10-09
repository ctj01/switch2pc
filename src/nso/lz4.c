#include "nso/lz4.h"

#include <string.h>

#define LZ4_MIN_MATCH     4
#define LZ4_MAX_DISTANCE  65535
#define LZ4_HASH_BITS     14
#define LZ4_LAST_LITERALS 5
#define LZ4_MF_LIMIT      12

int64_t lz4_block_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t ip = 0;
    size_t op = 0;

    while (ip < src_len) {
        uint8_t token = src[ip++];

        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= src_len) {
                    return -1;
                }
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > src_len - ip || lit > dst_cap - op) {
            return -1;
        }
        memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;

        if (ip == src_len) {
            break; /* last sequence carries literals only */
        }
        if (src_len - ip < 2) {
            return -1;
        }
        size_t offset = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) {
            return -1;
        }

        size_t mlen = token & 15;
        if (mlen == 15) {
            uint8_t b;
            do {
                if (ip >= src_len) {
                    return -1;
                }
                b = src[ip++];
                mlen += b;
            } while (b == 255);
        }
        mlen += LZ4_MIN_MATCH;
        if (mlen > dst_cap - op) {
            return -1;
        }
        /* Byte copy: the match may overlap its own output. */
        const uint8_t *m = dst + op - offset;
        for (size_t i = 0; i < mlen; i++) {
            dst[op + i] = m[i];
        }
        op += mlen;
    }
    return (int64_t)op;
}

size_t lz4_block_bound(size_t n) {
    return n + n / 255 + 16;
}

static uint32_t lz4_hash(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - LZ4_HASH_BITS);
}

static int64_t emit_length(uint8_t *dst, size_t cap, size_t pos, size_t len) {
    while (len >= 255) {
        if (pos >= cap) {
            return -1;
        }
        dst[pos++] = 255;
        len -= 255;
    }
    if (pos >= cap) {
        return -1;
    }
    dst[pos++] = (uint8_t)len;
    return (int64_t)pos;
}

/* Emit one sequence. `mlen` is the full match length (>= 4) or 0 for the tail. */
static int64_t emit_sequence(uint8_t *dst, size_t cap, size_t pos, const uint8_t *lit,
                             size_t lit_len, size_t offset, size_t mlen) {
    if (pos >= cap) {
        return -1;
    }
    size_t token_pos = pos++;
    uint8_t token = 0;

    if (lit_len >= 15) {
        token |= 0xF0;
        int64_t p = emit_length(dst, cap, pos, lit_len - 15);
        if (p < 0) {
            return -1;
        }
        pos = (size_t)p;
    } else {
        token |= (uint8_t)(lit_len << 4);
    }
    if (lit_len > cap - pos) {
        return -1;
    }
    memcpy(dst + pos, lit, lit_len);
    pos += lit_len;

    if (mlen == 0) {
        dst[token_pos] = token;
        return (int64_t)pos;
    }

    if (cap - pos < 2) {
        return -1;
    }
    dst[pos++] = (uint8_t)(offset & 0xFF);
    dst[pos++] = (uint8_t)(offset >> 8);

    size_t ml = mlen - LZ4_MIN_MATCH;
    if (ml >= 15) {
        token |= 0x0F;
        int64_t p = emit_length(dst, cap, pos, ml - 15);
        if (p < 0) {
            return -1;
        }
        pos = (size_t)p;
    } else {
        token |= (uint8_t)ml;
    }
    dst[token_pos] = token;
    return (int64_t)pos;
}

int64_t lz4_block_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    static uint32_t table[1u << LZ4_HASH_BITS];
    memset(table, 0xFF, sizeof(table));

    size_t pos = 0;
    size_t anchor = 0;
    size_t ip = 0;

    if (src_len >= LZ4_MF_LIMIT) {
        size_t limit = src_len - LZ4_MF_LIMIT;
        while (ip < limit) {
            uint32_t h = lz4_hash(src + ip);
            uint32_t ref = table[h];
            table[h] = (uint32_t)ip;
            if (ref == 0xFFFFFFFFu || ip - ref > LZ4_MAX_DISTANCE ||
                memcmp(src + ref, src + ip, LZ4_MIN_MATCH) != 0) {
                ip++;
                continue;
            }
            size_t mlen = LZ4_MIN_MATCH;
            size_t max = src_len - LZ4_LAST_LITERALS - ip;
            while (mlen < max && src[ref + mlen] == src[ip + mlen]) {
                mlen++;
            }
            int64_t p = emit_sequence(dst, dst_cap, pos, src + anchor, ip - anchor, ip - ref, mlen);
            if (p < 0) {
                return -1;
            }
            pos = (size_t)p;
            ip += mlen;
            anchor = ip;
        }
    }
    int64_t p = emit_sequence(dst, dst_cap, pos, src + anchor, src_len - anchor, 0, 0);
    return p;
}
