/*
 * nifm: network interface manager. This machine is offline as far as the
 * game can tell: requests never get accepted, there is no IP address, and
 * the connection status query fails the way it does with Wi-Fi off. Real
 * sockets come with bsd:u later.
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "nifm"

#define HOS_MODULE_NIFM           110
#define NIFM_RESULT_NO_CONNECTION HOS_RESULT_MAKE(HOS_MODULE_NIFM, 300)

enum { REQUEST_STATE_INVALID = 1, REQUEST_STATE_ON_HOLD = 2, REQUEST_STATE_ACCEPTED = 3 };

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

static hos_result out_u8(hos_ipc_ctx *ctx, uint8_t v) {
    hos_ipc_out_write(ctx, &v, 1);
    return HOS_RESULT_SUCCESS;
}

static hos_result out_event(hos_ipc_ctx *ctx) {
    void *ev;
    uint32_t h = hos_ipc_event_create(&ev);
    if (!h) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    hos_ipc_event_signal(ev); /* the request has already "completed" (and failed) */
    ctx->copy_handles[ctx->num_copy++] = h;
    return HOS_RESULT_SUCCESS;
}

/* ---- IRequest ----------------------------------------------------------------------- */

static hos_result request_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetRequestState -> u32 */ {
        uint32_t v = REQUEST_STATE_INVALID;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* GetResult */
        return NIFM_RESULT_NO_CONNECTION;
    case 2: /* GetSystemEventReadableHandles -> 2 events */ {
        hos_result r = out_event(ctx);
        return HOS_FAILED(r) ? r : out_event(ctx);
    }
    case 3:  /* Cancel */
    case 4:  /* Submit */
    case 5:  /* SetRequirement */
    case 6:  /* SetRequirementPreset */
    case 8:  /* SetPriority */
    case 9:  /* SetNetworkProfileId */
    case 10: /* SetRejectable */
    case 11: /* SetConnectionConfirmationOption */
    case 12: /* SetPersistent */
    case 13: /* SetInstant */
    case 14: /* SetSustainable */
    case 15: /* SetRawPriority */
    case 16: /* SetGreedy */
    case 17: /* SetSharable */
    case 18: /* SetRequirementByRevision */
        return HOS_RESULT_SUCCESS;
    case 21: /* GetAppletInfo */
        return NIFM_RESULT_NO_CONNECTION;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_request_class = {
    .name = "nifm:IRequest",
    .dispatch = request_dispatch,
};

/* ---- IGeneralService ------------------------------------------------------------------ */

static hos_result general_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 1: /* GetClientId -> buffer u32 */ {
        const hos_ipc_buffer *b = out_buffer(ctx);
        if (b && b->addr && b->size >= 4) {
            uint32_t id = 1;
            memcpy((void *)b->addr, &id, 4);
        }
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* CreateRequest(u32 requirement) */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_request_class, NULL));
    case 5:  /* GetCurrentNetworkProfile */
    case 12: /* GetCurrentIpAddress */
    case 15: /* GetCurrentIpConfigInfo */
    case 18: /* GetInternetConnectionStatus */
        return NIFM_RESULT_NO_CONNECTION;
    case 6: /* EnumerateNetworkInterfaces -> u32 count */
    case 7: /* EnumerateNetworkProfiles */ {
        uint32_t n = 0;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 21: /* IsAnyInternetRequestAccepted(client id) -> u8 */
    case 22: /* IsAnyForegroundRequestAccepted */
    case 24: /* IsWirelessCommunicationEnabled */
    case 25: /* IsEthernetCommunicationEnabled? */
    case 27: /* IsAnyInternetRequestAccepted (newer) */
        return out_u8(ctx, 0);
    case 23: /* PutToSleep */
    case 26: /* WakeUp? */
    case 30: /* SetWirelessCommunicationEnabled */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_general_class = {
    .name = "nifm:IGeneralService",
    .dispatch = general_dispatch,
};

/* ---- nifm:u / nifm:a / nifm:s ------------------------------------------------------- */

static hos_result nifm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 4: /* CreateGeneralService(pid) */
    case 5: /* CreateGeneralServiceOld */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_general_class, NULL));
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_nifm_class = {
    .name = "nifm",
    .dispatch = nifm_dispatch,
    .destroy = NULL,
};
