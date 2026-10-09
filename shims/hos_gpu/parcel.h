#ifndef HOS_GPU_PARCEL_H
#define HOS_GPU_PARCEL_H

/*
 * Android Parcel, as carried through vi's IHOSBinderDriver::TransactParcel.
 *
 *   header: u32 payload_size, u32 payload_off (0x10), u32 objects_size, u32 objects_off
 *   payload: 4-byte aligned words; strings are int32 length + UTF-16 + nul,
 *   flattened objects are int32 size, int32 fd count, bytes.
 */

#include <stddef.h>
#include <stdint.h>

#define PARCEL_HEADER_SIZE 0x10

typedef struct parcel_reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int error;
} parcel_reader;

typedef struct parcel_writer {
    uint8_t *data;
    size_t cap;
    size_t pos; /* bytes of payload written */
    int error;
} parcel_writer;

/**
 * Start reading a parcel from a buffer. Returns 0 if the header is bad.
 */
int parcel_reader_init(parcel_reader *r, const uint8_t *buf, size_t size);

int32_t parcel_read_i32(parcel_reader *r);
uint32_t parcel_read_u32(parcel_reader *r);
int64_t parcel_read_i64(parcel_reader *r);

/**
 * Copy `n` payload bytes out (4-byte aligned advance). Returns 0 if short.
 */
int parcel_read_data(parcel_reader *r, void *dst, size_t n);

/**
 * Skip a String16 (interface token). Returns 0 if malformed.
 */
int parcel_skip_string16(parcel_reader *r);

/**
 * Read a flattened object: int32 size, int32 fd count, bytes. Copies at most
 * `cap` bytes into dst; returns the declared size or -1.
 */
int32_t parcel_read_flattened(parcel_reader *r, void *dst, size_t cap);

/**
 * Start writing a reply parcel into `buf`; payload begins after the header.
 */
void parcel_writer_init(parcel_writer *w, uint8_t *buf, size_t cap);

void parcel_write_i32(parcel_writer *w, int32_t v);
void parcel_write_u32(parcel_writer *w, uint32_t v);
void parcel_write_data(parcel_writer *w, const void *src, size_t n);
void parcel_write_flattened(parcel_writer *w, const void *src, size_t n);

/**
 * Fill in the header; returns the total size (header + payload).
 */
size_t parcel_writer_finish(parcel_writer *w);

#endif /* HOS_GPU_PARCEL_H */
