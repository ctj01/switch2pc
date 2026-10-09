/* libhos_audio: audout null sink today; ALSA later. */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "audio"

extern const hos_service_class hos_audout_class;

__attribute__((constructor)) static void hos_audio_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("audout:u", &hos_audout_class, NULL);
    hos_ipc_register_service("audout:a", &hos_audout_class, NULL);
    HOS_LOGD(MOD, "libhos_audio loaded");
}
