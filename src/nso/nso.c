#include "nso/nso.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "common/hos_log.h"
#include "common/sha256.h"
#include "nso/lz4.h"

#define MOD "nso"

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static int seg_in_file(const nso_segment_header *s, uint32_t stored_size, size_t len) {
    return s->file_offset <= len && stored_size <= len - s->file_offset;
}

hos_result nso_parse_header(const uint8_t *buf, size_t len, nso_header *out) {
    if (len < NSO_HEADER_SIZE) {
        HOS_LOGE(MOD, "file too small for an NSO header (%zu bytes)", len);
        return S2PC_ERR_TRUNCATED;
    }
    memcpy(out, buf, sizeof(*out));
    if (memcmp(out->magic, NSO_MAGIC, 4) != 0) {
        HOS_LOGE(MOD, "bad magic %02x%02x%02x%02x", out->magic[0], out->magic[1], out->magic[2],
                 out->magic[3]);
        return S2PC_ERR_BAD_MAGIC;
    }

    const struct {
        const char *name;
        const nso_segment_header *seg;
        uint32_t flag;
        uint32_t compressed_size;
    } segs[3] = {
        {"text", &out->text, NSO_FLAG_TEXT_COMPRESSED, out->text_compressed_size},
        {"rodata", &out->rodata, NSO_FLAG_RODATA_COMPRESSED, out->rodata_compressed_size},
        {"data", &out->data, NSO_FLAG_DATA_COMPRESSED, out->data_compressed_size},
    };
    for (int i = 0; i < 3; i++) {
        uint32_t stored = (out->flags & segs[i].flag) ? segs[i].compressed_size : segs[i].seg->size;
        if (!seg_in_file(segs[i].seg, stored, len)) {
            HOS_LOGE(MOD, "%s segment (file %#x + %#x) exceeds file size %zu", segs[i].name,
                     segs[i].seg->file_offset, stored, len);
            return S2PC_ERR_TRUNCATED;
        }
    }
    if (out->text.memory_offset != 0) {
        HOS_LOGW(MOD, "text does not start at memory offset 0 (%#x)", out->text.memory_offset);
    }
    if ((uint64_t)out->rodata.memory_offset < (uint64_t)out->text.memory_offset + out->text.size ||
        (uint64_t)out->data.memory_offset <
            (uint64_t)out->rodata.memory_offset + out->rodata.size) {
        HOS_LOGE(MOD, "segments overlap or are out of order");
        return S2PC_ERR_CORRUPT;
    }
    return HOS_RESULT_SUCCESS;
}

static hos_result load_segment(const uint8_t *buf, const nso_segment_header *s, int compressed,
                               uint32_t compressed_size, uint8_t *image, const char *name) {
    if (s->size == 0) {
        return HOS_RESULT_SUCCESS;
    }
    if (!compressed) {
        memcpy(image + s->memory_offset, buf + s->file_offset, s->size);
        return HOS_RESULT_SUCCESS;
    }
    int64_t n =
        lz4_block_decode(buf + s->file_offset, compressed_size, image + s->memory_offset, s->size);
    if (n < 0) {
        HOS_LOGE(MOD, "%s: LZ4 block is malformed", name);
        return S2PC_ERR_CORRUPT;
    }
    if ((uint64_t)n != s->size) {
        HOS_LOGE(MOD, "%s: decompressed %" PRId64 " bytes, header says %#x", name, n, s->size);
        return S2PC_ERR_CORRUPT;
    }
    return HOS_RESULT_SUCCESS;
}

static hos_result nso_load_impl(const uint8_t *buf, size_t len, void *hint, int fixed,
                                nso_module *out) {
    memset(out, 0, sizeof(*out));
    hos_result r = nso_parse_header(buf, len, &out->hdr);
    if (HOS_FAILED(r)) {
        return r;
    }
    const nso_header *h = &out->hdr;

    out->text_end = (size_t)h->text.memory_offset + h->text.size;
    out->rodata_end = (size_t)h->rodata.memory_offset + h->rodata.size;
    out->data_end = (size_t)h->data.memory_offset + h->data.size + h->bss_size;
    out->image_size = align_up(out->data_end, NSO_PAGE);
    if (out->image_size == 0) {
        HOS_LOGE(MOD, "empty module");
        return S2PC_ERR_CORRUPT;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS | (fixed ? MAP_FIXED : 0);
    void *img = mmap(hint, out->image_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (img == MAP_FAILED) {
        HOS_LOGE(MOD, "mmap of %zu bytes failed", out->image_size);
        return S2PC_ERR_NO_MEMORY;
    }
    out->image = img;

    r = load_segment(buf, &h->text, h->flags & NSO_FLAG_TEXT_COMPRESSED, h->text_compressed_size,
                     out->image, "text");
    if (HOS_OK(r)) {
        r = load_segment(buf, &h->rodata, h->flags & NSO_FLAG_RODATA_COMPRESSED,
                         h->rodata_compressed_size, out->image, "rodata");
    }
    if (HOS_OK(r)) {
        r = load_segment(buf, &h->data, h->flags & NSO_FLAG_DATA_COMPRESSED,
                         h->data_compressed_size, out->image, "data");
    }
    if (HOS_FAILED(r)) {
        nso_free(out);
        return r;
    }
    if (h->flags & (NSO_FLAG_TEXT_HASH | NSO_FLAG_RODATA_HASH | NSO_FLAG_DATA_HASH)) {
        const struct {
            const char *name;
            const nso_segment_header *seg;
            uint32_t flag;
            const uint8_t *stored;
        } hashes[3] = {
            {"text", &h->text, NSO_FLAG_TEXT_HASH, h->text_hash},
            {"rodata", &h->rodata, NSO_FLAG_RODATA_HASH, h->rodata_hash},
            {"data", &h->data, NSO_FLAG_DATA_HASH, h->data_hash},
        };
        for (int i = 0; i < 3; i++) {
            if (!(h->flags & hashes[i].flag) || hashes[i].seg->size == 0) {
                continue;
            }
            uint8_t computed[SHA256_DIGEST_SIZE];
            sha256(out->image + hashes[i].seg->memory_offset, hashes[i].seg->size, computed);
            if (memcmp(computed, hashes[i].stored, SHA256_DIGEST_SIZE) != 0) {
                HOS_LOGE(MOD, "%s segment SHA-256 mismatch", hashes[i].name);
                nso_free(out);
                return S2PC_ERR_CORRUPT;
            }
            HOS_LOGD(MOD, "%s segment SHA-256 verified", hashes[i].name);
        }
    }
    HOS_LOGD(MOD, "loaded image at %p: text %#zx rodata %#x data %#x bss %#x total %#zx",
             (void *)out->image, out->text_end, h->rodata.size, h->data.size, h->bss_size,
             out->image_size);
    return HOS_RESULT_SUCCESS;
}

void nso_free(nso_module *m) {
    if (m->image) {
        munmap(m->image, m->image_size);
        m->image = NULL;
    }
}

typedef struct seg_out {
    const uint8_t *src;
    size_t size;
    uint8_t *packed; /* heap, only when compressed */
    size_t stored_size;
} seg_out;

static hos_result pack_segment(const uint8_t *src, size_t size, int compress, seg_out *o) {
    o->src = src;
    o->size = size;
    o->packed = NULL;
    o->stored_size = size;
    if (!compress || size == 0) {
        return HOS_RESULT_SUCCESS;
    }
    size_t cap = lz4_block_bound(size);
    o->packed = malloc(cap);
    if (!o->packed) {
        return S2PC_ERR_NO_MEMORY;
    }
    int64_t n = lz4_block_encode(src, size, o->packed, cap);
    if (n < 0) {
        free(o->packed);
        o->packed = NULL;
        return S2PC_ERR_NO_MEMORY;
    }
    o->stored_size = (size_t)n;
    return HOS_RESULT_SUCCESS;
}

hos_result nso_write(const nso_write_params *p, uint8_t **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;

    seg_out segs[3];
    memset(segs, 0, sizeof(segs));
    hos_result r = pack_segment(p->text, p->text_size, p->compress, &segs[0]);
    if (HOS_OK(r)) {
        r = pack_segment(p->rodata, p->rodata_size, p->compress, &segs[1]);
    }
    if (HOS_OK(r)) {
        r = pack_segment(p->data, p->data_size, p->compress, &segs[2]);
    }
    if (HOS_FAILED(r)) {
        goto out;
    }

    size_t name_len = p->module_name ? strlen(p->module_name) + 1 : 0;
    size_t total = NSO_HEADER_SIZE + name_len;
    for (int i = 0; i < 3; i++) {
        total += segs[i].stored_size;
    }
    uint8_t *buf = calloc(1, total);
    if (!buf) {
        r = S2PC_ERR_NO_MEMORY;
        goto out;
    }

    nso_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, NSO_MAGIC, 4);
    h.module_name_offset = NSO_HEADER_SIZE;
    h.module_name_size = (uint32_t)name_len;
    if (name_len) {
        memcpy(buf + NSO_HEADER_SIZE, p->module_name, name_len);
    }

    size_t file_pos = NSO_HEADER_SIZE + name_len;
    size_t mem_pos = 0;
    nso_segment_header *hdrs[3] = {&h.text, &h.rodata, &h.data};
    uint32_t *csizes[3] = {&h.text_compressed_size, &h.rodata_compressed_size,
                           &h.data_compressed_size};
    uint32_t cflags[3] = {NSO_FLAG_TEXT_COMPRESSED, NSO_FLAG_RODATA_COMPRESSED,
                          NSO_FLAG_DATA_COMPRESSED};
    uint64_t explicit_off[3] = {p->text_mem_off, p->rodata_mem_off, p->data_mem_off};
    for (int i = 0; i < 3; i++) {
        if (p->explicit_offsets) {
            mem_pos = explicit_off[i];
        }
        hdrs[i]->file_offset = (uint32_t)file_pos;
        hdrs[i]->memory_offset = (uint32_t)mem_pos;
        hdrs[i]->size = (uint32_t)segs[i].size;
        *csizes[i] = (uint32_t)segs[i].stored_size;
        if (segs[i].packed) {
            h.flags |= cflags[i];
            memcpy(buf + file_pos, segs[i].packed, segs[i].stored_size);
        } else if (segs[i].size) {
            memcpy(buf + file_pos, segs[i].src, segs[i].size);
        }
        file_pos += segs[i].stored_size;
        mem_pos = align_up(mem_pos + segs[i].size, NSO_PAGE);
    }
    h.bss_size = (uint32_t)p->bss_size;
    memcpy(buf, &h, sizeof(h));

    *out_buf = buf;
    *out_len = total;
out:
    for (int i = 0; i < 3; i++) {
        free(segs[i].packed);
    }
    return r;
}

hos_result nso_load(const uint8_t *buf, size_t len, void *hint, nso_module *out) {
    return nso_load_impl(buf, len, hint, 0, out);
}

hos_result nso_load_at(const uint8_t *buf, size_t len, void *addr, nso_module *out) {
    return nso_load_impl(buf, len, addr, 1, out);
}
