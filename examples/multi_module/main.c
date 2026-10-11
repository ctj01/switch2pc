/*
 * main: imports sdk_add, sdk_tick and sdk_message from the sdk module. The
 * linker leaves them undefined; hosrun resolves the JUMP_SLOT and GLOB_DAT
 * relocations against the sdk NSO mapped right after this one.
 */

#include "../kernel_test/hos.h"

extern int sdk_add(int a, int b);
extern uint64_t sdk_tick(void);
extern const char *sdk_message;
extern int sdk_ready(void);

void hos_main(uint32_t main_handle) {
    (void)main_handle;
    char line[96];
    char *p;

    int sum = sdk_add(2, 3);
    p = hos_str(line, "sdk_add(2,3) = ");
    hos_hex(p, (uint64_t)sum);
    hos_puts(line);

    uint64_t t0 = sdk_tick();
    uint64_t t1 = sdk_tick();
    hos_puts(t1 > t0 ? "sdk_tick advances" : "FAIL sdk_tick did not advance");

    p = hos_str(line, "message: ");
    hos_str(p, sdk_message);
    hos_puts(line);

    hos_puts(sdk_ready() ? "sdk init ran" : "FAIL sdk initialiser did not run");

    hos_puts(sum == 5 ? "multi_module done" : "FAIL multi_module");
    svc_exit_process();
}
