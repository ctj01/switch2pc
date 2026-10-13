#ifndef S2PC_NSO_NRO_H
#define S2PC_NSO_NRO_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/*
 * NRO0: the format homebrew is built and distributed in (what hbmenu launches).
 * Unlike NSO its segments are stored uncompressed at their memory offsets, the
 * text segment starts with the file itself, and an optional asset section
 * after the image ("ASET") carries the icon, the NACP and a RomFS.
 *
 *   0x00  NroStart: branch, MOD0 offset          0x10  "NRO0", version, size, flags
 *   0x20  text, rodata, data {u32 offset, u32 size}   0x38  bss size
 *   0x40  module id (build id, 0x20 bytes)
 *   size  "ASET", version, icon, nacp, romfs {u64 offset, u64 size} (from ASET)
 */

#define NRO_MAGIC  "NRO0"
#define ASET_MAGIC "ASET"

typedef struct nro_info {
    uint32_t text_off, text_size;
    uint32_t rodata_off, rodata_size;
    uint32_t data_off, data_size;
    uint32_t bss_size;
    uint32_t image_size;            /* the NRO header's size: everything but the assets */
    uint64_t romfs_off, romfs_size; /* in the file; size 0 when there is none */
    uint64_t nacp_off, nacp_size;
    char title[0x201]; /* NACP: first non-empty title (American English first), or "" */
    char author[0x101];
} nro_info;

/** True when the buffer starts like an NRO (magic at 0x10). */
int nro_is(const uint8_t *buf, size_t len);

/**
 * Parse an NRO and its assets.
 *
 * @return HOS_RESULT_SUCCESS, S2PC_ERR_TRUNCATED, S2PC_ERR_BAD_MAGIC or S2PC_ERR_CORRUPT
 */
hos_result nro_parse(const uint8_t *buf, size_t len, nro_info *out);

/**
 * The same module as an uncompressed NSO, segments at the NRO's memory offsets,
 * so the NSO loader can map it.
 *
 * @param out_buf  Receives a heap buffer; caller frees
 */
hos_result nro_to_nso(const uint8_t *buf, size_t len, uint8_t **out_buf, size_t *out_len);

#endif /* S2PC_NSO_NRO_H */
