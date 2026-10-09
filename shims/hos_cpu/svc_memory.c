#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD "svc"

static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

static int page_aligned(uint64_t v) {
    return (v & 0xFFF) == 0;
}

static int in_region(uint64_t addr, uint64_t size, uint64_t base, uint64_t rsize) {
    return addr >= base && size <= rsize && addr - base <= rsize - size;
}

/* svcSetHeapSize(out x1 addr, x1 size) */
void svc_set_heap_size(hos_svc_regs *r) {
    uint64_t size = r->x[1];
    if (size % HOS_HEAP_ALIGN != 0 || size > HOS_HEAP_MAX) {
        r->x[0] = HOS_KERNEL_INVALID_SIZE;
        return;
    }
    pthread_mutex_lock(&g_heap_lock);
    hos_result res = HOS_RESULT_SUCCESS;
    if (size > hos_heap_size) {
        if (mprotect((void *)(hos_heap_base + hos_heap_size), size - hos_heap_size,
                     PROT_READ | PROT_WRITE) != 0) {
            HOS_LOGE(MOD, "SetHeapSize: mprotect grow failed: %s", strerror(errno));
            res = HOS_KERNEL_OUT_OF_MEMORY;
        } else {
            hos_mem_set(hos_heap_base + hos_heap_size, size - hos_heap_size, HOS_MEM_NORMAL,
                        HOS_PERM_RW, 0);
        }
    } else if (size < hos_heap_size) {
        madvise((void *)(hos_heap_base + size), hos_heap_size - size, MADV_DONTNEED);
        mprotect((void *)(hos_heap_base + size), hos_heap_size - size, PROT_NONE);
        hos_mem_set(hos_heap_base + size, hos_heap_size - size, HOS_MEM_FREE, 0, 0);
    }
    if (HOS_OK(res)) {
        hos_heap_size = size;
        HOS_LOGD(MOD, "heap size now %#" PRIx64 " at %#" PRIx64, size, hos_heap_base);
    }
    pthread_mutex_unlock(&g_heap_lock);
    r->x[0] = res;
    r->x[1] = hos_heap_base;
}

/* svcSetMemoryPermission(x0 addr, x1 size, w2 perm) */
void svc_set_memory_permission(hos_svc_regs *r) {
    uint64_t addr = r->x[0], size = r->x[1];
    uint32_t perm = (uint32_t)r->x[2];
    if (!page_aligned(addr) || !page_aligned(size) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    if (perm & HOS_PERM_EXECUTE) {
        r->x[0] = HOS_KERNEL_INVALID_NEW_MEMORY_PERMISSION;
        return;
    }
    if (mprotect((void *)addr, size, hos_perm_to_prot(perm)) != 0) {
        HOS_LOGW(MOD, "SetMemoryPermission(%#" PRIx64 ", %#" PRIx64 "): %s", addr, size,
                 strerror(errno));
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    hos_memory_info info;
    hos_mem_query(addr, &info);
    hos_mem_set(addr, size, info.state == HOS_MEM_FREE ? HOS_MEM_NORMAL : info.state, perm,
                info.attr);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcSetMemoryAttribute(x0 addr, x1 size, w2 mask, w3 value): cacheability only. */
void svc_set_memory_attribute(hos_svc_regs *r) {
    uint64_t addr = r->x[0], size = r->x[1];
    uint32_t mask = (uint32_t)r->x[2], value = (uint32_t)r->x[3];
    if (!page_aligned(addr) || !page_aligned(size) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    if ((mask | value) & ~HOS_ATTR_UNCACHED) {
        r->x[0] = HOS_KERNEL_INVALID_COMBINATION;
        return;
    }
    hos_memory_info info;
    hos_mem_query(addr, &info);
    if (info.state == HOS_MEM_FREE) {
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    hos_mem_set(addr, size, info.state, info.perm, (info.attr & ~mask) | (value & mask));
    r->x[0] = HOS_RESULT_SUCCESS;
}

/*
 * svcMapMemory(x0 dst, x1 src, x2 size): mirror src at dst, src becomes inaccessible.
 * mremap with MREMAP_FIXED moves the pages, which matches HOS exactly: the source
 * is unmapped and the destination takes over, no copy.
 */
void svc_map_memory(hos_svc_regs *r) {
    uint64_t dst = r->x[0], src = r->x[1], size = r->x[2];
    if (!page_aligned(dst) || !page_aligned(src) || !page_aligned(size) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    if (!in_region(dst, size, HOS_STACK_BASE, HOS_STACK_SIZE) &&
        !in_region(dst, size, HOS_ALIAS_BASE, HOS_ALIAS_SIZE)) {
        HOS_LOGW(MOD, "MapMemory: dst %#" PRIx64 " outside stack/alias regions", dst);
        r->x[0] = HOS_KERNEL_INVALID_MEMORY_REGION;
        return;
    }
    void *p = mremap((void *)src, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)dst);
    if (p == MAP_FAILED) {
        HOS_LOGW(MOD, "MapMemory(%#" PRIx64 " <- %#" PRIx64 ", %#" PRIx64 "): %s", dst, src, size,
                 strerror(errno));
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    /* Keep the source address range reserved so nothing else lands there. */
    void *q = mmap((void *)src, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (q == MAP_FAILED) {
        HOS_LOGW(MOD, "MapMemory: cannot re-reserve source range");
    }
    hos_memory_info info;
    hos_mem_query(src, &info);
    hos_mem_set(src, size, info.state, HOS_PERM_NONE, info.attr | HOS_ATTR_LOCKED);
    hos_mem_set(dst, size, HOS_MEM_STACK, HOS_PERM_RW, 0);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcUnmapMemory(x0 dst, x1 src, x2 size): inverse of svcMapMemory. */
void svc_unmap_memory(hos_svc_regs *r) {
    uint64_t dst = r->x[0], src = r->x[1], size = r->x[2];
    if (!page_aligned(dst) || !page_aligned(src) || !page_aligned(size) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    void *p = mremap((void *)dst, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)src);
    if (p == MAP_FAILED) {
        HOS_LOGW(MOD, "UnmapMemory(%#" PRIx64 " -> %#" PRIx64 ", %#" PRIx64 "): %s", dst, src, size,
                 strerror(errno));
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    void *q = mmap((void *)dst, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (q == MAP_FAILED) {
        HOS_LOGW(MOD, "UnmapMemory: cannot re-reserve destination range");
    }
    hos_memory_info info;
    hos_mem_query(src, &info);
    hos_mem_set(src, size, info.state, HOS_PERM_RW, info.attr & ~HOS_ATTR_LOCKED);
    hos_mem_set(dst, size, HOS_MEM_FREE, 0, 0);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcQueryMemory(x0 MemoryInfo*, out x1 PageInfo, x2 addr) */
void svc_query_memory(hos_svc_regs *r) {
    hos_memory_info *out = (hos_memory_info *)r->x[0];
    if (!out) {
        r->x[0] = HOS_KERNEL_INVALID_POINTER;
        return;
    }
    hos_memory_info info;
    hos_mem_query(r->x[2], &info);
    memcpy(out, &info, sizeof(info));
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = 0;
}
