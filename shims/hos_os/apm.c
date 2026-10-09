/*
 * apm: performance manager. Applications open a session at start-up and
 * occasionally ask for or set a performance configuration. We are always in
 * handheld mode at the default configuration.
 */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "apm"

#define PERFORMANCE_CONFIG_DEFAULT 0x00020004u /* handheld 1020 MHz */

static hos_result out_u32(hos_ipc_ctx *ctx, uint32_t v) {
    hos_ipc_out_write(ctx, &v, 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result session_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* SetPerformanceConfiguration(mode, config) */
    case 2: /* SetCpuOverclockEnabled */
        return HOS_RESULT_SUCCESS;
    case 1: /* GetPerformanceConfiguration(mode) -> u32 */
        return out_u32(ctx, PERFORMANCE_CONFIG_DEFAULT);
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_session_class = {
    .name = "apm:ISession",
    .dispatch = session_dispatch,
};

static hos_result apm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* OpenSession */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_session_class, NULL));
    case 1: /* GetPerformanceMode -> u32 (0 = handheld) */
        return out_u32(ctx, 0);
    case 6: /* IsCpuOverclockEnabled -> u8 */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_apm_class = {
    .name = "apm",
    .dispatch = apm_dispatch,
    .destroy = NULL,
};
