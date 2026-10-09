#include "common/hos_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"

hos_result hos_file_read_all(const char *path, uint8_t **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        HOS_LOGE("file", "open %s: %s", path, strerror(errno));
        return S2PC_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return S2PC_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return S2PC_ERR_IO;
    }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return S2PC_ERR_NO_MEMORY;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        HOS_LOGE("file", "read %s: %s", path, strerror(errno));
        free(buf);
        fclose(f);
        return S2PC_ERR_IO;
    }
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return HOS_RESULT_SUCCESS;
}

hos_result hos_file_write_all(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        HOS_LOGE("file", "create %s: %s", path, strerror(errno));
        return S2PC_ERR_IO;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        HOS_LOGE("file", "write %s: %s", path, strerror(errno));
        fclose(f);
        return S2PC_ERR_IO;
    }
    if (fclose(f) != 0) {
        return S2PC_ERR_IO;
    }
    return HOS_RESULT_SUCCESS;
}
