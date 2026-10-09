#ifndef S2PC_NSO_NSO_H
#define S2PC_NSO_NSO_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/*
 * NSO0: the executable format HOS loads. Three segments (text, rodata, data),
 * each optionally LZ4-compressed, plus bss. No ELF headers survive; MOD0 (see
 * mod0.h) is the only link-time metadata left in the image.
 */

#define NSO_MAGIC       "NSO0"
#define NSO_HEADER_SIZE 0x100

#define NSO_FLAG_TEXT_COMPRESSED   (1u << 0)
#define NSO_FLAG_RODATA_COMPRESSED (1u << 1)
#define NSO_FLAG_DATA_COMPRESSED   (1u << 2)
#define NSO_FLAG_TEXT_HASH         (1u << 3)
#define NSO_FLAG_RODATA_HASH       (1u << 4)
#define NSO_FLAG_DATA_HASH         (1u << 5)

#define NSO_PAGE 0x1000u

typedef struct nso_segment_header {
    uint32_t file_offset;
    uint32_t memory_offset;
    uint32_t size; /* decompressed size */
} nso_segment_header;

typedef struct nso_sub_header {
    uint32_t offset; /* relative to rodata memory offset */
    uint32_t size;
} nso_sub_header;

typedef struct nso_header {
    uint8_t magic[4];
    uint32_t version;
    uint32_t reserved0;
    uint32_t flags;
    nso_segment_header text;
    uint32_t module_name_offset;
    nso_segment_header rodata;
    uint32_t module_name_size;
    nso_segment_header data;
    uint32_t bss_size;
    uint8_t module_id[0x20];
    uint32_t text_compressed_size;
    uint32_t rodata_compressed_size;
    uint32_t data_compressed_size;
    uint8_t reserved1[0x1C];
    nso_sub_header api_info;
    nso_sub_header dynstr;
    nso_sub_header dynsym;
    uint8_t text_hash[0x20];
    uint8_t rodata_hash[0x20];
    uint8_t data_hash[0x20];
} nso_header;

_Static_assert(sizeof(nso_header) == NSO_HEADER_SIZE, "nso_header must be 0x100 bytes");

/* A decompressed module image laid out exactly as HOS would map it. */
typedef struct nso_module {
    nso_header hdr;
    uint8_t *image;    /* page-aligned anonymous mapping; text at hdr.text.memory_offset */
    size_t image_size; /* through end of bss, page-rounded */
    size_t text_end;   /* memory offset one past text */
    size_t rodata_end;
    size_t data_end; /* includes bss */
} nso_module;

/**
 * Parse and sanity-check an NSO header.
 *
 * @param buf  File bytes
 * @param len  File length
 * @param out  Receives the header
 * @return HOS_RESULT_SUCCESS, S2PC_ERR_TRUNCATED, S2PC_ERR_BAD_MAGIC or S2PC_ERR_CORRUPT
 */
hos_result nso_parse_header(const uint8_t *buf, size_t len, nso_header *out);

/**
 * Decompress a whole NSO into a fresh anonymous mapping (read+write).
 * Hash verification is not performed yet; see FIXME(#1).
 *
 * @param buf   File bytes
 * @param len   File length
 * @param hint  Preferred base address or NULL
 * @param out   Receives the module; release with nso_free
 * @return HOS_RESULT_SUCCESS or an S2PC_ERR_* code
 */
hos_result nso_load(const uint8_t *buf, size_t len, void *hint, nso_module *out);

/**
 * Like nso_load, but map the image exactly at `addr` (MAP_FIXED over a range the
 * caller has reserved). Used to lay several modules out back to back.
 */
hos_result nso_load_at(const uint8_t *buf, size_t len, void *addr, nso_module *out);

/**
 * Unmap a module loaded with nso_load.
 */
void nso_free(nso_module *m);

/* Inputs for nso_write. Any segment may be empty. */
typedef struct nso_write_params {
    const uint8_t *text;
    size_t text_size;
    const uint8_t *rodata;
    size_t rodata_size;
    const uint8_t *data;
    size_t data_size;
    size_t bss_size;
    const char *module_name; /* may be NULL */
    int compress;            /* nonzero: LZ4 every non-empty segment */
    int explicit_offsets;    /* nonzero: use the *_mem_off fields instead of packing */
    uint64_t text_mem_off;
    uint64_t rodata_mem_off;
    uint64_t data_mem_off;
} nso_write_params;

/**
 * Build an NSO file image. Segments are placed at page-aligned memory offsets in
 * text, rodata, data order. Hash flags are left clear.
 *
 * @param p        Segment contents
 * @param out_buf  Receives a heap buffer; caller frees
 * @param out_len  Receives the length
 * @return HOS_RESULT_SUCCESS or S2PC_ERR_NO_MEMORY
 */
hos_result nso_write(const nso_write_params *p, uint8_t **out_buf, size_t *out_len);

#endif /* S2PC_NSO_NSO_H */
