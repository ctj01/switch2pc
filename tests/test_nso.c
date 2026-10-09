#include <elf.h>
#include <stdlib.h>

#include "nso/mod0.h"
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

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
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

TEST(mod0_rejects_out_of_range_fields) {
    uint8_t *img = malloc(0x1000);
    build_mod0_image(img);
    put32(img + 0x104, 0x7FFFFFF0); /* dynamic far outside */
    mod0_info mi;
    ASSERT_EQ(mod0_find(img, 0x1000, &mi), S2PC_ERR_CORRUPT);
    free(img);
}
