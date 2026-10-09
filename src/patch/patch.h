#ifndef S2PC_PATCH_PATCH_H
#define S2PC_PATCH_PATCH_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/*
 * Instruction patching. A Switch binary talks to its kernel with `svc #n`,
 * reads its thread pointer from tpidrro_el0 and times itself with a 19.2 MHz
 * counter. None of that holds under Linux: the kernel treats `svc` as a Linux
 * syscall, leaves tpidrro_el0 at zero, and the generic counter runs at
 * whatever frequency the host has (games hardcode 19.2 MHz). We rewrite four
 * instruction shapes, one word each, so code size and every relative branch
 * stay unchanged:
 *
 *   svc #n                     -> brk #n            trapped, dispatched to libhos_cpu
 *   mrs Xt, tpidrro_el0        -> mrs Xt, tpidr_el0 loader keeps the HOS TLS block there
 *   mrs Xt, cntpct/cntvct_el0  -> brk #(0x100|t)    trapped, Xt = ticks at 19.2 MHz
 *   mrs Xt, cntfrq_el0         -> brk #(0x120|t)    trapped, Xt = 19200000
 */

typedef enum patch_kind {
    PATCH_SVC_TO_BRK = 0,
    PATCH_TPIDRRO_TO_TPIDR,
    PATCH_COUNTER_TO_BRK,
    PATCH_CNTFRQ_TO_BRK,
    PATCH_KIND_COUNT
} patch_kind;

typedef struct patch_site {
    uint64_t offset; /* byte offset into the scanned buffer */
    uint32_t before;
    uint32_t after;
    patch_kind kind;
    uint16_t imm; /* svc number for PATCH_SVC_TO_BRK, else 0 */
} patch_site;

typedef struct patch_stats {
    size_t by_kind[PATCH_KIND_COUNT];
    size_t total;
} patch_stats;

typedef void (*patch_visit_fn)(const patch_site *site, void *user);

/* Encodings. imm16 lives in bits [20:5] for svc and brk; Rt in bits [4:0] for mrs. */
#define A64_SVC_MASK    0xFFE0001Fu
#define A64_SVC_BITS    0xD4000001u
#define A64_BRK_BITS    0xD4200000u
#define A64_MRS_MASK    0xFFFFFFE0u
#define A64_MRS_TPIDRRO 0xD53BD060u
#define A64_MRS_TPIDR   0xD53BD040u
#define A64_MRS_CNTFRQ  0xD53BE000u
#define A64_MRS_CNTPCT  0xD53BE020u
#define A64_MRS_CNTVCT  0xD53BE040u

/* HOS svc numbers are 0x01..0x7F. Anything else is not ours. */
#define HOS_SVC_MAX 0x7F

/* brk immediates used for the counter traps: base | register number. */
#define A64_BRK_TICK_BASE   0x100u
#define A64_BRK_CNTFRQ_BASE 0x120u
#define HOS_TICK_FREQ       19200000u

/**
 * Human-readable name of a patch kind.
 */
const char *patch_kind_name(patch_kind kind);

/**
 * Classify one instruction word.
 *
 * @param insn  Instruction word
 * @param site  Receives before/after/kind/imm when the word needs patching
 * @return 1 if the word needs patching, 0 otherwise
 */
int patch_classify(uint32_t insn, patch_site *site);

/**
 * Scan a text buffer word by word and optionally rewrite it in place.
 *
 * @param text   Buffer (4-byte aligned contents; trailing bytes ignored)
 * @param size   Buffer size
 * @param apply  Nonzero to rewrite, zero for a dry run
 * @param stats  Receives counts; may be NULL
 * @param fn     Called for every site, before rewriting; may be NULL
 * @param user   Passed to fn
 * @return HOS_RESULT_SUCCESS
 */
hos_result patch_text(uint8_t *text, size_t size, int apply, patch_stats *stats, patch_visit_fn fn,
                      void *user);

#endif /* S2PC_PATCH_PATCH_H */
