#ifndef HOS_GPU_MME_H
#define HOS_GPU_MME_H

#include <stdint.h>

/* Macro Method Expander interpreter; see gpu_mme.c for the instruction set. */

typedef void (*mme_send_fn)(void *user, uint32_t method, uint32_t value);
typedef uint32_t (*mme_read_fn)(void *user, uint32_t method);

/**
 * Run one macro to completion.
 *
 * @param code        Macro code RAM
 * @param code_words  Size of code RAM
 * @param start       Entry point (from the start-address RAM)
 * @param params      Call parameters; the first lands in r1
 * @param send_fn     Called for every method the macro emits
 * @param read_fn     Reads an engine register (the shadow register file)
 * @return 0, or -1 if the macro is malformed or does not terminate
 */
int mme_execute(const uint32_t *code, uint32_t code_words, uint32_t start, const uint32_t *params,
                uint32_t num_params, mme_send_fn send_fn, mme_read_fn read_fn, void *user);

#endif /* HOS_GPU_MME_H */
