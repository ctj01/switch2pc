#include <inttypes.h>
#include <string.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD "cpu"

hos_cpu_params hos_params;

/* Runs when the dynamic linker maps libhos_cpu.so, before main. */
__attribute__((constructor)) static void hos_cpu_ctor(void) {
    hos_log_init_from_env();
    hos_trap_install();
    HOS_LOGD(MOD, "libhos_cpu loaded; trap handlers installed");
}

hos_result hos_cpu_init(const hos_cpu_params *p) {
    if (p->num_modules > HOS_MAX_MODULES) {
        return HOS_KERNEL_OUT_OF_RANGE;
    }
    hos_params = *p;
    hos_result r = hos_mem_init();
    if (HOS_OK(r)) {
        r = hos_tls_arena_init();
    }
    if (HOS_OK(r)) {
        r = hos_ipc_init();
    }
    if (HOS_FAILED(r)) {
        return r;
    }
    for (uint32_t i = 0; i < p->num_modules; i++) {
        const hos_cpu_module *m = &p->modules[i];
        if (m->text_size) {
            hos_mem_set(m->image_base + m->text_off, m->text_size, HOS_MEM_CODE, HOS_PERM_RX, 0);
        }
        if (m->rodata_size) {
            hos_mem_set(m->image_base + m->rodata_off, m->rodata_size, HOS_MEM_CODE, HOS_PERM_READ,
                        0);
        }
        if (m->data_size) {
            hos_mem_set(m->image_base + m->data_off, m->data_size, HOS_MEM_CODE_DATA, HOS_PERM_RW,
                        0);
        }
        HOS_LOGD(MOD, "module %s at %#" PRIx64 " (%#" PRIx64 " bytes) registered",
                 m->name ? m->name : "?", m->image_base, m->image_size);
    }
    return HOS_RESULT_SUCCESS;
}
