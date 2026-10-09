/*
 * appletOE / appletAE: the applet manager, as seen by an application that is
 * always in the foreground on a handheld unit. Setters succeed, getters return
 * the quiet default, events are created on demand. Anything with real
 * behaviour (library applets, storages) is reported as unimplemented.
 */

#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "applet"

#define HOS_MODULE_APPLET        128
#define APPLET_RESULT_NO_DATA    HOS_RESULT_MAKE(HOS_MODULE_APPLET, 2)
#define APPLET_RESULT_NO_MESSAGE HOS_RESULT_MAKE(HOS_MODULE_APPLET, 3)

#define APPLET_MESSAGE_EXIT_REQUEST        4
#define APPLET_MESSAGE_FOCUS_STATE_CHANGED 15

static const uint64_t k_language_en_us = 0x53552d6e65ull;

typedef struct common_state {
    void *message_event;
    uint32_t message_event_handle;
    int focus_message_pending;
    int exit_pending;
} common_state;

/* A freshly started application always finds a FocusStateChanged message waiting. */
static common_state g_common = {.focus_message_pending = 1};

static void applet_request_exit(void) {
    HOS_LOGI(MOD, "exit requested; delivering AppletMessage_ExitRequest");
    g_common.exit_pending = 1;
    if (g_common.message_event) {
        hos_ipc_event_signal(g_common.message_event);
    }
}

__attribute__((constructor)) static void applet_ctor(void) {
    hos_applet_set_exit_handler(applet_request_exit);
}

static hos_result out_u8(hos_ipc_ctx *ctx, uint8_t v) {
    hos_ipc_out_write(ctx, &v, 1);
    return HOS_RESULT_SUCCESS;
}

static hos_result out_u32(hos_ipc_ctx *ctx, uint32_t v) {
    hos_ipc_out_write(ctx, &v, 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result out_u64(hos_ipc_ctx *ctx, uint64_t v) {
    hos_ipc_out_write(ctx, &v, 8);
    return HOS_RESULT_SUCCESS;
}

static hos_result out_event(hos_ipc_ctx *ctx, int signaled) {
    void *ev;
    uint32_t h = hos_ipc_event_create(&ev);
    if (!h) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    if (signaled) {
        hos_ipc_event_signal(ev);
    }
    ctx->copy_handles[ctx->num_copy++] = h;
    return HOS_RESULT_SUCCESS;
}

static int in_list(uint32_t cmd, const uint32_t *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (list[i] == cmd) {
            return 1;
        }
    }
    return 0;
}

#define OK_LIST(name, ...)                                                                         \
    static const uint32_t name[] = {__VA_ARGS__};                                                  \
    static int name##_has(uint32_t cmd) {                                                          \
        return in_list(cmd, name, sizeof(name) / sizeof(name[0]));                                 \
    }

/* ---- ICommonStateGetter ------------------------------------------------------------- */

static hos_result common_state_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetEventHandle */ {
        if (!g_common.message_event_handle) {
            g_common.message_event_handle = hos_ipc_event_create(&g_common.message_event);
            if (!g_common.message_event_handle) {
                return HOS_KERNEL_OUT_OF_RESOURCE;
            }
            if (g_common.focus_message_pending || g_common.exit_pending) {
                hos_ipc_event_signal(g_common.message_event);
            }
        }
        uint32_t h = hos_ipc_copy_handle(g_common.message_event_handle);
        if (!h) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* ReceiveMessage */
        if (g_common.focus_message_pending) {
            g_common.focus_message_pending = 0;
            if (g_common.message_event) {
                hos_ipc_event_clear(g_common.message_event);
            }
            return out_u32(ctx, APPLET_MESSAGE_FOCUS_STATE_CHANGED);
        }
        if (g_common.exit_pending) {
            g_common.exit_pending = 0;
            if (g_common.message_event) {
                hos_ipc_event_clear(g_common.message_event);
            }
            return out_u32(ctx, APPLET_MESSAGE_EXIT_REQUEST);
        }
        return APPLET_RESULT_NO_MESSAGE;
    case 5:                    /* GetOperationMode */
        return out_u8(ctx, 0); /* handheld */
    case 6:                    /* GetPerformanceMode */
        return out_u32(ctx, 0);
    case 8: /* GetBootMode */
        return out_u8(ctx, 0);
    case 9:                    /* GetCurrentFocusState */
        return out_u8(ctx, 1); /* in focus */
    case 10:                   /* RequestToAcquireSleepLock */
    case 11:                   /* ReleaseSleepLock */
    case 12:                   /* ReleaseSleepLockTransiently */
    case 66:                   /* SetCpuBoostMode */
    case 80:                   /* PerformSystemButtonPressingIfInFocus */
    case 90:                   /* SetPerformanceConfigurationChangedNotification */
    case 120:                  /* SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled */
        return HOS_RESULT_SUCCESS;
    case 13: /* GetAcquiredSleepLockEvent */
    case 61: /* GetDefaultDisplayResolutionChangeEvent */
    case 92: /* GetDefaultDisplayResolutionChangeEvent (older) */
        return out_event(ctx, 0);
    case 50: /* IsVrModeEnabled */
    case 51: /* SetVrModeEnabled */
    case 52: /* SetLcdBacklighOffEnabled */
    case 53: /* BeginVrModeEx */
    case 54: /* EndVrModeEx */
    case 55: /* IsInControllerFirmwareUpdateSection */
        return (ctx->cmd == 50 || ctx->cmd == 55) ? out_u8(ctx, 0) : HOS_RESULT_SUCCESS;
    case 60: /* GetDefaultDisplayResolution */
        out_u32(ctx, 1280);
        return out_u32(ctx, 720);
    case 91:                             /* GetCurrentPerformanceConfiguration */
        return out_u32(ctx, 0x00020004); /* handheld 1020 MHz */
    case 200:                            /* GetOperationModeSystemInfo */
        return out_u32(ctx, 0);
    case 300: /* GetSettingsPlatformRegion */
        return out_u8(ctx, 1);
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_common_state_class = {
    .name = "applet:ICommonStateGetter",
    .dispatch = common_state_dispatch,
};

/* ---- ISelfController ----------------------------------------------------------------- */

OK_LIST(self_ok, 1, 2, 3, 4, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 50, 51, 60, 61, 62, 64,
        65, 67, 68, 70, 80, 100, 110, 120, 130)

static hos_result self_controller_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    if (self_ok_has(ctx->cmd)) {
        return HOS_RESULT_SUCCESS;
    }
    switch (ctx->cmd) {
    case 0: /* Exit */
        HOS_LOGI(MOD, "ISelfController::Exit");
        exit(0);
    case 9: /* GetLibraryAppletLaunchableEvent */
        return out_event(ctx, 1);
    case 40: /* CreateManagedDisplayLayer */
    case 44: /* CreateManagedDisplaySeparableLayer */
        out_u64(ctx, 1);
        return (ctx->cmd == 44) ? out_u64(ctx, 2) : HOS_RESULT_SUCCESS;
    case 41: /* IsSystemBufferSharingEnabled */
        return HOS_KERNEL_NOT_IMPLEMENTED;
    case 63: /* GetIdleTimeDetectionExtension */
        return out_u32(ctx, 0);
    case 69: /* IsAutoSleepDisabled */
        return out_u8(ctx, 0);
    case 71:                             /* GetCurrentIlluminance */
        return out_u32(ctx, 0x447A0000); /* 1000.0f lux */
    case 72:                             /* IsIlluminanceAvailable */
        return out_u8(ctx, 0);
    case 90: /* GetAccumulatedSuspendedTickValue */
        return out_u64(ctx, 0);
    case 91: /* GetAccumulatedSuspendedTickChangedEvent */
        return out_event(ctx, 0);
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_self_controller_class = {
    .name = "applet:ISelfController",
    .dispatch = self_controller_dispatch,
};

/* ---- IWindowController ---------------------------------------------------------------- */

static hos_result window_controller_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 1: /* GetAppletResourceUserId */
    case 2: /* GetAppletResourceUserIdOfCallerApplet */
        return out_u64(ctx, 1);
    case 10: /* AcquireForegroundRights */
    case 11: /* ReleaseForegroundRights */
    case 12: /* RejectToChangeIntoBackground */
    case 20: /* SetAppletWindowVisibility */
    case 21: /* SetAppletGpuTimeSlice */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_window_controller_class = {
    .name = "applet:IWindowController",
    .dispatch = window_controller_dispatch,
};

/* ---- IAudioController ------------------------------------------------------------------ */

static hos_result audio_controller_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* SetExpectedMasterVolume */
    case 3: /* ChangeMainAppletMasterVolume */
    case 4: /* SetTransparentVolumeRate */
        return HOS_RESULT_SUCCESS;
    case 1:                              /* GetMainAppletExpectedMasterVolume */
    case 2:                              /* GetLibraryAppletExpectedMasterVolume */
        return out_u32(ctx, 0x3F800000); /* 1.0f */
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_audio_controller_class = {
    .name = "applet:IAudioController",
    .dispatch = audio_controller_dispatch,
};

/* ---- IDisplayController, ILibraryAppletCreator, IDebugFunctions ---------------------- */

static hos_result stub_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    return hos_ipc_unimplemented(obj, ctx);
}

static const hos_service_class g_display_controller_class = {
    .name = "applet:IDisplayController",
    .dispatch = stub_dispatch,
};

static const hos_service_class g_library_applet_creator_class = {
    .name = "applet:ILibraryAppletCreator",
    .dispatch = stub_dispatch,
};

static const hos_service_class g_debug_functions_class = {
    .name = "applet:IDebugFunctions",
    .dispatch = stub_dispatch,
};

/* ---- IApplicationFunctions ---------------------------------------------------------------- */

OK_LIST(app_ok, 22, 25, 30, 31, 32, 33, 60, 66, 67, 68, 90, 100, 101, 102, 120, 121, 122, 124, 170,
        500)

static hos_result application_functions_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    if (app_ok_has(ctx->cmd)) {
        return HOS_RESULT_SUCCESS;
    }
    switch (ctx->cmd) {
    case 1: /* PopLaunchParameter */
        return APPLET_RESULT_NO_DATA;
    case 20: /* EnsureSaveData */
        return out_u64(ctx, 0);
    case 21: /* GetDesiredLanguage */
        return out_u64(ctx, k_language_en_us);
    case 23: /* GetDisplayVersion */ {
        char v[16];
        memset(v, 0, sizeof(v));
        strcpy(v, "1.0.0");
        hos_ipc_out_write(ctx, v, sizeof(v));
        return HOS_RESULT_SUCCESS;
    }
    case 26: /* GetSaveDataSize */
        out_u64(ctx, 0);
        return out_u64(ctx, 0);
    case 40: /* NotifyRunning */
        return out_u8(ctx, 0);
    case 50: /* GetPseudoDeviceId */ {
        uint8_t id[16] = {0x53, 0x32, 0x50, 0x43, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        hos_ipc_out_write(ctx, id, 16);
        return HOS_RESULT_SUCCESS;
    }
    case 65: /* IsGamePlayRecordingSupported */
        return out_u8(ctx, 0);
    case 110: /* QueryApplicationPlayStatistics */
    case 111: /* QueryApplicationPlayStatisticsByUid */
        return out_u32(ctx, 0);
    case 123: /* GetPreviousProgramIndex */
        return out_u32(ctx, 0xFFFFFFFFu);
    case 130: /* GetGpuErrorDetectedSystemEvent */
    case 140: /* GetFriendInvitationStorageChannelEvent */
    case 150: /* GetNotificationStorageChannelEvent */
    case 160: /* GetHealthWarningDisappearedSystemEvent */
        return out_event(ctx, 0);
    case 141: /* TryPopFromFriendInvitationStorageChannel */
    case 151: /* TryPopFromNotificationStorageChannel */
        return APPLET_RESULT_NO_DATA;
    case 180: /* GetLaunchRequiredVersion */
        return out_u32(ctx, 0);
    case 200: /* GetLastApplicationExitReason */
        return out_u32(ctx, 0);
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_application_functions_class = {
    .name = "applet:IApplicationFunctions",
    .dispatch = application_functions_dispatch,
};

/* ---- IApplicationProxy ------------------------------------------------------------------- */

static hos_result application_proxy_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    const hos_service_class *cls = NULL;
    switch (ctx->cmd) {
    case 0:
        cls = &g_common_state_class;
        break;
    case 1:
        cls = &g_self_controller_class;
        break;
    case 2:
        cls = &g_window_controller_class;
        break;
    case 3:
        cls = &g_audio_controller_class;
        break;
    case 4:
        cls = &g_display_controller_class;
        break;
    case 11:
        cls = &g_library_applet_creator_class;
        break;
    case 20:
        cls = &g_application_functions_class;
        break;
    case 1000:
        cls = &g_debug_functions_class;
        break;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
    return hos_ipc_return_object(ctx, hos_ipc_object_new(cls, NULL));
}

static const hos_service_class g_application_proxy_class = {
    .name = "applet:IApplicationProxy",
    .dispatch = application_proxy_dispatch,
};

/* ---- appletOE / appletAE ----------------------------------------------------------------- */

static hos_result applet_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0:   /* appletOE OpenApplicationProxy */
    case 100: /* appletAE OpenSystemAppletProxy */
    case 200: /* appletAE OpenLibraryAppletProxyOld */
    case 201: /* appletAE OpenLibraryAppletProxy */
    case 300: /* appletAE OpenOverlayAppletProxy */
    case 350: /* appletAE OpenSystemApplicationProxy */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_application_proxy_class, NULL));
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_applet_oe_class = {
    .name = "appletOE",
    .dispatch = applet_dispatch,
    .destroy = NULL,
};

const hos_service_class hos_applet_ae_class = {
    .name = "appletAE",
    .dispatch = applet_dispatch,
    .destroy = NULL,
};
