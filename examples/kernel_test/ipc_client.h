#ifndef EXAMPLE_IPC_CLIENT_H
#define EXAMPLE_IPC_CLIENT_H

/*
 * A minimal HIPC/CMIF client, the way libnx builds requests: fill the TLS
 * buffer, svcSendSyncRequest, parse the reply. Freestanding, header only, so
 * the same code serves the test module and the host-side tests.
 *
 * Buffers: one A (send), one B (receive), one X (pointer send, used for
 * paths), one C (pointer receive, used for fixed-size out structs).
 */

#include <stddef.h>
#include <stdint.h>

#define IPC_TYPE_CLOSE     2
#define IPC_TYPE_REQUEST   4
#define IPC_TYPE_CONTROL   5
#define IPC_TYPE_TIPC(cmd) (16 + (cmd))

#define IPC_SFCI 0x49434653u
#define IPC_SFCO 0x4F434653u

typedef struct ipc_request {
    uint32_t type;
    uint32_t cmd;
    int send_pid;
    const void *data;
    uint32_t data_size;
    const void *a_buf;
    uint64_t a_size;
    void *b_buf;
    uint64_t b_size;
    const void *x_buf;
    uint64_t x_size;
    void *c_buf;
    uint64_t c_size;
    uint32_t copy_handles[2];
    uint32_t num_copy;
    int domain;         /* session is a domain: add the domain header */
    uint32_t object_id; /* domain target */
    uint8_t domain_cmd; /* 1 = request, 2 = close object */
} ipc_request;

typedef struct ipc_response {
    uint32_t result;
    const uint32_t *data;
    uint32_t data_words;
    uint32_t move_handles[8];
    uint32_t num_move;
    uint32_t copy_handles[8];
    uint32_t num_copy;
    uint32_t object_ids[8];
    uint32_t num_objects;
} ipc_response;

static inline void ipc_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) {
        *dd++ = *ss++;
    }
}

static inline void ipc_memset(void *d, int c, size_t n) {
    uint8_t *dd = d;
    while (n--) {
        *dd++ = (uint8_t)c;
    }
}

static inline uint32_t ipc_write_abw(uint32_t *w, const void *buf, uint64_t size) {
    uint64_t addr = (uint64_t)buf;
    w[0] = (uint32_t)size;
    w[1] = (uint32_t)addr;
    w[2] = (uint32_t)(((addr >> 36) & 7) << 2) | (uint32_t)(((size >> 32) & 0xF) << 24) |
           (uint32_t)(((addr >> 32) & 0xF) << 28);
    return 3;
}

/* Build the request into `tls` (0x100 bytes). */
static inline void ipc_write_request(uint32_t *tls, const ipc_request *r) {
    ipc_memset(tls, 0, 0x100);
    int tipc = r->type >= 16;
    uint32_t num_a = r->a_buf ? 1 : 0;
    uint32_t num_b = r->b_buf ? 1 : 0;
    uint32_t num_x = r->x_buf ? 1 : 0;
    uint32_t num_c = r->c_buf ? 1 : 0;
    int has_hd = r->send_pid || r->num_copy;
    uint32_t pos = 2;

    if (has_hd) {
        tls[pos++] = (r->send_pid ? 1u : 0u) | (r->num_copy << 1);
        if (r->send_pid) {
            pos += 2; /* pid, filled by the kernel */
        }
        for (uint32_t i = 0; i < r->num_copy; i++) {
            tls[pos++] = r->copy_handles[i];
        }
    }
    if (num_x) {
        uint64_t addr = (uint64_t)r->x_buf;
        tls[pos++] = (uint32_t)(((addr >> 36) & 7) << 6) | (uint32_t)(((addr >> 32) & 0xF) << 12) |
                     (uint32_t)(r->x_size << 16);
        tls[pos++] = (uint32_t)addr;
    }
    if (num_a) {
        pos += ipc_write_abw(tls + pos, r->a_buf, r->a_size);
    }
    if (num_b) {
        pos += ipc_write_abw(tls + pos, r->b_buf, r->b_size);
    }

    uint32_t raw_start = pos;
    uint32_t raw_words;
    if (tipc) {
        ipc_memcpy(tls + pos, r->data, r->data_size);
        raw_words = (r->data_size + 3) / 4;
    } else {
        uint32_t aligned = (pos + 3) & ~3u;
        uint32_t *p = tls + aligned;
        uint32_t payload_size = 16 + r->data_size;
        if (r->domain) {
            uint8_t *dh = (uint8_t *)p;
            dh[0] = r->domain_cmd;
            dh[1] = 0;
            uint16_t ps = (uint16_t)payload_size;
            ipc_memcpy(dh + 2, &ps, 2);
            ipc_memcpy(dh + 4, &r->object_id, 4);
            p += 4;
        }
        p[0] = IPC_SFCI;
        p[1] = 0;
        p[2] = r->cmd;
        p[3] = 0;
        ipc_memcpy(p + 4, r->data, r->data_size);
        uint32_t words = (aligned - raw_start) + (r->domain ? 4 : 0) + 4 + (r->data_size + 3) / 4;
        raw_words = words + (4 - (aligned - raw_start));
    }
    pos = raw_start + raw_words;
    if (num_c) {
        uint64_t addr = (uint64_t)r->c_buf;
        tls[pos++] = (uint32_t)addr;
        tls[pos++] = (uint32_t)((addr >> 32) & 0xFFFF) | (uint32_t)(r->c_size << 16);
    }
    tls[0] = (r->type & 0xFFFF) | (num_x << 16) | (num_a << 20) | (num_b << 24);
    tls[1] = (raw_words & 0x3FF) | (num_c ? (3u << 10) : 0) | (has_hd ? 0x80000000u : 0);
}

/* Parse a reply from `tls`. Returns 0 when well-formed. */
static inline int ipc_parse_response(const uint32_t *tls, ipc_response *out, int domain, int tipc) {
    ipc_memset(out, 0, sizeof(*out));
    uint32_t raw_words = tls[1] & 0x3FF;
    int has_hd = (tls[1] >> 31) & 1;
    uint32_t pos = 2;
    if (has_hd) {
        uint32_t hd = tls[pos++];
        if (hd & 1) {
            pos += 2;
        }
        out->num_copy = (hd >> 1) & 0xF;
        out->num_move = (hd >> 5) & 0xF;
        for (uint32_t i = 0; i < out->num_copy && i < 8; i++) {
            out->copy_handles[i] = tls[pos++];
        }
        for (uint32_t i = 0; i < out->num_move && i < 8; i++) {
            out->move_handles[i] = tls[pos++];
        }
    }
    if (tipc) {
        out->result = tls[pos];
        out->data = tls + pos + 1;
        out->data_words = raw_words ? raw_words - 1 : 0;
        return 0;
    }
    uint32_t aligned = (pos + 3) & ~3u;
    const uint32_t *p = tls + aligned;
    uint32_t words = raw_words - (aligned - pos) - (4 - (aligned - pos));
    if (domain) {
        out->num_objects = p[0];
        p += 4;
        words -= 4;
    }
    if (p[0] != IPC_SFCO) {
        return -1;
    }
    out->result = p[2];
    out->data = p + 4;
    out->data_words = words - 4 - out->num_objects;
    for (uint32_t i = 0; i < out->num_objects && i < 8; i++) {
        out->object_ids[i] = p[4 + out->data_words + i];
    }
    return 0;
}

#endif /* EXAMPLE_IPC_CLIENT_H */
