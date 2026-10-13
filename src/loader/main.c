/*
 * hosrun: load one or more NSO modules and run them natively in this process.
 *
 *   hosrun [options] <module.nso> [more.nso ...]
 *   hosrun [options] <app.nro>                   homebrew as hbmenu launches it
 *   hosrun [options] <exefs directory>          loads rtld, main, subsdk0-9, sdk
 *     --stack <bytes>    main thread stack (default 8 MiB)
 *     --no-patch         leave svc/mrs instructions alone (debugging aid)
 *     --romfs <file>     romfs image served as the data storage
 *     --sdmc <dir>       directory served as the SD card
 *     --save <dir>       directory served as save data
 *
 * Modules are mapped back to back in the order given, as the kernel lays an
 * exefs out, so a runtime linker that walks memory finds them. Relocations
 * are resolved across all modules; the entry point is the first module.
 *
 * An NRO is converted to an uncompressed NSO and started the same way, as an
 * application rather than through hbloader's ABI; the RomFS in its assets
 * becomes the data storage unless --romfs names another.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common/hos_file.h"
#include "common/hos_log.h"
#include "hos_cpu.h"
#include "nso/mod0.h"
#include "nso/nro.h"
#include "nso/nso.h"
#include "patch/patch.h"

#define MOD           "hosrun"
#define DEFAULT_STACK (8u << 20)
#define IMAGE_BASE    0x8000000ull
#define MAX_MODULES   HOS_MAX_MODULES

typedef struct loaded_module {
    char path[4096];
    const char *name;
    uint8_t *file;
    size_t file_len;
    nso_module nso;
} loaded_module;

static void usage(FILE *out) {
    fputs("usage: hosrun [options] <module.nso> [more.nso ...]\n"
          "       hosrun [options] <app.nro>\n"
          "       hosrun [options] <exefs directory>\n"
          "  --stack <bytes>   main thread stack size (default 8 MiB)\n"
          "  --no-patch        do not rewrite svc/mrs instructions\n"
          "  --romfs <path>    romfs image or directory for OpenDataStorageByCurrentProcess\n"
          "                    (default for an .nro: the RomFS in its assets)\n"
          "  --sdmc <dir>      directory served as the SD card\n"
          "  --save <dir>      directory served as save data\n"
          "  --program-id <hex>  title id (e.g. 0000000000010000)\n"
          "\n"
          "environment:\n"
          "  HOS_LOG_LEVEL     error|warn|info|debug|trace (default info)\n"
          "  HOS_BREAK         continue  resume after a non-notification svcBreak\n"
          "  HOS_ROMFS, HOS_SDMC, HOS_SAVE   same as the options above\n",
          out);
}

static int die(hos_result r, const char *what) {
    HOS_LOGE(MOD, "%s failed: module %u desc %u", what, HOS_RESULT_MODULE(r), HOS_RESULT_DESC(r));
    return 1;
}

static int protect(uint8_t *base, size_t off, size_t size, int prot, const char *what) {
    if (size == 0) {
        return 0;
    }
    size_t start = off & ~0xFFFull;
    size_t end = (off + size + 0xFFF) & ~0xFFFull;
    if (mprotect(base + start, end - start, prot) != 0) {
        HOS_LOGE(MOD, "mprotect(%s): %s", what, strerror(errno));
        return 1;
    }
    return 0;
}

/*
 * Library initialisers without rtld (#21). On hardware rtld runs DT_INIT and
 * DT_INIT_ARRAY of every module before the entry point; libnx's crt0 does its
 * own module's, so only the other modules need it. The loader writes a page
 * of ARM64 code that calls each initialiser in turn and then branches to the
 * real entry, and makes that page the entry: initialisers run on the main
 * thread with its stack and TLS, exactly as under rtld.
 *
 *     mov  x19, x0 ; mov x20, x1        keep the entry arguments
 *     ldr  x16, =init_k ; blr x16       for each initialiser
 *     mov  x0, x19 ; mov x1, x20
 *     ldr  x16, =entry ; br x16
 *     .quad init_0 ... init_n-1, entry
 */
#define A64_MOV_X19_X0 0xaa0003f3u
#define A64_MOV_X20_X1 0xaa0103f4u
#define A64_MOV_X0_X19 0xaa1303e0u
#define A64_MOV_X1_X20 0xaa1403e1u
#define A64_BLR_X16    0xd63f0200u
#define A64_BR_X16     0xd61f0200u
#define INIT_PAGE      0x1000u

static uint32_t a64_ldr_x16_literal(size_t insn_index, size_t literal_byte_off) {
    int64_t delta = (int64_t)literal_byte_off - (int64_t)(insn_index * 4);
    return 0x58000000u | ((uint32_t)((delta / 4) & 0x7FFFF) << 5) | 16u;
}

/* Returns the page address, or 0 on failure. */
static uint64_t build_init_trampoline(uint64_t at, const uint64_t *fns, size_t n, uint64_t entry) {
    size_t code_words = 2 + 2 * n + 2 + 2;
    size_t lit_off = (code_words * 4 + 7) & ~(size_t)7;
    if (lit_off + (n + 1) * 8 > INIT_PAGE) {
        HOS_LOGE(MOD, "too many initialisers (%zu) for one page", n);
        return 0;
    }
    void *page = mmap((void *)at, INIT_PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (page == MAP_FAILED || (uint64_t)page != at) {
        HOS_LOGE(MOD, "cannot map the init trampoline at %#" PRIx64 ": %s", at, strerror(errno));
        return 0;
    }
    uint32_t *w = page;
    uint64_t *lit = (uint64_t *)((uint8_t *)page + lit_off);
    size_t i = 0;
    w[i++] = A64_MOV_X19_X0;
    w[i++] = A64_MOV_X20_X1;
    for (size_t k = 0; k < n; k++) {
        lit[k] = fns[k];
        w[i] = a64_ldr_x16_literal(i, lit_off + k * 8);
        i++;
        w[i++] = A64_BLR_X16;
    }
    w[i++] = A64_MOV_X0_X19;
    w[i++] = A64_MOV_X1_X20;
    lit[n] = entry;
    w[i] = a64_ldr_x16_literal(i, lit_off + n * 8);
    i++;
    w[i++] = A64_BR_X16;
    __builtin___clear_cache((char *)page, (char *)page + INIT_PAGE);
    if (mprotect(page, INIT_PAGE, PROT_READ | PROT_EXEC) != 0) {
        HOS_LOGE(MOD, "mprotect init trampoline: %s", strerror(errno));
        return 0;
    }
    return at;
}

/* Replace an NRO's bytes with the equivalent NSO; serve its RomFS. */
static int load_nro(loaded_module *m, int index, int count) {
    nro_info info;
    hos_result r = nro_parse(m->file, m->file_len, &info);
    if (HOS_FAILED(r)) {
        HOS_LOGE(MOD, "%s is not a valid NRO", m->path);
        return die(r, "nro_parse");
    }
    if (index != 0 || count != 1) {
        HOS_LOGE(MOD, "an NRO runs on its own (got %d modules)", count);
        return 1;
    }
    uint8_t *nso = NULL;
    size_t nso_len = 0;
    r = nro_to_nso(m->file, m->file_len, &nso, &nso_len);
    if (HOS_FAILED(r)) {
        return die(r, "nro_to_nso");
    }
    free(m->file);
    m->file = nso;
    m->file_len = nso_len;
    if (info.title[0]) {
        HOS_LOGI(MOD, "%s: %s%s%s", m->name, info.title, info.author[0] ? " by " : "", info.author);
    }
    const char *romfs = getenv("HOS_ROMFS");
    if (info.romfs_size && (!romfs || !*romfs)) {
        char off[32], size[32];
        snprintf(off, sizeof(off), "%" PRIu64, info.romfs_off);
        snprintf(size, sizeof(size), "%" PRIu64, info.romfs_size);
        setenv("HOS_ROMFS", m->path, 1);
        setenv("HOS_ROMFS_OFFSET", off, 1);
        setenv("HOS_ROMFS_SIZE", size, 1);
        HOS_LOGI(MOD, "romfs: %" PRIu64 " bytes in the NRO's assets", info.romfs_size);
    }
    return 0;
}

static int take_value(int argc, char **argv, int *i, const char **out) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", argv[*i]);
        return 0;
    }
    *out = argv[++*i];
    return 1;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int add_module(loaded_module *mods, int *count, const char *path) {
    if (*count >= MAX_MODULES) {
        HOS_LOGE(MOD, "too many modules (max %d)", MAX_MODULES);
        return 0;
    }
    loaded_module *m = &mods[(*count)++];
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->name = basename_of(m->path);
    return 1;
}

/* exefs order: rtld first, then main, subsdk0..9, sdk. Missing files are skipped. */
static int add_exefs(loaded_module *mods, int *count, const char *dir) {
    static const char *const order[] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                        "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                        "subsdk8", "subsdk9", "sdk"};
    int added = 0;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, order[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (!add_module(mods, count, path)) {
                return 0;
            }
            added++;
        }
    }
    if (!added) {
        HOS_LOGE(MOD, "%s: no rtld/main/sdk modules found", dir);
    }
    return added;
}

int main(int argc, char **argv) {
    hos_log_init_from_env();

    static loaded_module mods[MAX_MODULES];
    int count = 0;
    const char *value = NULL;
    uint64_t stack_size = DEFAULT_STACK;
    uint64_t program_id = 0;
    int apply_patches = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--stack") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                return 2;
            }
            stack_size = strtoull(value, NULL, 0);
        } else if (strcmp(a, "--no-patch") == 0) {
            apply_patches = 0;
        } else if (strcmp(a, "--romfs") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                return 2;
            }
            setenv("HOS_ROMFS", value, 1);
        } else if (strcmp(a, "--sdmc") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                return 2;
            }
            setenv("HOS_SDMC", value, 1);
        } else if (strcmp(a, "--save") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                return 2;
            }
            setenv("HOS_SAVE", value, 1);
        } else if (strcmp(a, "--program-id") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                return 2;
            }
            program_id = strtoull(value, NULL, 16);
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (a[0] == '-') {
            usage(stderr);
            return 2;
        } else if (is_directory(a)) {
            if (!add_exefs(mods, &count, a)) {
                return 1;
            }
        } else if (!add_module(mods, &count, a)) {
            return 1;
        }
    }
    if (count == 0) {
        usage(stderr);
        return 2;
    }

    /* Read and size every module, then reserve one contiguous range for all. */
    uint64_t total = 0;
    for (int i = 0; i < count; i++) {
        loaded_module *m = &mods[i];
        hos_result r = hos_file_read_all(m->path, &m->file, &m->file_len);
        if (HOS_FAILED(r)) {
            return die(r, "read");
        }
        if (nro_is(m->file, m->file_len)) {
            if (load_nro(m, i, count) != 0) {
                return 1;
            }
        }
        nso_header h;
        r = nso_parse_header(m->file, m->file_len, &h);
        if (HOS_FAILED(r)) {
            HOS_LOGE(MOD, "%s is not a valid NSO", m->path);
            return die(r, "nso_parse_header");
        }
        uint64_t end = (uint64_t)h.data.memory_offset + h.data.size + h.bss_size;
        total += (end + 0xFFF) & ~0xFFFull;
    }
    void *region = mmap((void *)IMAGE_BASE, total, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        HOS_LOGE(MOD, "cannot reserve %#" PRIx64 " bytes for modules: %s", total, strerror(errno));
        return 1;
    }
    if ((uint64_t)region != IMAGE_BASE) {
        HOS_LOGW(MOD, "modules mapped at %p instead of %#" PRIx64, region, (uint64_t)IMAGE_BASE);
    }

    /* Map, patch, protect. */
    uint64_t cursor = (uint64_t)region;
    patch_stats ps_total;
    memset(&ps_total, 0, sizeof(ps_total));
    static mod0_module link[MAX_MODULES];
    hos_cpu_params params;
    memset(&params, 0, sizeof(params));

    for (int i = 0; i < count; i++) {
        loaded_module *m = &mods[i];
        hos_result r = nso_load_at(m->file, m->file_len, (void *)cursor, &m->nso);
        free(m->file);
        m->file = NULL;
        if (HOS_FAILED(r)) {
            return die(r, "nso_load");
        }
        const nso_header *h = &m->nso.hdr;
        uint8_t *image = m->nso.image;

        if (apply_patches) {
            patch_stats ps;
            patch_text(image + h->text.memory_offset, h->text.size, 1, &ps, NULL, NULL);
            ps_total.total += ps.total;
            for (int k = 0; k < PATCH_KIND_COUNT; k++) {
                ps_total.by_kind[k] += ps.by_kind[k];
            }
        }

        link[i].name = m->name;
        link[i].image = image;
        link[i].size = m->nso.image_size;
        link[i].base = (uint64_t)image;
        r = mod0_find(image, m->nso.image_size, &link[i].info);
        if (HOS_FAILED(r)) {
            return die(r, "mod0_find");
        }

        hos_cpu_module *cm = &params.modules[params.num_modules++];
        cm->image_base = (uint64_t)image;
        cm->image_size = m->nso.image_size;
        cm->text_off = h->text.memory_offset;
        cm->text_size = h->text.size;
        cm->rodata_off = h->rodata.memory_offset;
        cm->rodata_size = h->rodata.size;
        cm->data_off = h->data.memory_offset;
        cm->data_size = (uint64_t)h->data.size + h->bss_size;
        cm->name = m->name;

        HOS_LOGI(MOD, "%-8s at %#" PRIx64 " (%#zx bytes)%s", m->name, (uint64_t)image,
                 m->nso.image_size, link[i].info.present ? "" : "  [no MOD0]");
        cursor += m->nso.image_size;
    }

    /* Without rtld nobody else relocates or initialises library modules (#21, #22).
     * Module 0 is the entry: libnx's crt0 applies its own RELR and init_array. */
    int has_rtld = 0;
    for (int i = 0; i < count; i++) {
        has_rtld |= strncmp(mods[i].name, "rtld", 4) == 0 &&
                    (mods[i].name[4] == 0 || mods[i].name[4] == '.');
    }
    for (int i = 0; i < count; i++) {
        link[i].apply_relr = !has_rtld && i > 0;
    }

    mod0_reloc_stats rs;
    hos_result r = mod0_link(link, (size_t)count, &rs);
    if (HOS_FAILED(r)) {
        return die(r, "mod0_link");
    }
    if (rs.unresolved) {
        HOS_LOGW(MOD, "%zu unresolved symbols; run with HOS_LOG_LEVEL=debug to list them",
                 rs.unresolved);
    }

    for (int i = 0; i < count; i++) {
        const nso_header *h = &mods[i].nso.hdr;
        uint8_t *image = mods[i].nso.image;
        if (protect(image, h->text.memory_offset, h->text.size, PROT_READ | PROT_EXEC, "text") ||
            protect(image, h->rodata.memory_offset, h->rodata.size, PROT_READ, "rodata") ||
            protect(image, h->data.memory_offset, (size_t)h->data.size + h->bss_size,
                    PROT_READ | PROT_WRITE, "data")) {
            return 1;
        }
    }

    uint64_t entry = (uint64_t)mods[0].nso.image + mods[0].nso.hdr.text.memory_offset;
    if (!has_rtld && count > 1) {
        /* Dependencies first: sdk, then subsdk9..0, i.e. the library list backwards. */
        static uint64_t fns[MOD0_MAX_INIT];
        size_t num_init = 0;
        for (int i = count - 1; i > 0; i--) {
            size_t n = 0;
            r = mod0_init_functions(&link[i], fns + num_init, MOD0_MAX_INIT - num_init, &n);
            if (HOS_FAILED(r)) {
                return die(r, "mod0_init_functions");
            }
            if (n) {
                HOS_LOGI(MOD, "%s: %zu initialiser%s", link[i].name, n, n == 1 ? "" : "s");
            }
            num_init += n;
        }
        if (num_init) {
            uint64_t page = build_init_trampoline(cursor, fns, num_init, entry);
            if (!page || params.num_modules >= HOS_MAX_MODULES) {
                HOS_LOGE(MOD, "cannot run library initialisers");
                return 1;
            }
            hos_cpu_module *cm = &params.modules[params.num_modules++];
            memset(cm, 0, sizeof(*cm));
            cm->image_base = page;
            cm->image_size = INIT_PAGE;
            cm->text_size = INIT_PAGE;
            cm->name = "init";
            entry = page;
        }
    }

    params.program_id = program_id;

    r = hos_cpu_init(&params);
    if (HOS_FAILED(r)) {
        return die(r, "hos_cpu_init");
    }

    HOS_LOGI(MOD,
             "%d module%s, %zu patches (%zu svc, %zu counter), %zu relocations (%zu unresolved, "
             "%zu relr applied, %zu relr left to the module)",
             count, count == 1 ? "" : "s", ps_total.total, ps_total.by_kind[PATCH_SVC_TO_BRK],
             ps_total.by_kind[PATCH_COUNTER_TO_BRK], rs.applied, rs.unresolved, rs.relr_applied,
             rs.deferred);

    hos_cpu_run_main(entry, stack_size);
}
