#ifndef HOS_FS_ROMFS_BUILD_H
#define HOS_FS_ROMFS_BUILD_H

/*
 * Build a romfs image from a host directory, lazily. The header and the
 * directory/file tables are materialised in memory; file contents are read
 * from the host files when the game reads that part of the storage, so a
 * multi-gigabyte romfs costs nothing up front.
 *
 * Image layout (the format the HOS runtime and libnx parse):
 *   0x00  header (0x50 bytes: sizes and offsets of the four tables and the data)
 *   dir hash table, dir table, file hash table, file table, then file data
 */

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

typedef struct romfs_image romfs_image;

/**
 * Walk `dir` and build the romfs tables. Returns S2PC_ERR_IO if the directory
 * cannot be read, S2PC_ERR_NO_MEMORY if it is absurdly large.
 */
hos_result romfs_build_from_dir(const char *dir, romfs_image **out);

/**
 * Total size of the image in bytes (header, tables and data).
 */
uint64_t romfs_image_size(const romfs_image *img);

/**
 * Copy `size` bytes at `offset` of the image into dst. Padding reads as zero;
 * reads past the end are zero-filled.
 */
hos_result romfs_image_read(const romfs_image *img, uint64_t offset, void *dst, uint64_t size);

void romfs_image_free(romfs_image *img);

#endif /* HOS_FS_ROMFS_BUILD_H */
