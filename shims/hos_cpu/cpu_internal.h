#ifndef HOS_CPU_INTERNAL_H
#define HOS_CPU_INTERNAL_H

/* Private to libhos_cpu. Everything here has hidden visibility. */

#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>

#include "hos_cpu.h"
#include "hos_ipc.h"
#include "patch/patch.h"
#include "tls_layout.h"

#define HOS_HIDDEN __attribute__((visibility("hidden")))

/* HOS address-space reservations (39-bit layout, kept inside 512 GiB). */
#define HOS_IMAGE_HINT     0x0000000008000000ull
#define HOS_HEAP_BASE      0x0000001000000000ull
#define HOS_HEAP_MAX       0x0000000100000000ull /* 4 GiB */
#define HOS_HEAP_ALIGN     0x200000ull
#define HOS_ALIAS_BASE     0x0000002000000000ull
#define HOS_ALIAS_SIZE     0x0000000100000000ull
#define HOS_STACK_BASE     0x0000003000000000ull
#define HOS_STACK_SIZE     0x0000000100000000ull
#define HOS_TLS_ARENA_BASE 0x0000003800000000ull
#define HOS_TLS_ARENA_SIZE 0x0000000004000000ull /* 64 MiB = 16384 threads */
#define HOS_ASLR_BASE      0x0000000008000000ull
#define HOS_ASLR_SIZE      0x0000007FF8000000ull
#define HOS_ALT_STACK_SIZE 0x10000

#define HOS_TIMEOUT_INFINITE    0xFFFFFFFFFFFFFFFFull
#define HOS_MUTEX_HAS_LISTENERS 0x40000000u
#define HOS_WAIT_MAX_HANDLES    0x40

typedef enum hos_obj_type {
    HOS_OBJ_NONE = 0,
    HOS_OBJ_THREAD,
    HOS_OBJ_PROCESS,
    HOS_OBJ_READABLE_EVENT,
    HOS_OBJ_WRITABLE_EVENT,
    HOS_OBJ_SESSION,
    HOS_OBJ_PORT,
    HOS_OBJ_SHARED_MEMORY,
    HOS_OBJ_TRANSFER_MEMORY,
} hos_obj_type;

typedef struct hos_thread {
    uint64_t tls; /* game-visible TLS block */
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_top;
    uint32_t handle;
    uint32_t tid; /* host thread id */
    int32_t priority;
    int32_t core;
    int is_main;
    int exited;         /* set under the sync lock; waitable condition */
    int finished;       /* host thread is done with the struct; under the thread lock */
    int handle_closed;  /* the game closed our handle; free once finished */
    int cancel_pending; /* svcCancelSynchronization arrived while not waiting */
    pthread_t pt;
    sem_t start_sem;
    sigjmp_buf exit_jb; /* svcExitThread lands here */
    void *altstack;
} hos_thread;

typedef struct hos_event {
    int signaled;
    int refs; /* readable + writable handles still open */
    struct hos_event *next_free;
} hos_event;

typedef void (*hos_svc_fn)(hos_svc_regs *r);

/* Globals. The two arena words are read from assembly, so they must stay plain. */
extern HOS_HIDDEN uint64_t hos_tls_arena_base;
extern HOS_HIDDEN uint64_t hos_tls_arena_size;
extern HOS_HIDDEN hos_cpu_params hos_params;
extern HOS_HIDDEN uint64_t hos_heap_base;
extern HOS_HIDDEN uint64_t hos_heap_size;
extern HOS_HIDDEN uint64_t hos_main_stack_base;
extern HOS_HIDDEN uint64_t hos_main_stack_size;
extern HOS_HIDDEN __thread hos_thread *hos_current_thread;

/* trap_aarch64.S */
HOS_HIDDEN void hos_enter(uint64_t entry, uint64_t x0, uint64_t x1, uint64_t sp, uint64_t tls,
                          uint64_t lr) __attribute__((noreturn));
HOS_HIDDEN void hos_trap_entry(int sig, siginfo_t *si, void *uc);

/* trap.c */
HOS_HIDDEN void hos_trap_install(void);
HOS_HIDDEN void hos_trap_c(int sig, siginfo_t *si, void *uc, int in_game);

/* cpu_thread.c */
HOS_HIDDEN uint64_t hos_tls_alloc(void);
HOS_HIDDEN void hos_tls_free(uint64_t tls);
HOS_HIDDEN hos_result hos_tls_arena_init(void);
HOS_HIDDEN hos_result hos_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top,
                                        int32_t priority, int32_t core, uint32_t *out_handle);
HOS_HIDDEN hos_result hos_thread_start(uint32_t handle);
HOS_HIDDEN void hos_thread_exit_current(void) __attribute__((noreturn));
HOS_HIDDEN hos_thread *hos_thread_from_handle(uint32_t handle);
HOS_HIDDEN void hos_thread_handle_closed(hos_thread *t);

/* handles.c */
HOS_HIDDEN uint32_t hos_handle_alloc(hos_obj_type type, void *obj);
HOS_HIDDEN void *hos_handle_get(uint32_t handle, hos_obj_type type);
HOS_HIDDEN void *hos_handle_lookup(uint32_t handle, hos_obj_type *type);
HOS_HIDDEN hos_result hos_handle_close(uint32_t handle);

/* mem_regions.c */
HOS_HIDDEN hos_result hos_mem_init(void);
HOS_HIDDEN int hos_perm_to_prot(uint32_t perm);

/* sync.c */
HOS_HIDDEN void hos_sync_thread_exited(hos_thread *t);
HOS_HIDDEN void hos_event_release(hos_event *e);
HOS_HIDDEN hos_result hos_event_create(uint32_t *out_writable, uint32_t *out_readable);
HOS_HIDDEN void hos_event_signal(hos_event *e);

/* ipc.c */
HOS_HIDDEN hos_result hos_ipc_init(void);
HOS_HIDDEN void hos_session_close(hos_session *s);
HOS_HIDDEN hos_result hos_ipc_connect_named_port(const char *name, uint32_t *out_handle);
HOS_HIDDEN hos_result hos_ipc_send_sync(hos_session *s, uint32_t *tls_buf);
HOS_HIDDEN hos_object *hos_ipc_service_instance(const char *name);

/* ipc_sm.c */
HOS_HIDDEN void hos_sm_init(void);

/* svc_misc.c */
HOS_HIDDEN uint64_t hos_tick_now(void);

/* svc_shmem.c */
HOS_HIDDEN void hos_shmem_release(hos_shmem *s);
HOS_HIDDEN void hos_tmem_release(void *tmem);
HOS_HIDDEN void svc_create_shared_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_map_shared_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_unmap_shared_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_create_transfer_memory(hos_svc_regs *r);

/* svc handlers */
HOS_HIDDEN void svc_set_heap_size(hos_svc_regs *r);
HOS_HIDDEN void svc_set_memory_permission(hos_svc_regs *r);
HOS_HIDDEN void svc_set_memory_attribute(hos_svc_regs *r);
HOS_HIDDEN void svc_map_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_unmap_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_query_memory(hos_svc_regs *r);
HOS_HIDDEN void svc_exit_process(hos_svc_regs *r);
HOS_HIDDEN void svc_create_thread(hos_svc_regs *r);
HOS_HIDDEN void svc_start_thread(hos_svc_regs *r);
HOS_HIDDEN void svc_exit_thread(hos_svc_regs *r);
HOS_HIDDEN void svc_sleep_thread(hos_svc_regs *r);
HOS_HIDDEN void svc_get_thread_priority(hos_svc_regs *r);
HOS_HIDDEN void svc_set_thread_priority(hos_svc_regs *r);
HOS_HIDDEN void svc_get_thread_core_mask(hos_svc_regs *r);
HOS_HIDDEN void svc_set_thread_core_mask(hos_svc_regs *r);
HOS_HIDDEN void svc_get_current_processor_number(hos_svc_regs *r);
HOS_HIDDEN void svc_signal_event(hos_svc_regs *r);
HOS_HIDDEN void svc_clear_event(hos_svc_regs *r);
HOS_HIDDEN void svc_close_handle(hos_svc_regs *r);
HOS_HIDDEN void svc_reset_signal(hos_svc_regs *r);
HOS_HIDDEN void svc_wait_synchronization(hos_svc_regs *r);
HOS_HIDDEN void svc_cancel_synchronization(hos_svc_regs *r);
HOS_HIDDEN void svc_arbitrate_lock(hos_svc_regs *r);
HOS_HIDDEN void svc_arbitrate_unlock(hos_svc_regs *r);
HOS_HIDDEN void svc_wait_process_wide_key_atomic(hos_svc_regs *r);
HOS_HIDDEN void svc_signal_process_wide_key(hos_svc_regs *r);
HOS_HIDDEN void svc_get_system_tick(hos_svc_regs *r);
HOS_HIDDEN void svc_connect_to_named_port(hos_svc_regs *r);
HOS_HIDDEN void svc_send_sync_request(hos_svc_regs *r);
HOS_HIDDEN void svc_get_process_id(hos_svc_regs *r);
HOS_HIDDEN void svc_get_thread_id(hos_svc_regs *r);
HOS_HIDDEN void svc_break(hos_svc_regs *r);
HOS_HIDDEN void svc_output_debug_string(hos_svc_regs *r);
HOS_HIDDEN void svc_get_info(hos_svc_regs *r);
HOS_HIDDEN void svc_flush_cache_noop(hos_svc_regs *r);
HOS_HIDDEN void svc_wait_for_address(hos_svc_regs *r);
HOS_HIDDEN void svc_signal_to_address(hos_svc_regs *r);
HOS_HIDDEN void svc_create_event(hos_svc_regs *r);

#endif /* HOS_CPU_INTERNAL_H */
