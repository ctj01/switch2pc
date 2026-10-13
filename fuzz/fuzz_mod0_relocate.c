/* libFuzzer: MOD0 discovery, the dynamic section walk, relocation, symbol
 * lookup and the initialiser list, on a writable copy of the input. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "nso/mod0.h"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    hos_log_set_level(HOS_LOG_ERROR + 1);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *image = malloc(size ? size : 1);
    if (!image) {
        return 0;
    }
    memcpy(image, data, size);
    mod0_info info;
    if (mod0_find(image, size, &info) == HOS_RESULT_SUCCESS && info.present) {
        mod0_reloc_stats st;
        mod0_relocate(image, size, 0x8000000, &info, &st);
        mod0_module m = {
            .name = "fuzz", .image = image, .size = size, .base = 0x8000000, .info = info};
        uint64_t value;
        mod0_lookup(&m, "foo", &value);
        uint64_t inits[MOD0_MAX_INIT];
        size_t count;
        mod0_init_functions(&m, inits, MOD0_MAX_INIT, &count);
        m.apply_relr = 1;
        mod0_link(&m, 1, &st);
    }
    free(image);
    return 0;
}
