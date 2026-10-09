#ifndef HOS_COMMON_FILE_H
#define HOS_COMMON_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

/**
 * Read a whole file into a heap buffer. Tool-side only, never from a shim.
 *
 * @param path     File path
 * @param out_buf  Receives the buffer; caller frees
 * @param out_len  Receives the length
 * @return HOS_RESULT_SUCCESS, S2PC_ERR_IO or S2PC_ERR_NO_MEMORY
 */
hos_result hos_file_read_all(const char *path, uint8_t **out_buf, size_t *out_len);

/**
 * Write a buffer to a file, replacing any existing content.
 */
hos_result hos_file_write_all(const char *path, const uint8_t *buf, size_t len);

#endif /* HOS_COMMON_FILE_H */
