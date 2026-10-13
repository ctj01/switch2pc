/*
 * acc:u0 / acc:u1 / acc:su: the account service. One local user, always present and
 * already "opened", so homebrew that asks for a user (save data, profile
 * pickers) gets one without a selection dialog. The nickname comes from
 * HOS_USER_NAME, then $USER, then "switch2pc"; the profile image is empty.
 */

#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "account"

#define PROFILE_BASE_SIZE 0x38
#define USER_DATA_SIZE    0x80
#define NICKNAME_SIZE     0x20

/* AccountUid: two u64, never both zero for a real user. */
static const uint64_t k_uid[2] = {0x0000000000000001ull, 0x5332504353325043ull};

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

static void nickname(char out[NICKNAME_SIZE]) {
    const char *n = getenv("HOS_USER_NAME");
    if (!n || !*n) {
        n = getenv("USER");
    }
    if (!n || !*n) {
        n = "switch2pc";
    }
    memset(out, 0, NICKNAME_SIZE);
    strncpy(out, n, NICKNAME_SIZE - 1);
}

static int is_our_uid(const hos_ipc_ctx *ctx) {
    uint64_t uid[2] = {0, 0};
    hos_ipc_in_read(ctx, 0, uid, sizeof(uid));
    return uid[0] == k_uid[0] && uid[1] == k_uid[1];
}

static hos_result out_uid(hos_ipc_ctx *ctx) {
    hos_ipc_out_write(ctx, k_uid, sizeof(k_uid));
    return HOS_RESULT_SUCCESS;
}

/* ---- IProfile ------------------------------------------------------------------- */

static hos_result profile_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* Get -> ProfileBase, buffer UserData */
    case 1: /* GetBase -> ProfileBase */ {
        uint8_t base[PROFILE_BASE_SIZE];
        memset(base, 0, sizeof(base));
        memcpy(base, k_uid, sizeof(k_uid));
        uint64_t edited = 1600000000ull; /* last edit timestamp, POSIX seconds */
        memcpy(base + 0x10, &edited, 8);
        nickname((char *)base + 0x18);
        hos_ipc_out_write(ctx, base, sizeof(base));
        const hos_ipc_buffer *b = out_buffer(ctx);
        if (ctx->cmd == 0 && b && b->addr) {
            memset((void *)b->addr, 0, b->size < USER_DATA_SIZE ? b->size : USER_DATA_SIZE);
        }
        return HOS_RESULT_SUCCESS;
    }
    case 10: /* GetImageSize -> u32 */ {
        uint32_t size = 0;
        hos_ipc_out_write(ctx, &size, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 11: /* LoadImage -> buffer, u32 size */ {
        uint32_t size = 0;
        hos_ipc_out_write(ctx, &size, 4);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_profile_class = {
    .name = "acc:IProfile",
    .dispatch = profile_dispatch,
};

/* ---- IAccountServiceForApplication ---------------------------------------------- */

static hos_result account_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetUserCount -> s32 */ {
        int32_t n = 1;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* GetUserExistence(uid) -> bool */ {
        uint8_t v = (uint8_t)is_our_uid(ctx);
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* ListAllUsers -> buffer of uid */
    case 3: /* ListOpenUsers -> buffer of uid */ {
        const hos_ipc_buffer *b = out_buffer(ctx);
        if (b && b->addr) {
            memset((void *)b->addr, 0, b->size);
            if (b->size >= sizeof(k_uid)) {
                memcpy((void *)b->addr, k_uid, sizeof(k_uid));
            }
        }
        return HOS_RESULT_SUCCESS;
    }
    case 4:  /* GetLastOpenedUser -> uid */
    case 51: /* TrySelectUserWithoutInteraction(bool) -> uid */
        return out_uid(ctx);
    case 5: /* GetProfile(uid) -> IProfile */
        if (!is_our_uid(ctx)) {
            return HOS_RESULT_MAKE(124, 100); /* account: user not found */
        }
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_profile_class, NULL));
    case 50: /* IsUserRegistrationRequestPermitted -> bool */
    case 150: /* IsUserAccountSwitchLocked -> bool */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 100: /* InitializeApplicationInfoV0 */
    case 140: /* InitializeApplicationInfo */
    case 110: /* StoreSaveDataThumbnail */
    case 111: /* ClearSaveDataThumbnail */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_account_class = {
    .name = "acc:u0",
    .dispatch = account_dispatch,
    .destroy = NULL,
};
