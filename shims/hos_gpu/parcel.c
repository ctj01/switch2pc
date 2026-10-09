#include "parcel.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

int parcel_reader_init(parcel_reader *r, const uint8_t *buf, size_t size) {
    memset(r, 0, sizeof(*r));
    if (size < PARCEL_HEADER_SIZE) {
        r->error = 1;
        return 0;
    }
    uint32_t payload_size = rd32(buf);
    uint32_t payload_off = rd32(buf + 4);
    if (payload_off > size || payload_size > size - payload_off) {
        r->error = 1;
        return 0;
    }
    r->data = buf + payload_off;
    r->size = payload_size;
    return 1;
}

static int take(parcel_reader *r, size_t n, const uint8_t **out) {
    if (r->error || n > r->size - r->pos) {
        r->error = 1;
        return 0;
    }
    *out = r->data + r->pos;
    r->pos += (n + 3) & ~(size_t)3;
    if (r->pos > r->size) {
        r->pos = r->size;
    }
    return 1;
}

int32_t parcel_read_i32(parcel_reader *r) {
    const uint8_t *p;
    if (!take(r, 4, &p)) {
        return 0;
    }
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint32_t parcel_read_u32(parcel_reader *r) {
    return (uint32_t)parcel_read_i32(r);
}

int64_t parcel_read_i64(parcel_reader *r) {
    const uint8_t *p;
    if (!take(r, 8, &p)) {
        return 0;
    }
    int64_t v;
    memcpy(&v, p, 8);
    return v;
}

int parcel_read_data(parcel_reader *r, void *dst, size_t n) {
    const uint8_t *p;
    if (!take(r, n, &p)) {
        return 0;
    }
    memcpy(dst, p, n);
    return 1;
}

int parcel_skip_string16(parcel_reader *r) {
    int32_t len = parcel_read_i32(r);
    if (r->error || len < 0 || len > 1024) {
        r->error = 1;
        return 0;
    }
    const uint8_t *p;
    return take(r, ((size_t)len + 1) * 2, &p);
}

int32_t parcel_read_flattened(parcel_reader *r, void *dst, size_t cap) {
    int32_t size = parcel_read_i32(r);
    int32_t fds = parcel_read_i32(r);
    (void)fds;
    if (r->error || size < 0) {
        r->error = 1;
        return -1;
    }
    const uint8_t *p;
    if (!take(r, (size_t)size, &p)) {
        return -1;
    }
    memcpy(dst, p, (size_t)size < cap ? (size_t)size : cap);
    return size;
}

void parcel_writer_init(parcel_writer *w, uint8_t *buf, size_t cap) {
    memset(w, 0, sizeof(*w));
    w->data = buf;
    w->cap = cap;
    if (cap < PARCEL_HEADER_SIZE) {
        w->error = 1;
    }
}

static uint8_t *reserve(parcel_writer *w, size_t n) {
    size_t padded = (n + 3) & ~(size_t)3;
    if (w->error || PARCEL_HEADER_SIZE + w->pos + padded > w->cap) {
        w->error = 1;
        return NULL;
    }
    uint8_t *p = w->data + PARCEL_HEADER_SIZE + w->pos;
    memset(p, 0, padded);
    w->pos += padded;
    return p;
}

void parcel_write_i32(parcel_writer *w, int32_t v) {
    uint8_t *p = reserve(w, 4);
    if (p) {
        memcpy(p, &v, 4);
    }
}

void parcel_write_u32(parcel_writer *w, uint32_t v) {
    parcel_write_i32(w, (int32_t)v);
}

void parcel_write_data(parcel_writer *w, const void *src, size_t n) {
    uint8_t *p = reserve(w, n);
    if (p) {
        memcpy(p, src, n);
    }
}

void parcel_write_flattened(parcel_writer *w, const void *src, size_t n) {
    parcel_write_i32(w, (int32_t)n);
    parcel_write_i32(w, 0);
    parcel_write_data(w, src, n);
}

size_t parcel_writer_finish(parcel_writer *w) {
    if (w->error) {
        return 0;
    }
    uint32_t hdr[4] = {(uint32_t)w->pos, PARCEL_HEADER_SIZE, 0,
                       (uint32_t)(PARCEL_HEADER_SIZE + w->pos)};
    memcpy(w->data, hdr, sizeof(hdr));
    return PARCEL_HEADER_SIZE + w->pos;
}
