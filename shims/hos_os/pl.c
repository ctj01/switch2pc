/*
 * pl:u / pl:s: the shared font. On the console the service decrypts
 * Nintendo's fonts into a 0x1100000-byte shared memory block and tells the
 * application where each one starts. Here the block holds one TrueType font
 * from the host, served for every font type: HOS_FONT, or the first of a few
 * well-known system fonts (DejaVu Sans, Liberation Sans, Noto Sans). Without
 * any font the block is empty and the sizes are zero, so the application's
 * font library reports an error instead of the process stopping at the
 * missing service.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "pl"

#define SHARED_FONT_MEM_SIZE 0x1100000 /* what libnx maps (SHAREDMEMFONT_SIZE) */
#define FONT_TYPES           6         /* PlSharedFontType_Total */

static uint32_t g_shmem_handle;
static uint8_t *g_shmem_view;
static uint32_t g_font_size;
static int g_loaded;

static const char *const k_font_paths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
};

static int load_font_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(g_shmem_view, 1, SHARED_FONT_MEM_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n == 0 || more) {
        HOS_LOGW(MOD, "%s: %s", path, n == 0 ? "cannot read" : "larger than the shared memory");
        return 0;
    }
    g_font_size = (uint32_t)n;
    HOS_LOGI(MOD, "shared font: %s (%u bytes)", path, g_font_size);
    return 1;
}

/* Create the block and fill it on first use. */
static int ensure_loaded(void) {
    if (g_loaded) {
        return g_shmem_handle != 0;
    }
    g_loaded = 1;
    void *view = NULL;
    g_shmem_handle = hos_shmem_create(SHARED_FONT_MEM_SIZE, &view);
    if (!g_shmem_handle) {
        HOS_LOGE(MOD, "cannot create the shared font memory");
        return 0;
    }
    g_shmem_view = view;
    const char *env = getenv("HOS_FONT");
    if (env && *env) {
        if (!load_font_file(env)) {
            HOS_LOGW(MOD, "HOS_FONT=%s: not usable", env);
        }
    } else {
        for (size_t i = 0; i < sizeof(k_font_paths) / sizeof(k_font_paths[0]); i++) {
            if (load_font_file(k_font_paths[i])) {
                break;
            }
        }
    }
    if (!g_font_size) {
        HOS_LOGW(MOD, "no TrueType font found (set HOS_FONT); the shared font is empty");
    }
    return 1;
}

static hos_result pl_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* RequestLoad(u32 type) */
        return ensure_loaded() ? HOS_RESULT_SUCCESS : HOS_KERNEL_OUT_OF_RESOURCE;
    case 1: /* GetLoadState(u32 type) -> u32 (1 = loaded) */
    case 2: /* GetSize(u32 type) -> u32 */
    case 3: /* GetSharedMemoryAddressOffset(u32 type) -> u32 */ {
        if (!ensure_loaded()) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        /* Without a host font nothing is loaded: say so, and fail the calls that would
         * hand out a font, so the app learns it there rather than inside its font library. */
        if (ctx->cmd != 1 && !g_font_size) {
            HOS_LOGW(MOD, "no TrueType font on the host (set HOS_FONT); no shared font to give");
            return S2PC_ERR_NOT_FOUND;
        }
        uint32_t v = ctx->cmd == 1 ? (g_font_size ? 1 : 0) : ctx->cmd == 2 ? g_font_size : 0;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* GetSharedMemoryNativeHandle -> copy handle */ {
        if (!ensure_loaded()) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        uint32_t h = hos_ipc_copy_handle(g_shmem_handle);
        if (!h) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* GetSharedFontInOrderOfPriority(u64 language) -> u8 loaded, s32 count;
             * out buffers: types, offsets, sizes (u32 each) */
    {
        if (!ensure_loaded()) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        uint32_t count = g_font_size ? FONT_TYPES : 0;
        for (uint32_t i = 0; i < ctx->num_b && i < 3; i++) {
            uint32_t *out = (uint32_t *)ctx->b[i].addr;
            uint64_t room = ctx->b[i].size / 4;
            for (uint32_t k = 0; out && k < count && k < room; k++) {
                out[k] = i == 0 ? k : i == 1 ? 0 : g_font_size;
            }
        }
        struct {
            uint8_t loaded;
            uint8_t pad[3];
            int32_t count;
        } out = {1, {0, 0, 0}, (int32_t)count};
        hos_ipc_out_write(ctx, &out, sizeof(out));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_pl_class = {
    .name = "pl:u",
    .dispatch = pl_dispatch,
    .destroy = NULL,
};
