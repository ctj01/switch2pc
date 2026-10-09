#include "nso/elf2nso.h"

#include <elf.h>
#include <inttypes.h>
#include <string.h>

#include "common/hos_log.h"
#include "nso/nso.h"

#define MOD "elf2nso"

typedef struct load_seg {
    uint64_t vaddr, offset, filesz, memsz;
    uint32_t flags;
} load_seg;

hos_result elf2nso_convert(const uint8_t *elf, size_t len, const char *module_name, int compress,
                           uint8_t **out_buf, size_t *out_len) {
    Elf64_Ehdr eh;
    if (len < sizeof(eh)) {
        return S2PC_ERR_TRUNCATED;
    }
    memcpy(&eh, elf, sizeof(eh));
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
        HOS_LOGE(MOD, "not a 64-bit ELF");
        return S2PC_ERR_BAD_MAGIC;
    }
    if (eh.e_machine != EM_AARCH64) {
        HOS_LOGE(MOD, "machine %u is not AArch64", eh.e_machine);
        return S2PC_ERR_UNSUPPORTED;
    }
    if (eh.e_phoff + (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr) > len) {
        return S2PC_ERR_TRUNCATED;
    }

    load_seg segs[3];
    int n = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        memcpy(&ph, elf + eh.e_phoff + (size_t)i * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
            continue;
        }
        if (n == 3) {
            HOS_LOGE(MOD, "more than three PT_LOAD segments");
            return S2PC_ERR_UNSUPPORTED;
        }
        if (ph.p_offset + ph.p_filesz > len) {
            return S2PC_ERR_TRUNCATED;
        }
        segs[n].vaddr = ph.p_vaddr;
        segs[n].offset = ph.p_offset;
        segs[n].filesz = ph.p_filesz;
        segs[n].memsz = ph.p_memsz;
        segs[n].flags = ph.p_flags;
        n++;
    }
    if (n == 0) {
        HOS_LOGE(MOD, "no PT_LOAD segments");
        return S2PC_ERR_CORRUPT;
    }
    /* sort by address */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && segs[j - 1].vaddr > segs[j].vaddr; j--) {
            load_seg t = segs[j];
            segs[j] = segs[j - 1];
            segs[j - 1] = t;
        }
    }
    uint64_t base = segs[0].vaddr;
    if (base != 0) {
        HOS_LOGW(MOD, "first segment at %#" PRIx64 ", rebasing to 0", base);
    }
    if (!(segs[0].flags & PF_X)) {
        HOS_LOGE(MOD, "first segment is not executable");
        return S2PC_ERR_UNSUPPORTED;
    }

    nso_write_params p;
    memset(&p, 0, sizeof(p));
    p.module_name = module_name;
    p.compress = compress;
    p.explicit_offsets = 1;

    for (int i = 0; i < n; i++) {
        const load_seg *s = &segs[i];
        if ((s->vaddr - base) & 0xFFF) {
            HOS_LOGE(MOD, "segment %d at %#" PRIx64 " is not page aligned", i, s->vaddr);
            return S2PC_ERR_UNSUPPORTED;
        }
        uint64_t off = s->vaddr - base;
        const uint8_t *bytes = elf + s->offset;
        if (i == 0) {
            p.text = bytes;
            p.text_size = s->filesz;
            p.text_mem_off = off;
            if (s->memsz > s->filesz) {
                HOS_LOGE(MOD, "text segment has bss");
                return S2PC_ERR_UNSUPPORTED;
            }
        } else if (i == n - 1 && (s->flags & PF_W)) {
            p.data = bytes;
            p.data_size = s->filesz;
            p.data_mem_off = off;
            p.bss_size = s->memsz - s->filesz;
        } else {
            if (p.rodata) {
                HOS_LOGE(MOD, "two read-only data segments");
                return S2PC_ERR_UNSUPPORTED;
            }
            p.rodata = bytes;
            p.rodata_size = s->filesz;
            p.rodata_mem_off = off;
            if (s->memsz > s->filesz) {
                HOS_LOGE(MOD, "rodata segment has bss");
                return S2PC_ERR_UNSUPPORTED;
            }
        }
    }
    if (!p.rodata) {
        p.rodata_mem_off = p.text_mem_off + ((p.text_size + 0xFFF) & ~0xFFFull);
    }
    if (!p.data) {
        p.data_mem_off = p.rodata_mem_off + ((p.rodata_size + 0xFFF) & ~0xFFFull);
    }
    HOS_LOGD(MOD, "text %#" PRIx64 "+%#zx rodata %#" PRIx64 "+%#zx data %#" PRIx64 "+%#zx bss %#zx",
             p.text_mem_off, p.text_size, p.rodata_mem_off, p.rodata_size, p.data_mem_off,
             p.data_size, p.bss_size);
    return nso_write(&p, out_buf, out_len);
}
