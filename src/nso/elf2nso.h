#ifndef S2PC_NSO_ELF2NSO_H
#define S2PC_NSO_ELF2NSO_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/**
 * Convert a static-PIE ARM64 ELF with up to three PT_LOAD segments (text,
 * rodata, data+bss, in address order) into an NSO image. Segment memory
 * offsets are taken from the ELF so MOD0 and the dynamic section stay valid.
 *
 * @param elf          ELF bytes
 * @param len          Length
 * @param module_name  Name stored in the NSO, may be NULL
 * @param compress     Nonzero to LZ4 the segments
 * @param out_buf      Receives a heap buffer; caller frees
 * @param out_len      Receives the length
 * @return HOS_RESULT_SUCCESS, S2PC_ERR_BAD_MAGIC, S2PC_ERR_UNSUPPORTED or S2PC_ERR_CORRUPT
 */
hos_result elf2nso_convert(const uint8_t *elf, size_t len, const char *module_name, int compress,
                           uint8_t **out_buf, size_t *out_len);

#endif /* S2PC_NSO_ELF2NSO_H */
