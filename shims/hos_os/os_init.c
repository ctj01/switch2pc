/*
 * libhos_os: OS-level services. Registers its services with libhos_cpu when
 * the dynamic linker loads it; hosrun lists it in DT_NEEDED.
 */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "os"

extern const hos_service_class hos_lm_class;
extern const hos_service_class hos_set_class;
extern const hos_service_class hos_setsys_class;
extern const hos_service_class hos_time_class;
extern const hos_service_class hos_applet_oe_class;
extern const hos_service_class hos_applet_ae_class;
extern const hos_service_class hos_apm_class;
extern const hos_service_class hos_account_class;
extern const hos_service_class hos_account_system_class;
extern const hos_service_class hos_psm_class;
extern const hos_service_class hos_ns_class;
extern const hos_service_class hos_pdm_class;
extern const hos_service_class hos_pl_class;
extern const hos_service_class hos_csrng_class;

__attribute__((constructor)) static void hos_os_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("lm", &hos_lm_class, NULL);
    hos_ipc_register_service("set", &hos_set_class, NULL);
    hos_ipc_register_service("set:sys", &hos_setsys_class, NULL);
    hos_ipc_register_service("time:u", &hos_time_class, NULL);
    hos_ipc_register_service("time:a", &hos_time_class, NULL);
    hos_ipc_register_service("time:s", &hos_time_class, NULL);
    hos_ipc_register_service("appletOE", &hos_applet_oe_class, NULL);
    hos_ipc_register_service("appletAE", &hos_applet_ae_class, NULL);
    hos_ipc_register_service("apm", &hos_apm_class, NULL);
    hos_ipc_register_service("apm:am", &hos_apm_class, NULL);
    hos_ipc_register_service("acc:u0", &hos_account_class, NULL);
    hos_ipc_register_service("acc:u1", &hos_account_system_class, NULL);
    hos_ipc_register_service("acc:su", &hos_account_system_class, NULL);
    hos_ipc_register_service("psm", &hos_psm_class, NULL);
    hos_ipc_register_service("ns:am2", &hos_ns_class, NULL);
    hos_ipc_register_service("pdm:qry", &hos_pdm_class, NULL);
    hos_ipc_register_service("pl:u", &hos_pl_class, NULL);
    hos_ipc_register_service("pl:s", &hos_pl_class, NULL);
    hos_ipc_register_service("csrng", &hos_csrng_class, NULL);
    HOS_LOGD(MOD, "libhos_os loaded");
}
