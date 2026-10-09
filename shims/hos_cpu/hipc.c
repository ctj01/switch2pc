#include "hipc.h"

#include <string.h>

static hos_ipc_buffer decode_abw(const uint32_t *w) {
    hos_ipc_buffer b;
    b.size = (uint64_t)w[0] | ((uint64_t)((w[2] >> 24) & 0xF) << 32);
    b.addr = (uint64_t)w[1] | ((uint64_t)((w[2] >> 28) & 0xF) << 32) |
             ((uint64_t)((w[2] >> 2) & 0x7) << 36);
    return b;
}

static hos_ipc_buffer decode_x(const uint32_t *w) {
    hos_ipc_buffer b;
    b.addr = (uint64_t)w[1] | ((uint64_t)((w[0] >> 12) & 0xF) << 32) |
             ((uint64_t)((w[0] >> 6) & 0x7) << 36);
    b.size = w[0] >> 16;
    return b;
}

static hos_ipc_buffer decode_c(const uint32_t *w) {
    hos_ipc_buffer b;
    b.addr = (uint64_t)w[0] | ((uint64_t)(w[1] & 0xFFFF) << 32);
    b.size = w[1] >> 16;
    return b;
}

hos_result hipc_parse(uint32_t *buf, size_t buf_words, hipc_msg *m) {
    memset(m, 0, sizeof(*m));
    if (buf_words < 2) {
        return HOS_KERNEL_INVALID_COMBINATION;
    }
    uint32_t w0 = buf[0], w1 = buf[1];
    m->type = w0 & 0xFFFF;
    m->num_x = (w0 >> 16) & 0xF;
    m->num_a = (w0 >> 20) & 0xF;
    m->num_b = (w0 >> 24) & 0xF;
    m->num_w = (w0 >> 28) & 0xF;
    m->raw_words = w1 & 0x3FF;
    uint32_t c_flags = (w1 >> 10) & 0xF;
    if (c_flags == 2) {
        m->num_c = 1;
    } else if (c_flags > 2) {
        m->num_c = c_flags - 2;
    }
    int has_handles = (w1 >> 31) & 1;

    if (m->num_x > HOS_IPC_MAX_BUFFERS || m->num_a > HOS_IPC_MAX_BUFFERS ||
        m->num_b > HOS_IPC_MAX_BUFFERS || m->num_w > HOS_IPC_MAX_BUFFERS ||
        m->num_c > HOS_IPC_MAX_BUFFERS) {
        return HOS_KERNEL_INVALID_COMBINATION;
    }

    size_t pos = 2;
    if (has_handles) {
        if (pos >= buf_words) {
            return HOS_KERNEL_INVALID_COMBINATION;
        }
        uint32_t hd = buf[pos++];
        m->has_pid = hd & 1;
        m->num_copy = (hd >> 1) & 0xF;
        m->num_move = (hd >> 5) & 0xF;
        if (m->num_copy > HOS_IPC_MAX_HANDLES || m->num_move > HOS_IPC_MAX_HANDLES) {
            return HOS_KERNEL_INVALID_COMBINATION;
        }
        if (m->has_pid) {
            if (pos + 2 > buf_words) {
                return HOS_KERNEL_INVALID_COMBINATION;
            }
            memcpy(&m->pid, buf + pos, 8);
            pos += 2;
        }
        if (pos + m->num_copy + m->num_move > buf_words) {
            return HOS_KERNEL_INVALID_COMBINATION;
        }
        for (uint32_t i = 0; i < m->num_copy; i++) {
            m->copy[i] = buf[pos++];
        }
        for (uint32_t i = 0; i < m->num_move; i++) {
            m->move[i] = buf[pos++];
        }
    }

    size_t need =
        pos + m->num_x * 2 + (m->num_a + m->num_b + m->num_w) * 3 + m->raw_words + m->num_c * 2;
    if (need > buf_words) {
        return HOS_KERNEL_INVALID_COMBINATION;
    }
    for (uint32_t i = 0; i < m->num_x; i++, pos += 2) {
        m->x[i] = decode_x(buf + pos);
    }
    for (uint32_t i = 0; i < m->num_a; i++, pos += 3) {
        m->a[i] = decode_abw(buf + pos);
    }
    for (uint32_t i = 0; i < m->num_b; i++, pos += 3) {
        m->b[i] = decode_abw(buf + pos);
    }
    for (uint32_t i = 0; i < m->num_w; i++, pos += 3) {
        m->w[i] = decode_abw(buf + pos);
    }
    m->raw = buf + pos;
    size_t raw_end = pos + m->raw_words;

    /* CMIF payload: 16-byte aligned relative to the buffer start. */
    size_t aligned = (pos + 3) & ~(size_t)3;
    if (aligned < raw_end) {
        m->data = buf + aligned;
        size_t pad_before = aligned - pos;
        size_t pad_after = 4 - pad_before;
        m->data_words = (uint32_t)(m->raw_words - pad_before - pad_after);
        if ((int32_t)m->data_words < 0) {
            m->data_words = 0;
        }
    } else {
        m->data = m->raw;
        m->data_words = 0;
    }
    pos = raw_end;
    for (uint32_t i = 0; i < m->num_c; i++, pos += 2) {
        m->c[i] = decode_c(buf + pos);
    }
    return HOS_RESULT_SUCCESS;
}

uint32_t *hipc_write_header(uint32_t *buf, size_t buf_words, uint32_t type, const uint32_t *copy,
                            uint32_t num_copy, const uint32_t *move, uint32_t num_move,
                            uint32_t payload_words, int cmif_align) {
    size_t pos = 2;
    int has_handles = (num_copy + num_move) > 0;
    if (has_handles) {
        pos += 1 + num_copy + num_move;
    }
    size_t payload_pos = pos;
    uint32_t raw_words = payload_words;
    if (cmif_align) {
        payload_pos = (pos + 3) & ~(size_t)3;
        raw_words = payload_words + 4; /* 16 bytes of padding, split around the payload */
    }
    if (payload_pos + payload_words > buf_words || pos + raw_words > buf_words) {
        return NULL;
    }
    memset(buf, 0, (pos + raw_words) * 4);
    buf[0] = type & 0xFFFF;
    buf[1] = (raw_words & 0x3FF) | ((uint32_t)has_handles << 31);
    if (has_handles) {
        size_t p = 2;
        buf[p++] = ((num_copy & 0xF) << 1) | ((num_move & 0xF) << 5);
        for (uint32_t i = 0; i < num_copy; i++) {
            buf[p++] = copy[i];
        }
        for (uint32_t i = 0; i < num_move; i++) {
            buf[p++] = move[i];
        }
    }
    return buf + payload_pos;
}
