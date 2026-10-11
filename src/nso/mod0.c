#include "nso/mod0.h"

#include <elf.h>
#include <inttypes.h>
#include <string.h>

#include "common/hos_log.h"

#define MOD "mod0"

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int32_t rds32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Resolve a signed offset relative to `mod0_off`; returns 0 when out of range. */
static int rel_off(uint64_t mod0_off, int32_t rel, size_t size, uint64_t *out) {
    int64_t v = (int64_t)mod0_off + rel;
    if (v < 0 || (uint64_t)v > size) {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

hos_result mod0_find(const uint8_t *image, size_t size, mod0_info *out) {
    memset(out, 0, sizeof(*out));
    if (size < 8) {
        return HOS_RESULT_SUCCESS;
    }
    uint32_t off = rd32(image + 4);
    if (off == 0 || off > size - 0x1C) {
        return HOS_RESULT_SUCCESS; /* no MOD0 (hand-built modules) */
    }
    if (memcmp(image + off, MOD0_MAGIC, 4) != 0) {
        HOS_LOGD(MOD, "word at +4 (%#x) does not point at a MOD0 header", off);
        return HOS_RESULT_SUCCESS;
    }
    out->present = 1;
    out->mod0_off = off;
    const uint8_t *m = image + off;
    if (!rel_off(off, rds32(m + 0x04), size, &out->dynamic_off) ||
        !rel_off(off, rds32(m + 0x08), size, &out->bss_start_off) ||
        !rel_off(off, rds32(m + 0x0C), size, &out->bss_end_off) ||
        !rel_off(off, rds32(m + 0x10), size, &out->eh_frame_hdr_start_off) ||
        !rel_off(off, rds32(m + 0x14), size, &out->eh_frame_hdr_end_off)) {
        HOS_LOGE(MOD, "MOD0 field points outside the image");
        return S2PC_ERR_CORRUPT;
    }
    HOS_LOGD(MOD, "MOD0 at %#x: dynamic %#" PRIx64 " bss [%#" PRIx64 ", %#" PRIx64 ")", off,
             out->dynamic_off, out->bss_start_off, out->bss_end_off);
    return HOS_RESULT_SUCCESS;
}

/* ---- dynamic section view --------------------------------------------------------- */

typedef struct dyn_view {
    uint64_t rela_off, rela_size;
    uint64_t rel_off, rel_size;
    uint64_t jmprel_off, jmprel_size;
    uint64_t pltrel_type; /* DT_RELA or DT_REL */
    uint64_t symtab_off, strtab_off, strtab_size;
    uint64_t hash_off, gnu_hash_off;
    uint64_t relr_off, relr_size; /* DT_RELR: packed relative relocations */
    uint64_t init_off;            /* DT_INIT, 0 when absent */
    uint64_t init_array_off, init_array_size;
    uint32_t sym_count; /* 0 when unknown */
} dyn_view;

static hos_result read_dynamic(const uint8_t *image, size_t size, uint64_t dyn_off, dyn_view *d) {
    memset(d, 0, sizeof(*d));
    d->pltrel_type = DT_RELA;
    for (uint64_t off = dyn_off; off + sizeof(Elf64_Dyn) <= size; off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn e;
        memcpy(&e, image + off, sizeof(e));
        switch (e.d_tag) {
        case DT_NULL:
            goto done;
        case DT_RELA:
            d->rela_off = e.d_un.d_ptr;
            break;
        case DT_RELASZ:
            d->rela_size = e.d_un.d_val;
            break;
        case DT_REL:
            d->rel_off = e.d_un.d_ptr;
            break;
        case DT_RELSZ:
            d->rel_size = e.d_un.d_val;
            break;
        case DT_JMPREL:
            d->jmprel_off = e.d_un.d_ptr;
            break;
        case DT_PLTRELSZ:
            d->jmprel_size = e.d_un.d_val;
            break;
        case DT_PLTREL:
            d->pltrel_type = e.d_un.d_val;
            break;
        case DT_SYMTAB:
            d->symtab_off = e.d_un.d_ptr;
            break;
        case DT_STRTAB:
            d->strtab_off = e.d_un.d_ptr;
            break;
        case DT_STRSZ:
            d->strtab_size = e.d_un.d_val;
            break;
        case DT_HASH:
            d->hash_off = e.d_un.d_ptr;
            break;
        case DT_GNU_HASH:
            d->gnu_hash_off = e.d_un.d_ptr;
            break;
        case DT_INIT:
            d->init_off = e.d_un.d_ptr;
            break;
        case DT_INIT_ARRAY:
            d->init_array_off = e.d_un.d_ptr;
            break;
        case DT_INIT_ARRAYSZ:
            d->init_array_size = e.d_un.d_val;
            break;
        case DT_RELR:
            d->relr_off = e.d_un.d_ptr;
            break;
        case DT_RELRSZ:
            d->relr_size = e.d_un.d_val;
            break;
        default:
            break;
        }
    }
    HOS_LOGE(MOD, ".dynamic runs past the end of the image");
    return S2PC_ERR_CORRUPT;

done:
    /* Symbol count: DT_HASH says it outright; DT_GNU_HASH needs a walk. */
    if (d->hash_off && d->hash_off + 8 <= size) {
        d->sym_count = rd32(image + d->hash_off + 4); /* nchain */
    } else if (d->gnu_hash_off && d->gnu_hash_off + 16 <= size) {
        uint32_t nbuckets = rd32(image + d->gnu_hash_off);
        uint32_t symoffset = rd32(image + d->gnu_hash_off + 4);
        uint32_t bloom_size = rd32(image + d->gnu_hash_off + 8);
        uint64_t buckets = d->gnu_hash_off + 16 + (uint64_t)bloom_size * 8;
        uint64_t chains = buckets + (uint64_t)nbuckets * 4;
        uint32_t max = 0;
        for (uint32_t b = 0; b < nbuckets && buckets + (uint64_t)b * 4 + 4 <= size; b++) {
            uint32_t idx = rd32(image + buckets + (uint64_t)b * 4);
            if (idx < symoffset) {
                continue;
            }
            while (chains + (uint64_t)(idx - symoffset) * 4 + 4 <= size) {
                uint32_t c = rd32(image + chains + (uint64_t)(idx - symoffset) * 4);
                if (idx + 1 > max) {
                    max = idx + 1;
                }
                if (c & 1) {
                    break;
                }
                idx++;
            }
        }
        d->sym_count = max ? max : symoffset;
    } else if (d->symtab_off && d->strtab_off > d->symtab_off) {
        /* The usual layout puts .dynstr right after .dynsym. */
        d->sym_count = (uint32_t)((d->strtab_off - d->symtab_off) / sizeof(Elf64_Sym));
    }
    return HOS_RESULT_SUCCESS;
}

static const char *sym_name(const uint8_t *image, size_t size, const dyn_view *d,
                            const Elf64_Sym *s) {
    if (!d->strtab_off || s->st_name >= d->strtab_size || d->strtab_off + s->st_name >= size) {
        return "?";
    }
    return (const char *)(image + d->strtab_off + s->st_name);
}

static int read_sym(const uint8_t *image, size_t size, const dyn_view *d, uint32_t index,
                    Elf64_Sym *out) {
    uint64_t off = d->symtab_off + (uint64_t)index * sizeof(Elf64_Sym);
    if (!d->symtab_off || off + sizeof(Elf64_Sym) > size) {
        return 0;
    }
    memcpy(out, image + off, sizeof(*out));
    return 1;
}

/* ---- symbol lookup ------------------------------------------------------------------- */

static int lookup_in(const uint8_t *image, size_t size, uint64_t base, const dyn_view *d,
                     const char *name, uint64_t *value) {
    for (uint32_t i = 1; i < d->sym_count; i++) {
        Elf64_Sym s;
        if (!read_sym(image, size, d, i, &s)) {
            break;
        }
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) {
            continue;
        }
        if (strcmp(sym_name(image, size, d, &s), name) == 0) {
            *value = base + s.st_value;
            return 1;
        }
    }
    return 0;
}

int mod0_lookup(const mod0_module *m, const char *name, uint64_t *value) {
    if (!m->info.present) {
        return 0;
    }
    dyn_view d;
    if (HOS_FAILED(read_dynamic(m->image, m->size, m->info.dynamic_off, &d))) {
        return 0;
    }
    return lookup_in(m->image, m->size, m->base, &d, name, value);
}

/* ---- relocation ----------------------------------------------------------------------- */

typedef struct link_ctx {
    mod0_module *mods;
    size_t count;
    dyn_view *views; /* one per module */
    size_t self;     /* module being relocated */
    mod0_reloc_stats st;
} link_ctx;

/* Resolve symbol `index` of the current module: itself first, then the others. */
static int resolve(link_ctx *c, uint32_t index, uint64_t *value, const char **name) {
    mod0_module *m = &c->mods[c->self];
    dyn_view *d = &c->views[c->self];
    Elf64_Sym s;
    *name = "?";
    if (!read_sym(m->image, m->size, d, index, &s)) {
        return 0;
    }
    *name = sym_name(m->image, m->size, d, &s);
    if (s.st_shndx != SHN_UNDEF) {
        *value = m->base + s.st_value;
        return 1;
    }
    for (size_t j = 0; j < c->count; j++) {
        if (j == c->self || !c->mods[j].info.present) {
            continue;
        }
        if (lookup_in(c->mods[j].image, c->mods[j].size, c->mods[j].base, &c->views[j], *name,
                      value)) {
            HOS_LOGT(MOD, "%s: %s -> %s+%#" PRIx64, m->name ? m->name : "?", *name,
                     c->mods[j].name ? c->mods[j].name : "?", *value - c->mods[j].base);
            return 1;
        }
    }
    return 0;
}

static void apply_one(link_ctx *c, uint64_t r_offset, uint32_t type, uint32_t sym, int64_t addend) {
    mod0_module *m = &c->mods[c->self];
    if (r_offset + 8 > m->size) {
        HOS_LOGW(MOD, "relocation target %#" PRIx64 " outside image, skipped", r_offset);
        c->st.unsupported++;
        return;
    }
    uint64_t value;
    const char *name;
    switch (type) {
    case R_AARCH64_RELATIVE:
        value = m->base + (uint64_t)addend;
        break;
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
        if (!resolve(c, sym, &value, &name)) {
            HOS_LOGD(MOD, "%s: unresolved symbol %s at %#" PRIx64, m->name ? m->name : "?", name,
                     r_offset);
            value = 0;
            c->st.unresolved++;
            memcpy(m->image + r_offset, &value, 8);
            return;
        }
        if (type == R_AARCH64_ABS64) {
            value += (uint64_t)addend;
        }
        break;
    default:
        HOS_LOGW(MOD, "unsupported relocation type %u at %#" PRIx64, type, r_offset);
        c->st.unsupported++;
        return;
    }
    memcpy(m->image + r_offset, &value, 8);
    c->st.applied++;
}

static hos_result walk_rela(link_ctx *c, uint64_t off, uint64_t size) {
    mod0_module *m = &c->mods[c->self];
    if (off + size > m->size) {
        HOS_LOGE(MOD, "RELA table outside image");
        return S2PC_ERR_CORRUPT;
    }
    for (uint64_t p = off; p + sizeof(Elf64_Rela) <= off + size; p += sizeof(Elf64_Rela)) {
        Elf64_Rela r;
        memcpy(&r, m->image + p, sizeof(r));
        apply_one(c, r.r_offset, ELF64_R_TYPE(r.r_info), ELF64_R_SYM(r.r_info), r.r_addend);
    }
    return HOS_RESULT_SUCCESS;
}

static hos_result walk_rel(link_ctx *c, uint64_t off, uint64_t size) {
    mod0_module *m = &c->mods[c->self];
    if (off + size > m->size) {
        HOS_LOGE(MOD, "REL table outside image");
        return S2PC_ERR_CORRUPT;
    }
    for (uint64_t p = off; p + sizeof(Elf64_Rel) <= off + size; p += sizeof(Elf64_Rel)) {
        Elf64_Rel r;
        memcpy(&r, m->image + p, sizeof(r));
        int64_t addend = 0;
        if (r.r_offset + 8 <= m->size) {
            memcpy(&addend, m->image + r.r_offset, 8);
        }
        apply_one(c, r.r_offset, ELF64_R_TYPE(r.r_info), ELF64_R_SYM(r.r_info), addend);
    }
    return HOS_RESULT_SUCCESS;
}

/*
 * DT_RELR: an even entry is the address of a relative relocation; an odd entry
 * is a bitmap of the 63 words following the last address. The addend lives in
 * the target word, so applying RELR twice corrupts it. libnx applies RELR in
 * its own startup code and rtld does it for retail modules, so by default the
 * loader only counts these; a module flagged apply_relr (a library loaded
 * without rtld) gets them applied here.
 */
/* One RELR target: the word holds the link-time address; make it absolute. */
static int relr_add(mod0_module *m, uint64_t where) {
    if (where > m->size || m->size - where < 8 || (where & 7) != 0) {
        HOS_LOGE(MOD, "%s: RELR target %#llx outside image", m->name ? m->name : "module",
                 (unsigned long long)where);
        return -1;
    }
    uint64_t v;
    memcpy(&v, m->image + where, 8);
    v += m->base;
    memcpy(m->image + where, &v, 8);
    return 0;
}

static hos_result walk_relr(link_ctx *c, uint64_t off, uint64_t size) {
    mod0_module *m = &c->mods[c->self];
    if (off + size > m->size) {
        HOS_LOGE(MOD, "RELR table outside image");
        return S2PC_ERR_CORRUPT;
    }
    uint64_t next = 0; /* image offset the next bitmap entry starts at */
    for (uint64_t p = off; p + 8 <= off + size; p += 8) {
        uint64_t entry;
        memcpy(&entry, m->image + p, 8);
        if (!m->apply_relr) {
            c->st.deferred += (entry & 1) ? (size_t)__builtin_popcountll(entry >> 1) : 1;
            continue;
        }
        if ((entry & 1) == 0) {
            if (relr_add(m, entry) != 0) {
                return S2PC_ERR_CORRUPT;
            }
            c->st.relr_applied++;
            next = entry + 8;
        } else {
            for (unsigned bit = 1; bit < 64; bit++) {
                if ((entry >> bit) & 1) {
                    if (relr_add(m, next + (uint64_t)(bit - 1) * 8) != 0) {
                        return S2PC_ERR_CORRUPT;
                    }
                    c->st.relr_applied++;
                }
            }
            next += 63 * 8;
        }
    }
    return HOS_RESULT_SUCCESS;
}

static hos_result relocate_module(link_ctx *c, size_t index) {
    c->self = index;
    const dyn_view *d = &c->views[index];
    hos_result r = HOS_RESULT_SUCCESS;
    if (d->relr_size) {
        r = walk_relr(c, d->relr_off, d->relr_size);
    }
    if (HOS_OK(r) && d->rela_size) {
        r = walk_rela(c, d->rela_off, d->rela_size);
    }
    if (HOS_OK(r) && d->rel_size) {
        r = walk_rel(c, d->rel_off, d->rel_size);
    }
    if (HOS_OK(r) && d->jmprel_size) {
        r = (d->pltrel_type == DT_REL) ? walk_rel(c, d->jmprel_off, d->jmprel_size)
                                       : walk_rela(c, d->jmprel_off, d->jmprel_size);
    }
    return r;
}

hos_result mod0_link(mod0_module *mods, size_t count, mod0_reloc_stats *stats) {
    dyn_view views[16];
    if (count > sizeof(views) / sizeof(views[0])) {
        return S2PC_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < count; i++) {
        memset(&views[i], 0, sizeof(views[i]));
        if (!mods[i].info.present) {
            continue;
        }
        hos_result r =
            read_dynamic(mods[i].image, mods[i].size, mods[i].info.dynamic_off, &views[i]);
        if (HOS_FAILED(r)) {
            return r;
        }
    }
    link_ctx c = {.mods = mods, .count = count, .views = views};
    memset(&c.st, 0, sizeof(c.st));
    for (size_t i = 0; i < count; i++) {
        if (!mods[i].info.present) {
            continue;
        }
        hos_result r = relocate_module(&c, i);
        if (HOS_FAILED(r)) {
            return r;
        }
    }
    HOS_LOGD(MOD,
             "linked %zu modules: %zu relocations, %zu relr applied, %zu unresolved, "
             "%zu unsupported",
             count, c.st.applied, c.st.relr_applied, c.st.unresolved, c.st.unsupported);
    if (stats) {
        *stats = c.st;
    }
    return HOS_RESULT_SUCCESS;
}

hos_result mod0_relocate(uint8_t *image, size_t size, uint64_t base, const mod0_info *m,
                         mod0_reloc_stats *stats) {
    if (!m->present) {
        if (stats) {
            memset(stats, 0, sizeof(*stats));
        }
        return HOS_RESULT_SUCCESS;
    }
    mod0_module one = {.name = NULL, .image = image, .size = size, .base = base, .info = *m};
    return mod0_link(&one, 1, stats);
}

hos_result mod0_init_functions(const mod0_module *m, uint64_t *out, size_t max, size_t *count) {
    *count = 0;
    if (!m->info.present) {
        return HOS_RESULT_SUCCESS;
    }
    dyn_view d;
    hos_result r = read_dynamic(m->image, m->size, m->info.dynamic_off, &d);
    if (HOS_FAILED(r)) {
        return r;
    }
    size_t n = 0;
    if (d.init_off) {
        if (d.init_off >= m->size) {
            HOS_LOGE(MOD, "DT_INIT outside image");
            return S2PC_ERR_CORRUPT;
        }
        out[n++] = m->base + d.init_off;
    }
    if (d.init_array_size) {
        if (d.init_array_off > m->size || d.init_array_size > m->size - d.init_array_off) {
            HOS_LOGE(MOD, "DT_INIT_ARRAY outside image");
            return S2PC_ERR_CORRUPT;
        }
        for (uint64_t p = 0; p + 8 <= d.init_array_size; p += 8) {
            uint64_t fn;
            memcpy(&fn, m->image + d.init_array_off + p, 8);
            if (fn == 0 || fn == UINT64_MAX) {
                continue;
            }
            if (fn < m->base || fn >= m->base + m->size) {
                HOS_LOGE(MOD, "init_array entry %#llx outside the module (not relocated?)",
                         (unsigned long long)fn);
                return S2PC_ERR_CORRUPT;
            }
            if (n == max) {
                return S2PC_ERR_UNSUPPORTED;
            }
            out[n++] = fn;
        }
    }
    *count = n;
    return HOS_RESULT_SUCCESS;
}
