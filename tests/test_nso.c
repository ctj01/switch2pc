#include <elf.h>
#include <stdlib.h>
#include <string.h>

#include "nso/mod0.h"
#include "nso/nro.h"
#include "nso/nso.h"
#include "test.h"

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + (i % 7) * 3 + (i / 64));
    }
}

static hos_result build(int compress, uint8_t **buf, size_t *len) {
    static uint8_t text[300], rodata[50], data[20];
    fill(text, sizeof(text), 1);
    fill(rodata, sizeof(rodata), 2);
    fill(data, sizeof(data), 3);
    nso_write_params p = {
        .text = text,
        .text_size = sizeof(text),
        .rodata = rodata,
        .rodata_size = sizeof(rodata),
        .data = data,
        .data_size = sizeof(data),
        .bss_size = 0x100,
        .module_name = "fixture",
        .compress = compress,
    };
    return nso_write(&p, buf, len);
}

static void check_layout(const nso_module *m) {
    ASSERT_EQ(m->hdr.text.memory_offset, 0);
    ASSERT_EQ(m->hdr.text.size, 300);
    ASSERT_EQ(m->hdr.rodata.memory_offset, 0x1000);
    ASSERT_EQ(m->hdr.rodata.size, 50);
    ASSERT_EQ(m->hdr.data.memory_offset, 0x2000);
    ASSERT_EQ(m->hdr.data.size, 20);
    ASSERT_EQ(m->hdr.bss_size, 0x100);
    ASSERT_EQ(m->image_size, 0x3000);
    ASSERT_EQ(m->data_end, 0x2000 + 20 + 0x100);

    uint8_t text[300], rodata[50], data[20];
    fill(text, sizeof(text), 1);
    fill(rodata, sizeof(rodata), 2);
    fill(data, sizeof(data), 3);
    ASSERT_MEM_EQ(m->image, text, sizeof(text));
    ASSERT_MEM_EQ(m->image + 0x1000, rodata, sizeof(rodata));
    ASSERT_MEM_EQ(m->image + 0x2000, data, sizeof(data));
    /* bss is zero */
    for (size_t i = 0; i < 0x100; i++) {
        ASSERT_EQ(m->image[0x2000 + 20 + i], 0);
    }
}

TEST(nso_write_then_load_uncompressed) {
    uint8_t *buf;
    size_t len;
    ASSERT_EQ(build(0, &buf, &len), HOS_RESULT_SUCCESS);
    ASSERT_EQ(len, 0x100 + 8 + 300 + 50 + 20);

    nso_header h;
    ASSERT_EQ(nso_parse_header(buf, len, &h), HOS_RESULT_SUCCESS);
    ASSERT_EQ(h.flags, 0);
    ASSERT_EQ(h.module_name_offset, 0x100);
    ASSERT_EQ(h.module_name_size, 8);
    ASSERT_STR_EQ((const char *)buf + h.module_name_offset, "fixture");

    nso_module m;
    ASSERT_EQ(nso_load(buf, len, NULL, &m), HOS_RESULT_SUCCESS);
    check_layout(&m);
    nso_free(&m);
    free(buf);
}

TEST(nso_write_then_load_compressed) {
    uint8_t *buf;
    size_t len;
    ASSERT_EQ(build(1, &buf, &len), HOS_RESULT_SUCCESS);

    nso_header h;
    ASSERT_EQ(nso_parse_header(buf, len, &h), HOS_RESULT_SUCCESS);
    ASSERT_EQ(h.flags,
              NSO_FLAG_TEXT_COMPRESSED | NSO_FLAG_RODATA_COMPRESSED | NSO_FLAG_DATA_COMPRESSED);
    ASSERT_TRUE(h.text_compressed_size < h.text.size);

    nso_module m;
    ASSERT_EQ(nso_load(buf, len, NULL, &m), HOS_RESULT_SUCCESS);
    check_layout(&m);
    nso_free(&m);
    free(buf);
}

/* An NRO by hand: text 0x0-0x1000 (holding the header), rodata, data, then assets
 * with a NACP whose first title is set and a small RomFS. */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

#define NRO_IMAGE 0x3000u
#define NRO_NACP  0x4000u
#define NRO_LEN   (NRO_IMAGE + 0x38 + NRO_NACP + 0x40)

static uint8_t *build_nro(void) {
    uint8_t *f = calloc(1, NRO_LEN);
    fill(f + 0x80, 0x1000 - 0x80, 1); /* text after the header */
    fill(f + 0x1000, 0x800, 2);       /* rodata */
    fill(f + 0x2000, 0x100, 3);       /* data */
    memcpy(f + 0x10, "NRO0", 4);
    put32(f + 0x18, NRO_IMAGE);
    put32(f + 0x20, 0x0000);
    put32(f + 0x24, 0x1000);
    put32(f + 0x28, 0x1000);
    put32(f + 0x2C, 0x800);
    put32(f + 0x30, 0x2000);
    put32(f + 0x34, 0x100);
    put32(f + 0x38, 0x500); /* bss */
    uint8_t *a = f + NRO_IMAGE;
    memcpy(a, "ASET", 4);
    put64(a + 0x18, 0x38); /* nacp */
    put64(a + 0x20, NRO_NACP);
    put64(a + 0x28, 0x38 + NRO_NACP); /* romfs */
    put64(a + 0x30, 0x40);
    memcpy(a + 0x38, "Fixture App", 11);
    memcpy(a + 0x38 + 0x200, "switch2pc", 9);
    memset(a + 0x38 + NRO_NACP, 0xAB, 0x40);
    return f;
}

TEST(nro_parse_and_convert) {
    uint8_t *f = build_nro();
    nro_info info;
    ASSERT_EQ(nro_parse(f, NRO_LEN, &info), HOS_RESULT_SUCCESS);
    ASSERT_EQ(info.rodata_off, 0x1000);
    ASSERT_EQ(info.data_size, 0x100);
    ASSERT_EQ(info.bss_size, 0x500);
    ASSERT_EQ(info.romfs_off, NRO_IMAGE + 0x38 + NRO_NACP);
    ASSERT_EQ(info.romfs_size, 0x40);
    ASSERT_TRUE(strcmp(info.title, "Fixture App") == 0);
    ASSERT_TRUE(strcmp(info.author, "switch2pc") == 0);

    uint8_t *nso;
    size_t len;
    ASSERT_EQ(nro_to_nso(f, NRO_LEN, &nso, &len), HOS_RESULT_SUCCESS);
    nso_module m;
    ASSERT_EQ(nso_load(nso, len, NULL, &m), HOS_RESULT_SUCCESS);
    ASSERT_EQ(m.hdr.rodata.memory_offset, 0x1000);
    ASSERT_EQ(m.hdr.data.memory_offset, 0x2000);
    ASSERT_EQ(m.hdr.bss_size, 0x500);
    /* the image is the NRO's, byte for byte, header included */
    ASSERT_TRUE(memcmp(m.image, f, NRO_IMAGE - 0x1000 + 0x100) == 0);
    nso_free(&m);
    free(nso);
    free(f);
}

TEST(nro_rejects_bad_layout) {
    uint8_t *f = build_nro();
    nro_info info;
    ASSERT_EQ(nro_parse(f, 0x40, &info), S2PC_ERR_TRUNCATED);
    put32(f + 0x28, 0x800); /* rodata overlapping text */
    ASSERT_EQ(nro_parse(f, NRO_LEN, &info), S2PC_ERR_CORRUPT);
    memcpy(f + 0x10, "NSO0", 4);
    ASSERT_EQ(nro_parse(f, NRO_LEN, &info), S2PC_ERR_BAD_MAGIC);
    free(f);
}

TEST(nso_rejects_bad_magic_and_truncation) {
    uint8_t *buf;
    size_t len;
    ASSERT_EQ(build(0, &buf, &len), HOS_RESULT_SUCCESS);
    nso_header h;
    ASSERT_EQ(nso_parse_header(buf, 0x80, &h), S2PC_ERR_TRUNCATED);
    ASSERT_EQ(nso_parse_header(buf, len - 1, &h), S2PC_ERR_TRUNCATED);
    buf[0] = 'X';
    ASSERT_EQ(nso_parse_header(buf, len, &h), S2PC_ERR_BAD_MAGIC);
    free(buf);
}

TEST(nso_rejects_corrupt_lz4) {
    uint8_t *buf;
    size_t len;
    ASSERT_EQ(build(1, &buf, &len), HOS_RESULT_SUCCESS);
    nso_header h;
    ASSERT_EQ(nso_parse_header(buf, len, &h), HOS_RESULT_SUCCESS);
    /* Smash the text block: offset byte of the first match becomes 0. */
    for (uint32_t i = 0; i < h.text_compressed_size; i++) {
        buf[h.text.file_offset + i] = 0xFF;
    }
    nso_module m;
    ASSERT_EQ(nso_load(buf, len, NULL, &m), S2PC_ERR_CORRUPT);
    free(buf);
}

TEST(mod0_absent) {
    uint8_t image[0x1000] = {0};
    mod0_info mi;
    ASSERT_EQ(mod0_find(image, sizeof(image), &mi), HOS_RESULT_SUCCESS);
    ASSERT_EQ(mi.present, 0);
}

/*
 * Image layout:
 *   0x000  branch word, 0x004 = MOD0 offset (0x100)
 *   0x100  MOD0 header, dynamic at +0x100 (0x200), bss [0x800, 0x900)
 *   0x200  .dynamic: RELA 0x300/48, JMPREL 0x330/24, PLTREL RELA, SYMTAB 0x400, STRTAB 0x500/16
 *   0x300  RELA: RELATIVE @0x600 +0x1234 ; ABS64 sym1 @0x608 +0x10
 *   0x330  JMPREL: JUMP_SLOT sym2 (undefined) @0x610
 *   0x400  symtab: [0] null, [1] defined value 0x700, [2] undefined
 *   0x500  strtab: "\0foo\0bar\0"
 */
static void build_mod0_image(uint8_t *img) {
    memset(img, 0, 0x1000);
    put32(img + 4, 0x100);
    memcpy(img + 0x100, "MOD0", 4);
    put32(img + 0x104, 0x100); /* dynamic */
    put32(img + 0x108, 0x700); /* bss start = 0x800 */
    put32(img + 0x10C, 0x800); /* bss end = 0x900 */
    put32(img + 0x110, 0);
    put32(img + 0x114, 0);

    Elf64_Dyn dyn[] = {
        {DT_RELA, {0x300}},  {DT_RELASZ, {48}},      {DT_RELAENT, {24}},   {DT_JMPREL, {0x330}},
        {DT_PLTRELSZ, {24}}, {DT_PLTREL, {DT_RELA}}, {DT_SYMTAB, {0x400}}, {DT_STRTAB, {0x500}},
        {DT_STRSZ, {16}},    {DT_NULL, {0}},
    };
    memcpy(img + 0x200, dyn, sizeof(dyn));

    Elf64_Rela rela[] = {
        {0x600, ELF64_R_INFO(0, R_AARCH64_RELATIVE), 0x1234},
        {0x608, ELF64_R_INFO(1, R_AARCH64_ABS64), 0x10},
    };
    memcpy(img + 0x300, rela, sizeof(rela));
    Elf64_Rela jmp[] = {
        {0x610, ELF64_R_INFO(2, R_AARCH64_JUMP_SLOT), 0},
    };
    memcpy(img + 0x330, jmp, sizeof(jmp));

    Elf64_Sym syms[3];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = 1;
    syms[1].st_value = 0x700;
    syms[1].st_shndx = 1;
    syms[2].st_name = 5;
    syms[2].st_shndx = SHN_UNDEF;
    memcpy(img + 0x400, syms, sizeof(syms));
    memcpy(img + 0x500, "\0foo\0bar\0", 9);

    put64(img + 0x610, 0xDEADBEEFull); /* must be zeroed by the unresolved slot */
}

TEST(mod0_find_and_relocate) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);

    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    ASSERT_EQ(mi.present, 1);
    ASSERT_EQ(mi.mod0_off, 0x100);
    ASSERT_EQ(mi.dynamic_off, 0x200);
    ASSERT_EQ(mi.bss_start_off, 0x800);
    ASSERT_EQ(mi.bss_end_off, 0x900);

    mod0_reloc_stats st;
    ASSERT_EQ(mod0_relocate(img, 0x1000, 0x10000, &mi, &st), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.applied, 2);
    ASSERT_EQ(st.unresolved, 1);
    ASSERT_EQ(st.unsupported, 0);

    uint64_t v;
    memcpy(&v, img + 0x600, 8);
    ASSERT_EQ(v, 0x10000 + 0x1234);
    memcpy(&v, img + 0x608, 8);
    ASSERT_EQ(v, 0x10000 + 0x700 + 0x10);
    memcpy(&v, img + 0x610, 8);
    ASSERT_EQ(v, 0);
    free(img);
}

/* Replace the value of `tag` in the fixture's .dynamic (at 0x200). */
static void set_dyn(uint8_t *img, int64_t tag, uint64_t value) {
    for (size_t off = 0x200;; off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn e;
        memcpy(&e, img + off, sizeof(e));
        if (e.d_tag == DT_NULL) {
            return;
        }
        if (e.d_tag == tag) {
            e.d_un.d_val = value;
            memcpy(img + off, &e, sizeof(e));
            return;
        }
    }
}

/* Append a dynamic entry before the terminator; the fixture leaves room. */
static void add_dyn(uint8_t *img, int64_t tag, uint64_t value) {
    for (size_t off = 0x200;; off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn e;
        memcpy(&e, img + off, sizeof(e));
        if (e.d_tag == DT_NULL) {
            Elf64_Dyn n = {tag, {value}};
            memcpy(img + off, &n, sizeof(n));
            memcpy(img + off + sizeof(n), &e, sizeof(e));
            return;
        }
    }
}

/*
 * Offsets from a module are 64-bit words the file chooses; a value near 2^64
 * made "offset + length <= size" wrap and pass, and the loader read or wrote
 * far outside the image. Each case below did that before the bounds were
 * written as "offset <= size && length <= size - offset".
 */
TEST(mod0_relocation_target_near_top_of_address_space) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    Elf64_Rela r = {0xFFFFFFFFFFFFFFF8ull, ELF64_R_INFO(0, R_AARCH64_RELATIVE), 0};
    memcpy(img + 0x300, &r, sizeof(r));
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    mod0_reloc_stats st;
    ASSERT_EQ(mod0_relocate(img, 0x1000, 0x10000, &mi, &st), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.unsupported, 1); /* skipped, like any target outside the image */
    ASSERT_EQ(st.applied, 1);
    free(img);
}

TEST(mod0_rela_table_near_top_of_address_space) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    set_dyn(img, DT_RELA, 0xFFFFFFFFFFFFFFF0ull); /* + RELASZ 48 wraps to 32 */
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    ASSERT_EQ(mod0_relocate(img, 0x1000, 0x10000, &mi, NULL), S2PC_ERR_CORRUPT);
    free(img);
}

TEST(mod0_rel_entry_near_top_of_address_space) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    set_dyn(img, DT_PLTREL, DT_REL); /* JMPREL read as REL: addend from the target word */
    Elf64_Rel r = {0xFFFFFFFFFFFFFFFCull, ELF64_R_INFO(0, R_AARCH64_RELATIVE)};
    memcpy(img + 0x330, &r, sizeof(r));
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    mod0_reloc_stats st;
    ASSERT_EQ(mod0_relocate(img, 0x1000, 0x10000, &mi, &st), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.unsupported, 1);
    free(img);
}

TEST(mod0_symtab_near_top_of_address_space) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    set_dyn(img, DT_SYMTAB, 0xFFFFFFFFFFFFFFE8ull); /* symbol 1 lands at offset 0 after wrap */
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    mod0_reloc_stats st;
    ASSERT_EQ(mod0_relocate(img, 0x1000, 0x10000, &mi, &st), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.unresolved, 2); /* no symbol table to resolve from */
    mod0_module m = {.image = img, .size = 0x1000, .base = 0x10000, .info = mi};
    uint64_t v;
    ASSERT_EQ(mod0_lookup(&m, "foo", &v), 0);
    free(img);
}

TEST(mod0_hash_tables_near_top_of_address_space) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    add_dyn(img, DT_HASH, 0xFFFFFFFFFFFFFFFCull);
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    mod0_module m = {.image = img, .size = 0x1000, .base = 0x10000, .info = mi};
    uint64_t v = 0;
    mod0_lookup(&m, "foo", &v); /* must not read outside the image */
    set_dyn(img, DT_HASH, 0);
    add_dyn(img, DT_GNU_HASH, 0xFFFFFFFFFFFFFFF8ull);
    mod0_lookup(&m, "foo", &v);
    free(img);
}

TEST(mod0_symbol_name_runs_to_the_image_end) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    set_dyn(img, DT_STRTAB, 0xFFC); /* "foo" as the last four bytes, no terminator */
    set_dyn(img, DT_STRSZ, 0x100);
    memcpy(img + 0xFFC, "ffoo", 4);
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), HOS_RESULT_SUCCESS);
    mod0_module m = {.image = img, .size = 0x1000, .base = 0x10000, .info = mi};
    uint64_t v = 0;
    ASSERT_EQ(mod0_lookup(&m, "foo", &v), 0); /* an unterminated name matches nothing */
    free(img);
}

TEST(mod0_rejects_out_of_range_fields) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    put32(img + 0x104, 0x7FFFFFF0); /* dynamic far outside */
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), S2PC_ERR_CORRUPT);
    free(img);
}
