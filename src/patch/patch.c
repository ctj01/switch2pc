#include "patch/patch.h"

#include <string.h>

static const char *const g_kind_names[PATCH_KIND_COUNT] = {
    "svc->brk",
    "tpidrro_el0->tpidr_el0",
    "cntvct_el0->brk",
    "cntfrq_el0->brk",
};

const char *patch_kind_name(patch_kind kind) {
    if ((unsigned)kind >= PATCH_KIND_COUNT) {
        return "?";
    }
    return g_kind_names[kind];
}

int patch_classify(uint32_t insn, patch_site *site) {
    site->before = insn;
    site->imm = 0;
    if ((insn & A64_SVC_MASK) == A64_SVC_BITS) {
        uint16_t imm = (uint16_t)((insn >> 5) & 0xFFFF);
        if (imm == 0 || imm > HOS_SVC_MAX) {
            return 0;
        }
        site->kind = PATCH_SVC_TO_BRK;
        site->imm = imm;
        site->after = A64_BRK_BITS | ((uint32_t)imm << 5);
        return 1;
    }
    if ((insn & A64_MRS_MASK) == A64_MRS_TPIDRRO) {
        site->kind = PATCH_TPIDRRO_TO_TPIDR;
        site->after = A64_MRS_TPIDR | (insn & 0x1F);
        return 1;
    }
    uint32_t rt = insn & 0x1F;
    if (rt == 31) {
        return 0; /* mrs xzr, ...: a no-op, nothing to emulate */
    }
    if ((insn & A64_MRS_MASK) == A64_MRS_CNTPCT || (insn & A64_MRS_MASK) == A64_MRS_CNTVCT) {
        site->kind = PATCH_COUNTER_TO_BRK;
        site->after = A64_BRK_BITS | ((A64_BRK_TICK_BASE | rt) << 5);
        return 1;
    }
    if ((insn & A64_MRS_MASK) == A64_MRS_CNTFRQ) {
        site->kind = PATCH_CNTFRQ_TO_BRK;
        site->after = A64_BRK_BITS | ((A64_BRK_CNTFRQ_BASE | rt) << 5);
        return 1;
    }
    return 0;
}

hos_result patch_text(uint8_t *text, size_t size, int apply, patch_stats *stats, patch_visit_fn fn,
                      void *user) {
    patch_stats st;
    memset(&st, 0, sizeof(st));
    for (size_t off = 0; off + 4 <= size; off += 4) {
        uint32_t insn;
        memcpy(&insn, text + off, 4);
        patch_site site;
        if (!patch_classify(insn, &site)) {
            continue;
        }
        site.offset = off;
        if (fn) {
            fn(&site, user);
        }
        if (apply) {
            memcpy(text + off, &site.after, 4);
        }
        st.by_kind[site.kind]++;
        st.total++;
    }
    if (stats) {
        *stats = st;
    }
    return HOS_RESULT_SUCCESS;
}
