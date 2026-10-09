/*
 * set and set:sys: system settings. Enough for a runtime to learn the
 * firmware version, language and region; everything else says so loudly.
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "set"

/* The firmware we claim to be. libnx switches sm to TIPC at 12.0.0. */
#define FW_MAJOR 11
#define FW_MINOR 0
#define FW_MICRO 1

typedef struct firmware_version {
    uint8_t major, minor, micro, pad0;
    uint8_t revision_major, revision_minor, pad1[2];
    char platform[0x20];
    char version_hash[0x40];
    char display_version[0x18];
    char display_title[0x80];
} firmware_version;

_Static_assert(sizeof(firmware_version) == 0x100, "SetSysFirmwareVersion is 0x100 bytes");

static const uint64_t k_language_en_us = 0x53552d6e65ull; /* "en-US" */

static void write_out_buffer(hos_ipc_ctx *ctx, const void *data, size_t n) {
    const hos_ipc_buffer *b = ctx->num_c ? &ctx->c[0] : (ctx->num_b ? &ctx->b[0] : NULL);
    if (!b || !b->addr) {
        return;
    }
    if (n > b->size) {
        n = b->size;
    }
    memcpy((void *)b->addr, data, n);
}

static hos_result setsys_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 3: /* GetFirmwareVersion */
    case 4: /* GetFirmwareVersion2 */ {
        firmware_version fw;
        memset(&fw, 0, sizeof(fw));
        fw.major = FW_MAJOR;
        fw.minor = FW_MINOR;
        fw.micro = FW_MICRO;
        strcpy(fw.platform, "NX");
        strcpy(fw.version_hash, "switch2pc");
        strcpy(fw.display_version, "11.0.1");
        strcpy(fw.display_title, "NintendoSDK Firmware for NX 11.0.1-1.0");
        write_out_buffer(ctx, &fw, sizeof(fw));
        return HOS_RESULT_SUCCESS;
    }
    case 23: /* GetColorSetId */ {
        uint32_t v = 0; /* light */
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 24: /* SetColorSetId */
        return HOS_RESULT_SUCCESS;
    case 62: /* GetDebugModeFlag */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 77: /* GetDeviceNickName */
        write_out_buffer(ctx, "switch2pc", 10);
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static hos_result set_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetLanguageCode */
        hos_ipc_out_write(ctx, &k_language_en_us, 8);
        return HOS_RESULT_SUCCESS;
    case 1: /* GetAvailableLanguageCodes */
    case 5: /* GetAvailableLanguageCodes2 */ {
        write_out_buffer(ctx, &k_language_en_us, 8);
        uint32_t n = 1;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* MakeLanguageCode(u32 language) */
        hos_ipc_out_write(ctx, &k_language_en_us, 8);
        return HOS_RESULT_SUCCESS;
    case 3: /* GetAvailableLanguageCodeCount */
    case 6: /* GetAvailableLanguageCodeCount2 */ {
        uint32_t n = 1;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* GetRegionCode */ {
        uint32_t v = 1; /* USA */
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 8: /* GetQuestFlag */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_setsys_class = {
    .name = "set:sys",
    .dispatch = setsys_dispatch,
    .destroy = NULL,
};

const hos_service_class hos_set_class = {
    .name = "set",
    .dispatch = set_dispatch,
    .destroy = NULL,
};
