/* libFuzzer: the HIPC decoder on a TLS-sized buffer, then the reply header
 * writer with the counts the decoder accepted. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hipc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint32_t buf[0x40];
    memset(buf, 0, sizeof(buf));
    size_t n = size < sizeof(buf) ? size : sizeof(buf);
    memcpy(buf, data, n);
    hipc_msg m;
    /* The full TLS area, then a heap block of exactly the words given, so a read
     * past the declared length is one the sanitizer sees. */
    hipc_parse(buf, sizeof(buf) / 4, &m);
    size_t words = n / 4;
    uint32_t *exact = malloc(words ? words * 4 : 1);
    if (!exact) {
        return 0;
    }
    memcpy(exact, data, words * 4);
    if (hipc_parse(exact, words, &m) == HOS_RESULT_SUCCESS) {
        uint32_t reply[0x40];
        hipc_write_header(reply, sizeof(reply) / 4, m.type, m.copy, m.num_copy, m.move, m.num_move,
                          m.data_words, m.type < HIPC_TYPE_TIPC_BASE);
    }
    free(exact);
    return 0;
}
