/*
 * Shared memory and transfer memory. A shared memory object is a memfd; the
 * game maps it with svcMapSharedMemory and a service library maps its own
 * view with hos_shmem_create. Both see the same pages, which is exactly what
 * HOS shared memory is.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD        "shmem"
#define SHMEM_POOL 1024

typedef struct hos_tmem {
    uint64_t addr;
    uint64_t size;
    uint32_t perm;
} hos_tmem;

static hos_shmem g_shmem_pool[SHMEM_POOL];
static hos_tmem g_tmem_pool[SHMEM_POOL];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static hos_shmem *shmem_alloc(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SHMEM_POOL; i++) {
        if (g_shmem_pool[i].refs == 0) {
            g_shmem_pool[i].refs = 1;
            pthread_mutex_unlock(&g_lock);
            return &g_shmem_pool[i];
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static hos_shmem *shmem_new(uint64_t size) {
    if (size == 0 || (size & 0xFFF)) {
        return NULL;
    }
    hos_shmem *s = shmem_alloc();
    if (!s) {
        HOS_LOGE(MOD, "shared memory pool exhausted");
        return NULL;
    }
    s->fd = memfd_create("hos_shmem", 0);
    if (s->fd < 0 || ftruncate(s->fd, (off_t)size) != 0) {
        HOS_LOGE(MOD, "memfd: %s", strerror(errno));
        if (s->fd >= 0) {
            close(s->fd);
        }
        s->refs = 0;
        return NULL;
    }
    s->size = size;
    s->host_view = NULL;
    return s;
}

void hos_shmem_release(hos_shmem *s) {
    pthread_mutex_lock(&g_lock);
    int last = (--s->refs == 0);
    pthread_mutex_unlock(&g_lock);
    if (!last) {
        return;
    }
    if (s->host_view) {
        munmap(s->host_view, s->size);
        s->host_view = NULL;
    }
    close(s->fd);
    s->fd = -1;
}

uint32_t hos_shmem_create(uint64_t size, void **host_view) {
    hos_shmem *s = shmem_new(size);
    if (!s) {
        return 0;
    }
    s->host_view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->host_view == MAP_FAILED) {
        s->host_view = NULL;
        hos_shmem_release(s);
        return 0;
    }
    memset(s->host_view, 0, size);
    uint32_t h = hos_handle_alloc(HOS_OBJ_SHARED_MEMORY, s);
    if (!h) {
        hos_shmem_release(s);
        return 0;
    }
    if (host_view) {
        *host_view = s->host_view;
    }
    return h;
}

/* svcCreateSharedMemory(out w1 handle, x1 size, w2 owner perm, w3 remote perm) */
void svc_create_shared_memory(hos_svc_regs *r) {
    uint64_t size = r->x[1];
    hos_shmem *s = shmem_new(size);
    if (!s) {
        r->x[0] =
            (size == 0 || (size & 0xFFF)) ? HOS_KERNEL_INVALID_SIZE : HOS_KERNEL_OUT_OF_RESOURCE;
        return;
    }
    uint32_t h = hos_handle_alloc(HOS_OBJ_SHARED_MEMORY, s);
    if (!h) {
        hos_shmem_release(s);
        r->x[0] = HOS_KERNEL_OUT_OF_HANDLES;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = h;
}

/* svcMapSharedMemory(w0 handle, x1 addr, x2 size, w3 perm) */
void svc_map_shared_memory(hos_svc_regs *r) {
    hos_shmem *s = hos_handle_get((uint32_t)r->x[0], HOS_OBJ_SHARED_MEMORY);
    uint64_t addr = r->x[1], size = r->x[2];
    uint32_t perm = (uint32_t)r->x[3];
    if (!s) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    if ((addr & 0xFFF) || (size & 0xFFF) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    if (size != s->size) {
        r->x[0] = HOS_KERNEL_INVALID_SIZE;
        return;
    }
    if (perm & HOS_PERM_EXECUTE) {
        r->x[0] = HOS_KERNEL_INVALID_NEW_MEMORY_PERMISSION;
        return;
    }
    void *p = mmap((void *)addr, size, hos_perm_to_prot(perm), MAP_SHARED | MAP_FIXED, s->fd, 0);
    if (p == MAP_FAILED) {
        HOS_LOGW(MOD, "MapSharedMemory at %#" PRIx64 ": %s", addr, strerror(errno));
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    hos_mem_set(addr, size, HOS_MEM_SHARED, perm, 0);
    HOS_LOGD(MOD, "shared memory %#x mapped at %#" PRIx64 " (%#" PRIx64 ")", (uint32_t)r->x[0],
             addr, size);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcUnmapSharedMemory(w0 handle, x1 addr, x2 size) */
void svc_unmap_shared_memory(hos_svc_regs *r) {
    hos_shmem *s = hos_handle_get((uint32_t)r->x[0], HOS_OBJ_SHARED_MEMORY);
    uint64_t addr = r->x[1], size = r->x[2];
    if (!s) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    if ((addr & 0xFFF) || (size & 0xFFF) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    void *p = mmap((void *)addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        r->x[0] = HOS_KERNEL_INVALID_CURRENT_MEMORY;
        return;
    }
    hos_mem_set(addr, size, HOS_MEM_FREE, 0, 0);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcCreateTransferMemory(out w1 handle, x1 addr, x2 size, w3 perm) */
void svc_create_transfer_memory(hos_svc_regs *r) {
    uint64_t addr = r->x[1], size = r->x[2];
    uint32_t perm = (uint32_t)r->x[3];
    if ((addr & 0xFFF) || (size & 0xFFF) || size == 0) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    hos_tmem *t = NULL;
    for (int i = 0; i < SHMEM_POOL; i++) {
        if (g_tmem_pool[i].size == 0) {
            t = &g_tmem_pool[i];
            t->addr = addr;
            t->size = size;
            t->perm = perm;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!t) {
        r->x[0] = HOS_KERNEL_OUT_OF_RESOURCE;
        return;
    }
    uint32_t h = hos_handle_alloc(HOS_OBJ_TRANSFER_MEMORY, t);
    if (!h) {
        t->size = 0;
        r->x[0] = HOS_KERNEL_OUT_OF_HANDLES;
        return;
    }
    /* The owner keeps its pages; services in this process read them directly. */
    hos_memory_info info;
    hos_mem_query(addr, &info);
    hos_mem_set(addr, size, info.state, info.perm, info.attr | HOS_ATTR_LOCKED);
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = h;
}

void hos_tmem_release(void *obj) {
    hos_tmem *t = obj;
    hos_memory_info info;
    hos_mem_query(t->addr, &info);
    if (info.state != HOS_MEM_FREE) {
        hos_mem_set(t->addr, t->size, info.state, info.perm, info.attr & ~HOS_ATTR_LOCKED);
    }
    pthread_mutex_lock(&g_lock);
    t->size = 0;
    pthread_mutex_unlock(&g_lock);
}
