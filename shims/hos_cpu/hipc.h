#ifndef HOS_CPU_HIPC_H
#define HOS_CPU_HIPC_H

/*
 * HIPC: the wire format of a Horizon IPC message in the thread's TLS buffer.
 *
 *   word 0: type:16 | num_x:4 | num_a:4 | num_b:4 | num_w:4
 *   word 1: raw_words:10 | c_flags:4 | reserved:17 | has_handle_descriptor:1
 *   [handle descriptor: send_pid:1 | num_copy:4 | num_move:4 ; pid u64 ; copy[] ; move[]]
 *   [X descriptors, 2 words each] [A, B, W descriptors, 3 words each]
 *   raw data (raw_words words)
 *   [C descriptors, 2 words each]
 *
 * CMIF (types 4..6) puts its header 16-byte aligned inside the raw data and
 * counts 16 bytes of padding in raw_words. TIPC (types >= 16) puts the payload
 * at the start of the raw data with no padding and no header.
 */

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"
#include "hos_ipc.h"

#define HIPC_TYPE_INVALID              0
#define HIPC_TYPE_LEGACY_REQUEST       1
#define HIPC_TYPE_CLOSE                2
#define HIPC_TYPE_LEGACY_CONTROL       3
#define HIPC_TYPE_REQUEST              4
#define HIPC_TYPE_CONTROL              5
#define HIPC_TYPE_REQUEST_WITH_CONTEXT 6
#define HIPC_TYPE_CONTROL_WITH_CONTEXT 7
#define HIPC_TYPE_TIPC_BASE            16

#define CMIF_MAGIC_SFCI 0x49434653u
#define CMIF_MAGIC_SFCO 0x4F434653u

typedef struct hipc_msg {
    uint32_t type;
    uint32_t num_x, num_a, num_b, num_w, num_c;
    hos_ipc_buffer x[HOS_IPC_MAX_BUFFERS];
    hos_ipc_buffer a[HOS_IPC_MAX_BUFFERS];
    hos_ipc_buffer b[HOS_IPC_MAX_BUFFERS];
    hos_ipc_buffer w[HOS_IPC_MAX_BUFFERS];
    hos_ipc_buffer c[HOS_IPC_MAX_BUFFERS];
    int has_pid;
    uint64_t pid;
    uint32_t num_copy, num_move;
    uint32_t copy[HOS_IPC_MAX_HANDLES];
    uint32_t move[HOS_IPC_MAX_HANDLES];
    uint32_t *raw;       /* first raw word */
    uint32_t raw_words;  /* raw_words from the header */
    uint32_t *data;      /* 16-byte aligned CMIF payload start */
    uint32_t data_words; /* words from data to the end of raw (minus trailing pad) */
} hipc_msg;

/**
 * Decode a message in place. Pointers in `m` refer into `buf`.
 *
 * @param buf        Message buffer (the TLS IPC area, 0x100 bytes)
 * @param buf_words  Size of buf in words
 * @param m          Output
 * @return HOS_RESULT_SUCCESS or HOS_KERNEL_INVALID_COMBINATION if malformed
 */
hos_result hipc_parse(uint32_t *buf, size_t buf_words, hipc_msg *m);

/**
 * Write a response header into `buf` and return a zeroed payload area of
 * `payload_words` words, 16-byte aligned when `cmif_align` is set. NULL if it
 * does not fit.
 */
uint32_t *hipc_write_header(uint32_t *buf, size_t buf_words, uint32_t type, const uint32_t *copy,
                            uint32_t num_copy, const uint32_t *move, uint32_t num_move,
                            uint32_t payload_words, int cmif_align);

#endif /* HOS_CPU_HIPC_H */
