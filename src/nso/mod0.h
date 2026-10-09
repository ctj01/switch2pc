#ifndef S2PC_NSO_MOD0_H
#define S2PC_NSO_MOD0_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/*
 * MOD0: the small header the HOS runtime linker (nnrtld) uses to find the ELF
 * .dynamic section inside an NSO image. The word at image offset 4 holds the
 * offset of the MOD0 header from the image base. All MOD0 fields are signed
 * offsets relative to the MOD0 header itself. Dynamic d_ptr values are
 * offsets from the image base.
 */

#define MOD0_MAGIC "MOD0"

typedef struct mod0_info {
    int present;
    uint64_t mod0_off;    /* image offset of the MOD0 header */
    uint64_t dynamic_off; /* image offset of .dynamic */
    uint64_t bss_start_off;
    uint64_t bss_end_off;
    uint64_t eh_frame_hdr_start_off;
    uint64_t eh_frame_hdr_end_off;
} mod0_info;

typedef struct mod0_reloc_stats {
    size_t applied;
    size_t unresolved;  /* symbol relocations whose symbol is undefined; slot left 0 */
    size_t unsupported; /* relocation types we do not handle; slot untouched */
    size_t deferred;    /* DT_RELR entries left to the module's own startup code (not idempotent) */
} mod0_reloc_stats;

/* A mapped module for mod0_link. */
typedef struct mod0_module {
    const char *name;
    uint8_t *image;
    size_t size;
    uint64_t base; /* execution address of image[0] */
    mod0_info info;
} mod0_module;

/**
 * Locate and validate the MOD0 header of a loaded image.
 *
 * @param image  Image bytes
 * @param size   Image size
 * @param out    Receives the header info; out->present is 0 when absent
 * @return HOS_RESULT_SUCCESS (present or absent), S2PC_ERR_CORRUPT if malformed
 */
hos_result mod0_find(const uint8_t *image, size_t size, mod0_info *out);

/**
 * Apply the dynamic relocations of one module in place, resolving symbols
 * against the module itself only. See mod0_link for several modules.
 *
 * @param image  Writable image
 * @param size   Image size
 * @param base   Load address
 * @param m      Result of mod0_find with present != 0
 * @param stats  Receives counts; may be NULL
 * @return HOS_RESULT_SUCCESS or S2PC_ERR_CORRUPT
 */
hos_result mod0_relocate(uint8_t *image, size_t size, uint64_t base, const mod0_info *m,
                         mod0_reloc_stats *stats);

/**
 * Relocate a set of modules together, the way nnrtld does: a symbol undefined
 * in one module is looked up by name in the dynamic symbol tables of the
 * others, in list order. Handles R_AARCH64_RELATIVE, ABS64, GLOB_DAT and
 * JUMP_SLOT. Modules without MOD0 are skipped.
 *
 * @param mods   Modules, already mapped and patched
 * @param count  Number of modules
 * @param stats  Receives totals; may be NULL
 * @return HOS_RESULT_SUCCESS or S2PC_ERR_CORRUPT
 */
hos_result mod0_link(mod0_module *mods, size_t count, mod0_reloc_stats *stats);

/**
 * Look a symbol up by name in a module's dynamic symbol table.
 *
 * @param m      Module with info.present != 0
 * @param name   Symbol name
 * @param value  Receives base + st_value
 * @return 1 if found and defined, 0 otherwise
 */
int mod0_lookup(const mod0_module *m, const char *name, uint64_t *value);

#endif /* S2PC_NSO_MOD0_H */
