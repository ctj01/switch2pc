/*
 * switch2pc: inspect, patch and package NSO modules.
 *
 *   switch2pc info    <module.nso>              header, MOD0, patch summary
 *   switch2pc scan    <module.nso>              list every instruction that will be rewritten
 *   switch2pc patch   <in.nso> <out.nso>        write a pre-patched, uncompressed copy
 *   switch2pc mknso   --text <bin> -o <out.nso> [--rodata <bin>] [--data <bin>] [--bss <n>]
 *                     [--name <str>] [--compress]
 *   switch2pc elf2nso <in.elf> -o <out.nso> [--name <str>] [--compress]
 *
 * This binary runs on the build host (any arch). Running the result is hosrun's job.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_file.h"
#include "common/hos_log.h"
#include "nso/elf2nso.h"
#include "nso/mod0.h"
#include "nso/nso.h"
#include "patch/patch.h"

#define MOD "cli"

static void usage(FILE *out) {
    fputs("usage:\n"
          "  switch2pc info    <module.nso>\n"
          "  switch2pc scan    <module.nso>\n"
          "  switch2pc patch   <in.nso> <out.nso>\n"
          "  switch2pc mknso   --text <bin> -o <out.nso> [--rodata <bin>] [--data <bin>]\n"
          "                    [--bss <bytes>] [--name <module name>] [--compress]\n"
          "  switch2pc elf2nso <in.elf> -o <out.nso> [--name <module name>] [--compress]\n"
          "\n"
          "environment:\n"
          "  HOS_LOG_LEVEL   error|warn|info|debug|trace (default info)\n",
          out);
}

static int fail(hos_result r, const char *what) {
    HOS_LOGE(MOD, "%s failed: module %u desc %u", what, HOS_RESULT_MODULE(r), HOS_RESULT_DESC(r));
    return 1;
}

static void print_hex(const char *label, const uint8_t *p, size_t n) {
    printf("  %-14s", label);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

static void print_segment(const char *name, const nso_segment_header *s, int compressed,
                          uint32_t compressed_size) {
    printf("  %-8s file %#010x  mem %#010x  size %#010x", name, s->file_offset, s->memory_offset,
           s->size);
    if (compressed) {
        printf("  lz4 %#x", compressed_size);
    }
    printf("\n");
}

static int load_module(const char *path, nso_module *m) {
    uint8_t *buf;
    size_t len;
    hos_result r = hos_file_read_all(path, &buf, &len);
    if (HOS_FAILED(r)) {
        return fail(r, "read");
    }
    r = nso_load(buf, len, NULL, m);
    free(buf);
    if (HOS_FAILED(r)) {
        return fail(r, "nso_load");
    }
    return 0;
}

static int cmd_info(const char *path) {
    nso_module m;
    if (load_module(path, &m)) {
        return 1;
    }
    const nso_header *h = &m.hdr;

    printf("%s\n", path);
    printf("  version       %u   flags %#x\n", h->version, h->flags);
    print_segment("text", &h->text, h->flags & NSO_FLAG_TEXT_COMPRESSED, h->text_compressed_size);
    print_segment("rodata", &h->rodata, h->flags & NSO_FLAG_RODATA_COMPRESSED,
                  h->rodata_compressed_size);
    print_segment("data", &h->data, h->flags & NSO_FLAG_DATA_COMPRESSED, h->data_compressed_size);
    printf("  bss           %#x\n", h->bss_size);
    printf("  image         %#zx bytes\n", m.image_size);
    print_hex("module id", h->module_id, sizeof(h->module_id));

    mod0_info mi;
    hos_result r = mod0_find(m.image, m.image_size, &mi);
    if (HOS_FAILED(r)) {
        nso_free(&m);
        return fail(r, "mod0_find");
    }
    if (mi.present) {
        printf("  MOD0          at %#" PRIx64 "  dynamic %#" PRIx64 "  bss [%#" PRIx64 ", %#" PRIx64
               ")\n",
               mi.mod0_off, mi.dynamic_off, mi.bss_start_off, mi.bss_end_off);
        mod0_reloc_stats rs;
        if (HOS_OK(mod0_relocate(m.image, m.image_size, 0, &mi, &rs))) {
            printf("  relocations   %zu applied, %zu unresolved, %zu unsupported, %zu relr "
                   "(applied by the module itself)\n",
                   rs.applied, rs.unresolved, rs.unsupported, rs.deferred);
        }
    } else {
        printf("  MOD0          absent\n");
    }

    patch_stats st;
    patch_text(m.image + h->text.memory_offset, h->text.size, 0, &st, NULL, NULL);
    printf("  patch sites   %zu total", st.total);
    for (int k = 0; k < PATCH_KIND_COUNT; k++) {
        printf(", %zu %s", st.by_kind[k], patch_kind_name((patch_kind)k));
    }
    printf("\n");

    nso_free(&m);
    return 0;
}

static void print_site(const patch_site *s, void *user) {
    (void)user;
    if (s->kind == PATCH_SVC_TO_BRK) {
        printf("  %#010" PRIx64 "  %08x -> %08x  %s  svc %#x\n", s->offset, s->before, s->after,
               patch_kind_name(s->kind), s->imm);
    } else {
        printf("  %#010" PRIx64 "  %08x -> %08x  %s\n", s->offset, s->before, s->after,
               patch_kind_name(s->kind));
    }
}

static int cmd_scan(const char *path) {
    nso_module m;
    if (load_module(path, &m)) {
        return 1;
    }
    printf("%s: text offsets\n", path);
    patch_stats st;
    patch_text(m.image + m.hdr.text.memory_offset, m.hdr.text.size, 0, &st, print_site, NULL);
    printf("  %zu sites\n", st.total);
    nso_free(&m);
    return 0;
}

static int write_out(const char *out, uint8_t *buf, size_t len, const char *what) {
    hos_result r = hos_file_write_all(out, buf, len);
    free(buf);
    if (HOS_FAILED(r)) {
        return fail(r, what);
    }
    return 0;
}

static int cmd_patch(const char *in, const char *out) {
    nso_module m;
    if (load_module(in, &m)) {
        return 1;
    }
    patch_stats st;
    patch_text(m.image + m.hdr.text.memory_offset, m.hdr.text.size, 1, &st, NULL, NULL);

    nso_write_params p = {
        .text = m.image + m.hdr.text.memory_offset,
        .text_size = m.hdr.text.size,
        .rodata = m.image + m.hdr.rodata.memory_offset,
        .rodata_size = m.hdr.rodata.size,
        .data = m.image + m.hdr.data.memory_offset,
        .data_size = m.hdr.data.size,
        .bss_size = m.hdr.bss_size,
        .module_name = NULL,
        .compress = 0,
        .explicit_offsets = 1,
        .text_mem_off = m.hdr.text.memory_offset,
        .rodata_mem_off = m.hdr.rodata.memory_offset,
        .data_mem_off = m.hdr.data.memory_offset,
    };
    uint8_t *buf;
    size_t len;
    hos_result r = nso_write(&p, &buf, &len);
    nso_free(&m);
    if (HOS_FAILED(r)) {
        return fail(r, "nso_write");
    }
    if (write_out(out, buf, len, "write")) {
        return 1;
    }
    HOS_LOGI(MOD, "patched %zu sites -> %s (%zu bytes)", st.total, out, len);
    return 0;
}

static int read_optional(const char *path, uint8_t **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (!path) {
        return 0;
    }
    return HOS_FAILED(hos_file_read_all(path, buf, len)) ? 1 : 0;
}

static int cmd_mknso(int argc, char **argv) {
    const char *text = NULL, *rodata = NULL, *data = NULL, *out = NULL, *name = NULL;
    size_t bss = 0;
    int compress = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--compress") == 0) {
            compress = 1;
        } else if (!v) {
            fprintf(stderr, "mknso: %s needs a value\n", a);
            return 2;
        } else if (strcmp(a, "--text") == 0) {
            text = v, i++;
        } else if (strcmp(a, "--rodata") == 0) {
            rodata = v, i++;
        } else if (strcmp(a, "--data") == 0) {
            data = v, i++;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            out = v, i++;
        } else if (strcmp(a, "--name") == 0) {
            name = v, i++;
        } else if (strcmp(a, "--bss") == 0) {
            bss = strtoull(v, NULL, 0), i++;
        } else {
            fprintf(stderr, "mknso: unknown option %s\n", a);
            return 2;
        }
    }
    if (!text || !out) {
        fprintf(stderr, "mknso: --text and -o are required\n");
        return 2;
    }

    uint8_t *tb, *rb, *db;
    size_t tl, rl, dl;
    if (read_optional(text, &tb, &tl) || read_optional(rodata, &rb, &rl) ||
        read_optional(data, &db, &dl)) {
        return 1;
    }
    nso_write_params p = {
        .text = tb,
        .text_size = tl,
        .rodata = rb,
        .rodata_size = rl,
        .data = db,
        .data_size = dl,
        .bss_size = bss,
        .module_name = name,
        .compress = compress,
    };
    uint8_t *buf;
    size_t len;
    hos_result r = nso_write(&p, &buf, &len);
    free(tb);
    free(rb);
    free(db);
    if (HOS_FAILED(r)) {
        return fail(r, "nso_write");
    }
    if (write_out(out, buf, len, "write")) {
        return 1;
    }
    HOS_LOGI(MOD, "wrote %s (%zu bytes, text %zu rodata %zu data %zu bss %zu)", out, len, tl, rl,
             dl, bss);
    return 0;
}

static int cmd_elf2nso(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *name = NULL;
    int compress = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--compress") == 0) {
            compress = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (!v) {
                return 2;
            }
            out = v, i++;
        } else if (strcmp(a, "--name") == 0) {
            if (!v) {
                return 2;
            }
            name = v, i++;
        } else if (a[0] == '-') {
            fprintf(stderr, "elf2nso: unknown option %s\n", a);
            return 2;
        } else {
            in = a;
        }
    }
    if (!in || !out) {
        fprintf(stderr, "elf2nso: input and -o are required\n");
        return 2;
    }
    uint8_t *elf;
    size_t len;
    hos_result r = hos_file_read_all(in, &elf, &len);
    if (HOS_FAILED(r)) {
        return fail(r, "read");
    }
    uint8_t *buf;
    size_t out_len;
    r = elf2nso_convert(elf, len, name, compress, &buf, &out_len);
    free(elf);
    if (HOS_FAILED(r)) {
        return fail(r, "elf2nso");
    }
    if (write_out(out, buf, out_len, "write")) {
        return 1;
    }
    HOS_LOGI(MOD, "wrote %s (%zu bytes)", out, out_len);
    return 0;
}

int main(int argc, char **argv) {
    hos_log_init_from_env();
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "info") == 0 && argc == 3) {
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "scan") == 0 && argc == 3) {
        return cmd_scan(argv[2]);
    }
    if (strcmp(cmd, "patch") == 0 && argc == 4) {
        return cmd_patch(argv[2], argv[3]);
    }
    if (strcmp(cmd, "mknso") == 0) {
        return cmd_mknso(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "elf2nso") == 0) {
        return cmd_elf2nso(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    usage(stderr);
    return 2;
}
