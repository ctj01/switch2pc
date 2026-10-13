/*
 * libhos_net: nifm (offline), bsd:u sockets over host sockets, sfdnsres over the
 * host resolver, ssl (no TLS behind it yet).
 */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "net"

extern const hos_service_class hos_nifm_class;
extern const hos_service_class hos_bsd_class;
extern const hos_service_class hos_sfdnsres_class;
extern const hos_service_class hos_ssl_class;

__attribute__((constructor)) static void hos_net_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("nifm:u", &hos_nifm_class, NULL);
    hos_ipc_register_service("nifm:a", &hos_nifm_class, NULL);
    hos_ipc_register_service("nifm:s", &hos_nifm_class, NULL);
    hos_ipc_register_service("bsd:u", &hos_bsd_class, NULL);
    hos_ipc_register_service("bsd:s", &hos_bsd_class, NULL);
    hos_ipc_register_service("sfdnsres", &hos_sfdnsres_class, NULL);
    hos_ipc_register_service("ssl", &hos_ssl_class, NULL);
    HOS_LOGD(MOD, "libhos_net loaded");
}
