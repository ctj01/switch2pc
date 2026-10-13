/*
 * ns: the application manager, as a console with nothing installed. Homebrew
 * ask it for the list of installed titles (save managers, launchers) and for
 * storage sizes; the list is empty and the sizes are those of the host
 * filesystem behind the SD card directory. libnx opens the getter service
 * (ns:am2 on 3.0.0+) and asks it for one of the interfaces below.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "ns"

/* ns results: the apps only need a failure to fall back on. */
#define NS_RESULT_NOT_FOUND HOS_RESULT_MAKE(16, 8)

static const hos_ipc_buffer *out_buffer(const hos_ipc_ctx *ctx) {
    for (uint32_t i = 0; i < ctx->num_b; i++) {
        if (ctx->b[i].addr) {
            return &ctx->b[i];
        }
    }
    for (uint32_t i = 0; i < ctx->num_c; i++) {
        if (ctx->c[i].addr) {
            return &ctx->c[i];
        }
    }
    return NULL;
}

/* Total or free bytes of the filesystem behind the SD card (or the root). */
static int64_t space_size(int free_space) {
    const char *root = getenv("HOS_SDMC");
    struct statvfs vs;
    if (!root || !*root || statvfs(root, &vs) != 0) {
        if (statvfs("/", &vs) != 0) {
            return 0;
        }
    }
    uint64_t blocks = free_space ? vs.f_bavail : vs.f_blocks;
    return (int64_t)(blocks * vs.f_frsize);
}

/* ---- IApplicationManagerInterface --------------------------------------------- */

static hos_result app_manager_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* ListApplicationRecord(s32 offset, out buffer) -> s32 count */
    case 1: /* GenerateApplicationRecordCount -> s32 */ {
        int32_t n = 0;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 47:   /* GetTotalSpaceSize(u8 storage) -> s64 */
    case 48: { /* GetFreeSpaceSize(u8 storage) -> s64 */
        int64_t v = space_size(ctx->cmd == 48);
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 400: /* GetApplicationControlData(u8 flag, u64 id, out buffer) -> u32 size */
        return NS_RESULT_NOT_FOUND;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_app_manager_class = {
    .name = "ns:IApplicationManagerInterface",
    .dispatch = app_manager_dispatch,
};

/* ---- IReadOnlyApplicationControlDataInterface ---------------------------------- */

static hos_result control_data_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetApplicationControlData(u8 flag, u64 id, out buffer) -> u32 size */ {
        const hos_ipc_buffer *b = out_buffer(ctx);
        if (b && b->addr) {
            memset((void *)b->addr, 0, b->size);
        }
        return NS_RESULT_NOT_FOUND;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_control_data_class = {
    .name = "ns:IReadOnlyApplicationControlDataInterface",
    .dispatch = control_data_dispatch,
};

/* ---- the getter (ns:am2) --------------------------------------------------------- */

static hos_result getter_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 7989: /* GetReadOnlyApplicationControlDataInterface */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_control_data_class, NULL));
    case 7996: /* GetApplicationManagerInterface */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_app_manager_class, NULL));
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_ns_class = {
    .name = "ns:am2",
    .dispatch = getter_dispatch,
    .destroy = NULL,
};
