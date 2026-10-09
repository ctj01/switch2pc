/* libhos_fs: fsp-srv over host directories and a romfs image file. */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "fs"

extern const hos_service_class hos_fsp_class;

__attribute__((constructor)) static void hos_fs_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("fsp-srv", &hos_fsp_class, NULL);
    HOS_LOGD(MOD, "libhos_fs loaded");
}
