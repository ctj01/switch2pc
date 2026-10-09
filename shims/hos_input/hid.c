/*
 * hid: human interface devices. The runtime maps a 0x40000-byte shared memory
 * and polls it for controller state; we hand out that memory zeroed, which
 * reads as "no controllers, nothing pressed". Every activate/configure
 * command succeeds. Real input lands here later (evdev on Linux).
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD            "hid"
#define HID_SHMEM_SIZE 0x40000

#include "input_state.h"

static uint32_t g_shmem_handle;
static void *g_shmem_view;

static hos_result out_u32(hos_ipc_ctx *ctx, uint32_t v) {
    hos_ipc_out_write(ctx, &v, 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result out_u64(hos_ipc_ctx *ctx, uint64_t v) {
    hos_ipc_out_write(ctx, &v, 8);
    return HOS_RESULT_SUCCESS;
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
    ctx->copy_handles[ctx->num_copy++] = h;
    return HOS_RESULT_SUCCESS;
}

/* ---- IAppletResource ----------------------------------------------------------- */

static hos_result applet_resource_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    if (ctx->cmd != 0) { /* GetSharedMemoryHandle */
        return hos_ipc_unimplemented(obj, ctx);
    }
    if (!g_shmem_handle) {
        g_shmem_handle = hos_shmem_create(HID_SHMEM_SIZE, &g_shmem_view);
        if (!g_shmem_handle) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        hos_input_start(g_shmem_view);
    }
    uint32_t h = hos_ipc_copy_handle(g_shmem_handle);
    if (!h) {
        return HOS_KERNEL_OUT_OF_HANDLES;
    }
    ctx->copy_handles[ctx->num_copy++] = h;
    return HOS_RESULT_SUCCESS;
}

static const hos_service_class g_applet_resource_class = {
    .name = "hid:IAppletResource",
    .dispatch = applet_resource_dispatch,
};

/* ---- IActiveVibrationDeviceList ------------------------------------------------- */

static hos_result vibration_list_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    (void)obj;
    (void)ctx;
    return HOS_RESULT_SUCCESS; /* ActivateVibrationDevice */
}

static const hos_service_class g_vibration_list_class = {
    .name = "hid:IActiveVibrationDeviceList",
    .dispatch = vibration_list_dispatch,
};

/* ---- hid ------------------------------------------------------------------------- */

static hos_result hid_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* CreateAppletResource */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_applet_resource_class, NULL));
    case 40:  /* AcquireXpadIdEventHandle */
    case 106: /* AcquireNpadStyleSetUpdateEventHandle */
    case 306: /* AcquireConsoleSixAxisSensorEventHandle? (old) */
        return out_event(ctx);
    case 55: /* GetXpadIds */
    case 59: /* GetJoyXpadIds */
        return out_u64(ctx, 0);
    case 68:   /* IsSixAxisSensorFusionEnabled */
    case 82:   /* IsSixAxisSensorAtRest */
    case 131:  /* IsUnintendedHomeButtonInputProtectionEnabled */
    case 400:  /* IsUsbFullKeyControllerEnabled */
    case 1003: /* IsFirmwareUpdateNeededForNotification */
        return out_u8(ctx, 0);
    case 101: /* GetSupportedNpadStyleSet */
        return out_u32(ctx, 0x3F);
    case 108:  /* GetPlayerLedPattern */
    case 121:  /* GetNpadJoyHoldType */
    case 129:  /* GetNpadHandheldActivationMode */
    case 1001: /* GetNpadCommunicationMode */
        return out_u64(ctx, 0);
    case 200:                   /* GetVibrationDeviceInfo */
        out_u32(ctx, 1);        /* type: LinearResonantActuator */
        return out_u32(ctx, 1); /* position: left */
    case 202: /* GetActualVibrationValue */ {
        uint8_t v[16] = {0};
        hos_ipc_out_write(ctx, v, 16);
        return HOS_RESULT_SUCCESS;
    }
    case 203: /* CreateActiveVibrationDeviceList */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_vibration_list_class, NULL));
    case 205: /* IsVibrationPermitted */
    case 211: /* IsVibrationDeviceMounted */
        return out_u8(ctx, 1);
    case 208: /* GetActualVibrationGcErmCommand */
        return out_u64(ctx, 0);
    default:
        break;
    }
    /* Activate*, Set*, Start*, Stop*, Permit*, Send*: nothing to do without hardware. */
    if (ctx->cmd <= 1003) {
        HOS_LOGD(MOD, "command %u accepted (no-op)", ctx->cmd);
        return HOS_RESULT_SUCCESS;
    }
    return hos_ipc_unimplemented(obj, ctx);
}

const hos_service_class hos_hid_class = {
    .name = "hid",
    .dispatch = hid_dispatch,
    .destroy = NULL,
};
