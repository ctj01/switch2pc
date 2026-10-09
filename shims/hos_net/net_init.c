/* libhos_net: nifm today (offline); bsd:u sockets later. */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "net"

extern const hos_service_class hos_nifm_class;

__attribute__((constructor)) static void hos_net_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("nifm:u", &hos_nifm_class, NULL);
    hos_ipc_register_service("nifm:a", &hos_nifm_class, NULL);
    hos_ipc_register_service("nifm:s", &hos_nifm_class, NULL);
    HOS_LOGD(MOD, "libhos_net loaded");
}
