#include <pthread.h>
#include <string.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD          "handle"
#define HANDLE_COUNT 4096
#define HANDLE_BASE  0x100u /* keep clear of 0 and small magic values */

typedef struct handle_slot {
    hos_obj_type type;
    void *obj;
} handle_slot;

static handle_slot g_slots[HANDLE_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next;

uint32_t hos_handle_alloc(hos_obj_type type, void *obj) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t n = 0; n < HANDLE_COUNT; n++) {
        uint32_t i = (g_next + n) % HANDLE_COUNT;
        if (g_slots[i].type == HOS_OBJ_NONE) {
            g_slots[i].type = type;
            g_slots[i].obj = obj;
            g_next = i + 1;
            pthread_mutex_unlock(&g_lock);
            return HANDLE_BASE + i;
        }
    }
    pthread_mutex_unlock(&g_lock);
    HOS_LOGE(MOD, "out of handles");
    return 0;
}

void *hos_handle_lookup(uint32_t handle, hos_obj_type *type) {
    *type = HOS_OBJ_NONE;
    if (handle < HANDLE_BASE || handle - HANDLE_BASE >= HANDLE_COUNT) {
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    handle_slot s = g_slots[handle - HANDLE_BASE];
    pthread_mutex_unlock(&g_lock);
    *type = s.type;
    return s.obj;
}

void *hos_handle_get(uint32_t handle, hos_obj_type type) {
    hos_obj_type t;
    void *obj = hos_handle_lookup(handle, &t);
    return (obj && t == type) ? obj : NULL;
}

hos_result hos_handle_close(uint32_t handle) {
    if (handle == HOS_HANDLE_CURRENT_THREAD || handle == HOS_HANDLE_CURRENT_PROCESS) {
        return HOS_KERNEL_INVALID_HANDLE;
    }
    if (handle < HANDLE_BASE || handle - HANDLE_BASE >= HANDLE_COUNT) {
        return HOS_KERNEL_INVALID_HANDLE;
    }
    pthread_mutex_lock(&g_lock);
    handle_slot *s = &g_slots[handle - HANDLE_BASE];
    hos_result r = (s->type == HOS_OBJ_NONE) ? HOS_KERNEL_INVALID_HANDLE : HOS_RESULT_SUCCESS;
    s->type = HOS_OBJ_NONE;
    s->obj = NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

uint32_t hos_ipc_copy_handle(uint32_t handle) {
    hos_obj_type type;
    void *obj = hos_handle_lookup(handle, &type);
    if (!obj) {
        return 0;
    }
    switch (type) {
    case HOS_OBJ_READABLE_EVENT:
    case HOS_OBJ_WRITABLE_EVENT:
        __atomic_add_fetch(&((hos_event *)obj)->refs, 1, __ATOMIC_SEQ_CST);
        break;
    case HOS_OBJ_SHARED_MEMORY:
        __atomic_add_fetch(&((hos_shmem *)obj)->refs, 1, __ATOMIC_SEQ_CST);
        break;
    default:
        return 0;
    }
    return hos_handle_alloc(type, obj);
}
