/* libhos_input: hid today, evdev/XInput backends later. */

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "input"

extern const hos_service_class hos_hid_class;

__attribute__((constructor)) static void hos_input_ctor(void) {
    hos_log_init_from_env();
    hos_ipc_register_service("hid", &hos_hid_class, NULL);
    HOS_LOGD(MOD, "libhos_input loaded");
}
