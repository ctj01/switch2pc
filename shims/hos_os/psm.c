/*
 * psm: power state. A full battery on the official charger, never changing,
 * so status bars and "low battery" checks in homebrew stay quiet.
 */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "psm"

#define CHARGER_OFFICIAL 1
#define VOLTAGE_GOOD     3 /* PsmBatteryVoltageState_Good */

static hos_result out_u32(hos_ipc_ctx *ctx, uint32_t v) {
    hos_ipc_out_write(ctx, &v, 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result session_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* BindStateChangeEvent -> event (never signalled: nothing changes) */ {
        void *ev = NULL;
        uint32_t h = hos_ipc_event_create(&ev);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* UnbindStateChangeEvent */
    case 2: /* SetChargerTypeChangeEventEnabled */
    case 3: /* SetPowerSupplyChangeEventEnabled */
    case 4: /* SetBatteryVoltageStateChangeEventEnabled */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_session_class = {
    .name = "psm:IPsmSession",
    .dispatch = session_dispatch,
};

static hos_result psm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetBatteryChargePercentage -> u32 */
        return out_u32(ctx, 100);
    case 1: /* GetChargerType -> u32 */
        return out_u32(ctx, CHARGER_OFFICIAL);
    case 7: /* OpenSession -> IPsmSession */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_session_class, NULL));
    case 12: /* GetBatteryVoltageState -> u32 */
        return out_u32(ctx, VOLTAGE_GOOD);
    case 13: /* GetRawBatteryChargePercentage -> f64 */ {
        double v = 100.0;
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 14: /* IsEnoughPowerSupplied -> bool */ {
        uint8_t v = 1;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 15: /* GetBatteryAgePercentage -> f64 */ {
        double v = 100.0;
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_psm_class = {
    .name = "psm",
    .dispatch = psm_dispatch,
    .destroy = NULL,
};
