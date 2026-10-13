/* libFuzzer: the whole NSO path, header check, LZ4, hashes, mapping. */

#include <stddef.h>
#include <stdint.h>

#include "common/hos_log.h"
#include "nso/nso.h"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    hos_log_set_level(HOS_LOG_ERROR + 1); /* every rejected input would log otherwise */
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    nso_header h;
    nso_parse_header(data, size, &h);
    nso_module m;
    if (nso_load(data, size, NULL, &m) == HOS_RESULT_SUCCESS) {
        nso_free(&m);
    }
    return 0;
}
