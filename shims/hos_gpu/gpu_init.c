/* libhos_gpu: nvdrv and vi. No GPU yet; frames are consumed and optionally dumped. */

#include "common/hos_log.h"
#include "gpu_internal.h"

#define MOD "gpu"

__attribute__((constructor)) static void hos_gpu_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("nvdrv", &hos_nvdrv_class, NULL);
    hos_ipc_register_service("nvdrv:a", &hos_nvdrv_class, NULL);
    hos_ipc_register_service("nvdrv:s", &hos_nvdrv_class, NULL);
    hos_ipc_register_service("nvdrv:t", &hos_nvdrv_class, NULL);
    hos_ipc_register_service("vi:u", &hos_vi_u_class, NULL);
    hos_ipc_register_service("vi:m", &hos_vi_m_class, NULL);
    hos_ipc_register_service("vi:s", &hos_vi_s_class, NULL);
    HOS_LOGD(MOD, "libhos_gpu loaded");
}
