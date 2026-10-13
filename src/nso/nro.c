#include "nso/nro.h"

#include <string.h>

#include "nso/nso.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

int nro_is(const uint8_t *buf, size_t len) {
    return len >= 0x80 && memcmp(buf + 0x10, NRO_MAGIC, 4) == 0;
}

/* NACP titles: 16 entries of name [0x200] and author [0x100]; take the first one set. */
static void nacp_title(const uint8_t *nacp, uint64_t size, nro_info *out) {
    for (int k = 0; k < 16 && (uint64_t)(k + 1) * 0x300 <= size; k++) {
        const uint8_t *e = nacp + k * 0x300;
        if (e[0]) {
            memcpy(out->title, e, 0x200);
            memcpy(out->author, e + 0x200, 0x100);
            return;
        }
    }
}

hos_result nro_parse(const uint8_t *buf, size_t len, nro_info *out) {
    memset(out, 0, sizeof(*out));
    if (len < 0x80) {
        return S2PC_ERR_TRUNCATED;
    }
    if (!nro_is(buf, len)) {
        return S2PC_ERR_BAD_MAGIC;
    }
    out->image_size = rd32(buf + 0x18);
    out->text_off = rd32(buf + 0x20);
    out->text_size = rd32(buf + 0x24);
    out->rodata_off = rd32(buf + 0x28);
    out->rodata_size = rd32(buf + 0x2C);
    out->data_off = rd32(buf + 0x30);
    out->data_size = rd32(buf + 0x34);
    out->bss_size = rd32(buf + 0x38);
    if (out->image_size > len) {
        return S2PC_ERR_TRUNCATED;
    }
    /* Segments are in order, page aligned, inside the image; text starts the file. */
    const uint32_t seg[3][2] = {{out->text_off, out->text_size},
                                {out->rodata_off, out->rodata_size},
                                {out->data_off, out->data_size}};
    uint64_t prev_end = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t end = (uint64_t)seg[k][0] + seg[k][1];
        if ((seg[k][0] & (NSO_PAGE - 1)) || seg[k][0] < prev_end || end > out->image_size) {
            return S2PC_ERR_CORRUPT;
        }
        prev_end = end;
    }
    if (out->text_off != 0 || out->text_size == 0) {
        return S2PC_ERR_CORRUPT;
    }
    /* Assets, when present, follow the image. */
    uint64_t a = out->image_size;
    if (len >= a + 0x38 && memcmp(buf + a, ASET_MAGIC, 4) == 0) {
        uint64_t nacp_off = rd64(buf + a + 0x18), nacp_size = rd64(buf + a + 0x20);
        uint64_t romfs_off = rd64(buf + a + 0x28), romfs_size = rd64(buf + a + 0x30);
        if (romfs_size && a + romfs_off + romfs_size <= len) {
            out->romfs_off = a + romfs_off;
            out->romfs_size = romfs_size;
        }
        if (nacp_size && a + nacp_off + nacp_size <= len) {
            out->nacp_off = a + nacp_off;
            out->nacp_size = nacp_size;
            nacp_title(buf + out->nacp_off, nacp_size, out);
        }
    }
    return HOS_RESULT_SUCCESS;
}

hos_result nro_to_nso(const uint8_t *buf, size_t len, uint8_t **out_buf, size_t *out_len) {
    nro_info info;
    hos_result r = nro_parse(buf, len, &info);
    if (HOS_FAILED(r)) {
        return r;
    }
    nso_write_params p;
    memset(&p, 0, sizeof(p));
    p.text = buf + info.text_off;
    p.text_size = info.text_size;
    p.rodata = buf + info.rodata_off;
    p.rodata_size = info.rodata_size;
    p.data = buf + info.data_off;
    p.data_size = info.data_size;
    p.bss_size = info.bss_size;
    p.explicit_offsets = 1;
    p.text_mem_off = info.text_off;
    p.rodata_mem_off = info.rodata_off;
    p.data_mem_off = info.data_off;
    return nso_write(&p, out_buf, out_len);
}
