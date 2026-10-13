/*
 * pdm:qry: play data manager queries. This console has never played anything:
 * every statistic is zero and every event list is empty, which is what a save
 * manager or an activity log expects from a fresh system.
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "pdm"

#define PLAY_STATISTICS_SIZE 0x48 /* PdmPlayStatistics, [10.0.0+] layout */

static hos_result pdm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0:  /* QueryAppletEvent(s32 index, out buffer) -> s32 total */
    case 7:  /* QueryLastPlayTime(in ids, out buffer) -> s32 total */
    case 14: /* QueryRecentlyPlayedApplication(uid, out buffer) -> s32 total */
    case 17: /* QueryLastPlayTime [10.0.0+] -> s32 total */ {
        int32_t total = 0;
        hos_ipc_out_write(ctx, &total, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4:   /* QueryPlayStatisticsByApplicationId(u64 id) -> PdmPlayStatistics */
    case 5: { /* QueryPlayStatisticsByApplicationIdAndUserAccountId(u64 id, uid) -> same */
        uint8_t stats[PLAY_STATISTICS_SIZE];
        memset(stats, 0, sizeof(stats));
        hos_ipc_in_read(ctx, 0, stats, 8); /* program_id is echoed back */
        hos_ipc_out_write(ctx, stats, sizeof(stats));
        return HOS_RESULT_SUCCESS;
    }
    case 9: /* GetAvailablePlayEventRange -> s32 total, s32 start, s32 end */ {
        int32_t range[3] = {0, 0, 0};
        hos_ipc_out_write(ctx, range, sizeof(range));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_pdm_class = {
    .name = "pdm:qry",
    .dispatch = pdm_dispatch,
    .destroy = NULL,
};
