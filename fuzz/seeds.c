/*
 * Seed corpus for the fuzzers, written to <dir>/{nso,mod0,hipc}/: NSOs from
 * nso_write (plain and LZ4), the MOD0 image tests/test_nso.c relocates, and
 * HIPC requests from examples/kernel_test/ipc_client.h. Nothing is committed;
 * scripts/fuzz.sh runs this before the fuzzers.
 */

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ipc_client.h"
#include "nso/nso.h"

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static int save(const char *dir, const char *sub, const char *name, const void *data, size_t n) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, sub);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/%s/%s", dir, sub, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i * 7);
    }
}

/* The layout tests/test_nso.c describes: MOD0 at 0x100, dynamic at 0x200, RELA,
 * JMPREL, a symbol table and a string table, bss [0x800, 0x900). */
static void build_mod0_image(uint8_t *img) {
    memset(img, 0, 0x1000);
    put32(img + 4, 0x100);
    memcpy(img + 0x100, "MOD0", 4);
    put32(img + 0x104, 0x100);
    put32(img + 0x108, 0x700);
    put32(img + 0x10C, 0x800);
    Elf64_Dyn dyn[] = {
        {DT_RELA, {0x300}},   {DT_RELASZ, {48}},        {DT_RELAENT, {24}},
        {DT_JMPREL, {0x330}}, {DT_PLTRELSZ, {24}},      {DT_PLTREL, {DT_RELA}},
        {DT_SYMTAB, {0x400}}, {DT_STRTAB, {0x500}},     {DT_STRSZ, {16}},
        {DT_INIT, {0x700}},   {DT_INIT_ARRAY, {0x620}}, {DT_INIT_ARRAYSZ, {8}},
        {DT_RELR, {0x640}},   {DT_RELRSZ, {16}},        {DT_NULL, {0}},
    };
    memcpy(img + 0x200, dyn, sizeof(dyn));
    Elf64_Rela rela[] = {
        {0x600, ELF64_R_INFO(0, R_AARCH64_RELATIVE), 0x1234},
        {0x608, ELF64_R_INFO(1, R_AARCH64_ABS64), 0x10},
    };
    memcpy(img + 0x300, rela, sizeof(rela));
    Elf64_Rela jmp[] = {{0x610, ELF64_R_INFO(2, R_AARCH64_JUMP_SLOT), 0}};
    memcpy(img + 0x330, jmp, sizeof(jmp));
    Elf64_Sym syms[3];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = 1;
    syms[1].st_value = 0x700;
    syms[1].st_shndx = 1;
    syms[2].st_name = 5;
    memcpy(img + 0x400, syms, sizeof(syms));
    memcpy(img + 0x500, "\0foo\0bar\0", 9);
    put64(img + 0x610, 0xDEADBEEFull);
    put64(img + 0x620, 0x8000000 + 0x700);
    put64(img + 0x640, 0x660); /* RELR: one address, then a bitmap of the next word */
    put64(img + 0x648, 0x3);
}

static int seed_nso(const char *dir) {
    uint8_t text[0x300], rodata[0x80], data[0x40];
    fill(text, sizeof(text), 1);
    fill(rodata, sizeof(rodata), 2);
    fill(data, sizeof(data), 3);
    static const char *names[2] = {"plain.nso", "lz4.nso"};
    for (int compress = 0; compress < 2; compress++) {
        nso_write_params p = {
            .text = text,
            .text_size = sizeof(text),
            .rodata = rodata,
            .rodata_size = sizeof(rodata),
            .data = data,
            .data_size = sizeof(data),
            .bss_size = 0x20,
            .module_name = "seed",
            .compress = compress,
        };
        uint8_t *buf;
        size_t len;
        if (nso_write(&p, &buf, &len) != HOS_RESULT_SUCCESS) {
            return 0;
        }
        int ok = save(dir, "nso", names[compress], buf, len);
        free(buf);
        if (!ok) {
            return 0;
        }
    }
    /* A module carrying MOD0 in its text, as every real one does. */
    uint8_t img[0x1000];
    build_mod0_image(img);
    nso_write_params p = {.text = img, .text_size = sizeof(img), .module_name = "mod0"};
    uint8_t *buf;
    size_t len;
    if (nso_write(&p, &buf, &len) != HOS_RESULT_SUCCESS) {
        return 0;
    }
    int ok = save(dir, "nso", "mod0.nso", buf, len);
    free(buf);
    return ok;
}

static int seed_mod0(const char *dir) {
    uint8_t img[0x1000];
    build_mod0_image(img);
    if (!save(dir, "mod0", "relocs.bin", img, sizeof(img))) {
        return 0;
    }
    uint8_t none[0x100];
    memset(none, 0, sizeof(none));
    return save(dir, "mod0", "no_mod0.bin", none, sizeof(none));
}

static int seed_hipc(const char *dir) {
    uint32_t tls[0x40];
    uint32_t words[4] = {1, 2, 3, 4};
    uint8_t payload[0x40];
    memset(payload, 0xAB, sizeof(payload));
    ipc_request cmif = {.type = IPC_TYPE_REQUEST,
                        .cmd = 7,
                        .send_pid = 1,
                        .data = words,
                        .data_size = sizeof(words),
                        .a_buf = payload,
                        .a_size = sizeof(payload),
                        .b_buf = payload,
                        .b_size = 0x20,
                        .x_buf = payload,
                        .x_size = 0x10,
                        .c_buf = payload,
                        .c_size = 0x10,
                        .copy_handles = {0x1234, 0x5678},
                        .num_copy = 2};
    ipc_write_request(tls, &cmif);
    if (!save(dir, "hipc", "cmif_buffers.bin", tls, sizeof(tls))) {
        return 0;
    }
    ipc_request domain = {.type = IPC_TYPE_REQUEST,
                          .cmd = 1,
                          .data = words,
                          .data_size = 8,
                          .domain = 1,
                          .object_id = 3,
                          .domain_cmd = 1};
    ipc_write_request(tls, &domain);
    if (!save(dir, "hipc", "cmif_domain.bin", tls, sizeof(tls))) {
        return 0;
    }
    ipc_request tipc = {.type = IPC_TYPE_TIPC(2), .data = words, .data_size = 12};
    ipc_write_request(tls, &tipc);
    if (!save(dir, "hipc", "tipc.bin", tls, sizeof(tls))) {
        return 0;
    }
    ipc_request close = {.type = IPC_TYPE_CLOSE};
    ipc_write_request(tls, &close);
    return save(dir, "hipc", "close.bin", tls, sizeof(tls));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fuzz_seeds <corpus dir>\n");
        return 2;
    }
    mkdir(argv[1], 0755);
    return seed_nso(argv[1]) && seed_mod0(argv[1]) && seed_hipc(argv[1]) ? 0 : 1;
}
