/*
 * The HOS memory map as seen through svcQueryMemory. A sorted array of
 * non-overlapping regions; gaps are reported as Free. This is bookkeeping
 * only: host page tables are changed by the svc handlers, not here.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD               "mem"
#define REGION_MAX        1024
#define ADDRESS_SPACE_END (1ull << 39)

typedef struct region {
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t perm;
    uint32_t attr;
} region;

static region g_regions[REGION_MAX];
static size_t g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t hos_heap_base;
uint64_t hos_heap_size;

int hos_perm_to_prot(uint32_t perm) {
    int prot = PROT_NONE;
    if (perm & HOS_PERM_READ) {
        prot |= PROT_READ;
    }
    if (perm & HOS_PERM_WRITE) {
        prot |= PROT_WRITE;
    }
    if (perm & HOS_PERM_EXECUTE) {
        prot |= PROT_EXEC;
    }
    return prot;
}

static const char *state_name(uint32_t s) {
    static const char *const names[] = {
        "Free",      "Io",         "Static",      "Code",       "CodeData", "Normal",
        "Shared",    "Alias",      "AliasCode",   "AliasCodeD", "Ipc",      "Stack",
        "ThreadLoc", "Transfered", "SharedTrans", "SharedCode", "Inaccess", "NonSecIpc",
        "NonDevIpc", "Kernel",     "GenCode",     "CodeOut",
    };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static int insert_at(size_t idx, const region *r) {
    if (g_count >= REGION_MAX) {
        return 0;
    }
    memmove(&g_regions[idx + 1], &g_regions[idx], (g_count - idx) * sizeof(region));
    g_regions[idx] = *r;
    g_count++;
    return 1;
}

static void remove_at(size_t idx) {
    memmove(&g_regions[idx], &g_regions[idx + 1], (g_count - idx - 1) * sizeof(region));
    g_count--;
}

/* Remove [addr, addr+size) from every region, splitting as needed. */
static int carve(uint64_t addr, uint64_t size) {
    uint64_t end = addr + size;
    for (size_t i = 0; i < g_count;) {
        region *r = &g_regions[i];
        uint64_t rend = r->addr + r->size;
        if (rend <= addr || r->addr >= end) {
            i++;
            continue;
        }
        if (r->addr < addr && rend > end) {
            /* hole in the middle: keep head, add tail */
            region tail = {end, rend - end, r->state, r->perm, r->attr};
            r->size = addr - r->addr;
            if (!insert_at(i + 1, &tail)) {
                return 0;
            }
            return 1;
        }
        if (r->addr < addr) {
            r->size = addr - r->addr;
            i++;
        } else if (rend > end) {
            r->size = rend - end;
            r->addr = end;
            i++;
        } else {
            remove_at(i);
        }
    }
    return 1;
}

/* Merge neighbours with identical attributes. */
static void coalesce(void) {
    for (size_t i = 0; i + 1 < g_count;) {
        region *a = &g_regions[i];
        region *b = &g_regions[i + 1];
        if (a->addr + a->size == b->addr && a->state == b->state && a->perm == b->perm &&
            a->attr == b->attr) {
            a->size += b->size;
            remove_at(i + 1);
        } else {
            i++;
        }
    }
}

hos_result hos_mem_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm, uint32_t attr) {
    if (size == 0 || addr + size < addr) {
        return HOS_KERNEL_INVALID_SIZE;
    }
    pthread_mutex_lock(&g_lock);
    hos_result res = HOS_RESULT_SUCCESS;
    if (!carve(addr, size)) {
        res = HOS_KERNEL_OUT_OF_RESOURCE;
        goto out;
    }
    if (state != HOS_MEM_FREE) {
        size_t i = 0;
        while (i < g_count && g_regions[i].addr < addr) {
            i++;
        }
        region r = {addr, size, state, perm, attr};
        if (!insert_at(i, &r)) {
            res = HOS_KERNEL_OUT_OF_RESOURCE;
            goto out;
        }
    }
    coalesce();
out:
    pthread_mutex_unlock(&g_lock);
    if (HOS_FAILED(res)) {
        HOS_LOGE(MOD, "region table full (%d entries)", REGION_MAX);
    }
    return res;
}

void hos_mem_query(uint64_t addr, hos_memory_info *info) {
    memset(info, 0, sizeof(*info));
    pthread_mutex_lock(&g_lock);
    uint64_t gap_start = 0;
    uint64_t gap_end = ADDRESS_SPACE_END;
    for (size_t i = 0; i < g_count; i++) {
        const region *r = &g_regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info->addr = r->addr;
            info->size = r->size;
            info->state = r->state;
            info->perm = r->perm;
            info->attr = r->attr;
            pthread_mutex_unlock(&g_lock);
            return;
        }
        if (r->addr + r->size <= addr) {
            gap_start = r->addr + r->size;
        } else {
            gap_end = r->addr;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (addr >= ADDRESS_SPACE_END) {
        info->addr = ADDRESS_SPACE_END;
        info->size = 0 - ADDRESS_SPACE_END;
        info->state = HOS_MEM_INACCESSIBLE;
        return;
    }
    info->addr = gap_start;
    info->size = gap_end - gap_start;
    info->state = HOS_MEM_FREE;
}

void hos_mem_dump(void) {
    pthread_mutex_lock(&g_lock);
    HOS_LOGI(MOD, "memory map (%zu regions):", g_count);
    for (size_t i = 0; i < g_count; i++) {
        const region *r = &g_regions[i];
        HOS_LOGI(MOD, "  %#014" PRIx64 " - %#014" PRIx64 "  %-10s %c%c%c attr %#x", r->addr,
                 r->addr + r->size, state_name(r->state), (r->perm & HOS_PERM_READ) ? 'r' : '-',
                 (r->perm & HOS_PERM_WRITE) ? 'w' : '-', (r->perm & HOS_PERM_EXECUTE) ? 'x' : '-',
                 r->attr);
    }
    pthread_mutex_unlock(&g_lock);
}

static hos_result reserve(uint64_t base, uint64_t size, const char *what) {
    void *p =
        mmap((void *)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        HOS_LOGE(MOD, "cannot reserve %s region at %#" PRIx64 ": %s", what, base, strerror(errno));
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    if ((uint64_t)p != base) {
        HOS_LOGE(MOD, "%s region landed at %p instead of %#" PRIx64, what, p, base);
        munmap(p, size);
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    HOS_LOGD(MOD, "%s region reserved at %#" PRIx64 " (%#" PRIx64 " bytes)", what, base, size);
    return HOS_RESULT_SUCCESS;
}

hos_result hos_mem_init(void) {
    hos_result r = reserve(HOS_HEAP_BASE, HOS_HEAP_MAX, "heap");
    if (HOS_OK(r)) {
        r = reserve(HOS_ALIAS_BASE, HOS_ALIAS_SIZE, "alias");
    }
    if (HOS_OK(r)) {
        r = reserve(HOS_STACK_BASE, HOS_STACK_SIZE, "stack");
    }
    hos_heap_base = HOS_HEAP_BASE;
    hos_heap_size = 0;
    return r;
}
