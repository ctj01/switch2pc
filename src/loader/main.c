/*
 * hosrun: load one or more NSO modules and run them natively in this process.
 *
 *   hosrun [options] <module.nso> [more.nso ...]
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
          "       hosrun [options] <exefs directory>\n"
          "  --stack <bytes>   main thread stack size (default 8 MiB)\n"
          "  --no-patch        do not rewrite svc/mrs instructions\n"
          "  --romfs <file>    romfs image for OpenDataStorageByCurrentProcess\n"
          "  --sdmc <dir>      directory served as the SD card\n"
          "  --save <dir>      directory served as save data\n"
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

    r = hos_cpu_init(&params);
    if (HOS_FAILED(r)) {
        return die(r, "hos_cpu_init");
    }

    HOS_LOGI(MOD,
             "%d module%s, %zu patches (%zu svc, %zu counter), %zu relocations (%zu unresolved, "
             "%zu relr left to the module)",
             count, count == 1 ? "" : "s", ps_total.total, ps_total.by_kind[PATCH_SVC_TO_BRK],
             ps_total.by_kind[PATCH_COUNTER_TO_BRK], rs.applied, rs.unresolved, rs.deferred);

    uint64_t entry = (uint64_t)mods[0].nso.image + mods[0].nso.hdr.text.memory_offset;
    hos_cpu_run_main(entry, stack_size);
}
