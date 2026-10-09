/*
 * sm: the service manager port. The one service that lives in libhos_cpu,
 * because every other service is reached through it.
 */

#include <string.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD "sm"

#define HOS_MODULE_SM                  21
#define SM_RESULT_INVALID_CLIENT       HOS_RESULT_MAKE(HOS_MODULE_SM, 3)
#define SM_RESULT_INVALID_SERVICE_NAME HOS_RESULT_MAKE(HOS_MODULE_SM, 7)
#define SM_RESULT_NOT_REGISTERED       HOS_RESULT_MAKE(HOS_MODULE_SM, 8)
#define SM_RESULT_NOT_ALLOWED          HOS_RESULT_MAKE(HOS_MODULE_SM, 9)

enum {
    SM_REGISTER_CLIENT = 0,
    SM_GET_SERVICE_HANDLE = 1,
    SM_REGISTER_SERVICE = 2,
    SM_UNREGISTER_SERVICE = 3,
};

static void name_from_u64(uint64_t v, char out[9]) {
    memcpy(out, &v, 8);
    out[8] = '\0';
}

static hos_result sm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case SM_REGISTER_CLIENT:
        return HOS_RESULT_SUCCESS;
    case SM_GET_SERVICE_HANDLE: {
        uint64_t raw = 0;
        if (!hos_ipc_in_read(ctx, 0, &raw, 8)) {
            return SM_RESULT_INVALID_SERVICE_NAME;
        }
        char name[9];
        name_from_u64(raw, name);
        if (name[0] == '\0') {
            return SM_RESULT_INVALID_SERVICE_NAME;
        }
        hos_object *svc = hos_ipc_service_instance(name);
        if (!svc) {
            HOS_LOGW(MOD, "GetService(%s): not registered", name);
            return SM_RESULT_NOT_REGISTERED;
        }
        uint32_t h = hos_ipc_session_create(svc);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        ctx->move_handles[ctx->num_move++] = h;
        HOS_LOGD(MOD, "GetService(%s) -> %#x", name, h);
        return HOS_RESULT_SUCCESS;
    }
    case SM_REGISTER_SERVICE:
    case SM_UNREGISTER_SERVICE:
        HOS_LOGW(MOD, "applications may not register services");
        return SM_RESULT_NOT_ALLOWED;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_sm_class = {
    .name = "sm:",
    .dispatch = sm_dispatch,
    .destroy = NULL,
};

void hos_sm_init(void) {
    hos_ipc_register_service("sm:", &g_sm_class, NULL);
}
