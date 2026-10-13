/* libFuzzer: the HIPC decoder on a TLS-sized buffer, then the reply header
 * writer with the counts the decoder accepted. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hipc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint32_t buf[0x40];
    memset(buf, 0, sizeof(buf));
    size_t n = size < sizeof(buf) ? size : sizeof(buf);
    memcpy(buf, data, n);
    hipc_msg m;
    /* The full TLS area, and a short buffer of exactly the bytes given. */
    hipc_parse(buf, sizeof(buf) / 4, &m);
    if (hipc_parse(buf, n / 4, &m) == HOS_RESULT_SUCCESS) {
        uint32_t reply[0x40];
        hipc_write_header(reply, sizeof(reply) / 4, m.type, m.copy, m.num_copy, m.move, m.num_move,
                          m.data_words, m.type < HIPC_TYPE_TIPC_BASE);
    }
    return 0;
}
