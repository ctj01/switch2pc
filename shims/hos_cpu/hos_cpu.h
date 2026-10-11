#ifndef HOS_CPU_H
#define HOS_CPU_H

/*
 * libhos_cpu: the kernel the game thinks it is talking to.
 *
 * Game code runs natively in this process. Every `svc #n` in the module was
 * rewritten to `brk #n` by the patcher; the SIGTRAP handler installed here
 * reads the number, calls hos_svc_dispatch with x0..x7, writes the results
 * back into the signal frame and resumes four bytes later.
 *
 * Game threads carry a HOS TLS block in tpidr_el0 (see tls_layout.h). While
 * shim code runs, tpidr_el0 is swapped back to the host TCB so libc works.
 */

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"

#define HOS_EXPORT __attribute__((visibility("default")))

/* x0..x7 on entry; x0..x7 on return. Unused slots pass through unchanged. */
typedef struct hos_svc_regs {
    uint64_t x[8];
} hos_svc_regs;

/* HOS memory states as reported by svcQueryMemory. */
enum hos_mem_state {
    HOS_MEM_FREE = 0x00,
    HOS_MEM_IO = 0x01,
    HOS_MEM_STATIC = 0x02,
    HOS_MEM_CODE = 0x03,
    HOS_MEM_CODE_DATA = 0x04,
    HOS_MEM_NORMAL = 0x05, /* heap */
    HOS_MEM_SHARED = 0x06,
    HOS_MEM_ALIAS = 0x07,
    HOS_MEM_ALIAS_CODE = 0x08,
    HOS_MEM_ALIAS_CODE_DATA = 0x09,
    HOS_MEM_IPC = 0x0A,
    HOS_MEM_STACK = 0x0B,
    HOS_MEM_THREAD_LOCAL = 0x0C,
    HOS_MEM_TRANSFERED = 0x0D,
    HOS_MEM_SHARED_TRANSFERED = 0x0E,
    HOS_MEM_SHARED_CODE = 0x0F,
    HOS_MEM_INACCESSIBLE = 0x10,
    HOS_MEM_NON_SECURE_IPC = 0x11,
    HOS_MEM_NON_DEVICE_IPC = 0x12,
    HOS_MEM_KERNEL = 0x13,
    HOS_MEM_GENERATED_CODE = 0x14,
    HOS_MEM_CODE_OUT = 0x15,
};

#define HOS_PERM_NONE    0u
#define HOS_PERM_READ    (1u << 0)
#define HOS_PERM_WRITE   (1u << 1)
#define HOS_PERM_EXECUTE (1u << 2)
#define HOS_PERM_RW      (HOS_PERM_READ | HOS_PERM_WRITE)
#define HOS_PERM_RX      (HOS_PERM_READ | HOS_PERM_EXECUTE)

#define HOS_ATTR_LOCKED   (1u << 0)
#define HOS_ATTR_IPC      (1u << 1)
#define HOS_ATTR_DEVICE   (1u << 2)
#define HOS_ATTR_UNCACHED (1u << 3)

/* Mirrors the kernel MemoryInfo struct (0x28 bytes). */
typedef struct hos_memory_info {
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
} hos_memory_info;

_Static_assert(sizeof(hos_memory_info) == 0x28, "MemoryInfo must be 0x28 bytes");

#define HOS_MAX_MODULES 16

/* One mapped module. Offsets are relative to image_base; data_size includes bss. */
typedef struct hos_cpu_module {
    uint64_t image_base;
    uint64_t image_size;
    uint64_t text_off, text_size;
    uint64_t rodata_off, rodata_size;
    uint64_t data_off, data_size;
    const char *name;
} hos_cpu_module;

/* What the loader tells us about the process image. */
typedef struct hos_cpu_params {
    uint32_t num_modules;
    hos_cpu_module modules[HOS_MAX_MODULES];
    uint64_t program_id; /* title id, e.g. 0000000000010000 */
} hos_cpu_params;

/**
 * Initialise the CPU shim for a loaded module: reserve the HOS address-space
 * regions (heap, alias, stack, TLS) and register the module in the memory map.
 * Call once, before hos_cpu_run_main.
 *
 * @param p  Module description
 * @return HOS_RESULT_SUCCESS or HOS_KERNEL_OUT_OF_MEMORY
 */
HOS_EXPORT hos_result hos_cpu_init(const hos_cpu_params *p);

/**
 * Dispatch one supervisor call. Called from the trap handler, but also usable
 * directly from tests. Runs with the host TCB in tpidr_el0.
 *
 * @param id  svc number 0x01..0x7F
 * @param r   Registers in/out
 */
HOS_EXPORT void hos_svc_dispatch(uint32_t id, hos_svc_regs *r);

/**
 * Turn the calling thread into the HOS main thread and jump to `entry` with
 * x0 = 0, x1 = main thread handle and a fresh stack. Does not return; the game
 * ends the process through svcExitProcess.
 *
 * @param entry       Address of the first instruction
 * @param stack_size  Main stack size in bytes (page multiple)
 */
HOS_EXPORT void hos_cpu_run_main(uint64_t entry, uint64_t stack_size) __attribute__((noreturn));

/**
 * Give the calling host thread a HOS identity (TLS block, handle) without
 * entering game code. For tests and tools that call hos_svc_dispatch directly.
 *
 * @return The thread handle, or 0 on failure
 */
HOS_EXPORT uint32_t hos_cpu_adopt_current_thread(void);

/* A shared memory object: a memfd the game maps with svcMapSharedMemory. */
typedef struct hos_shmem {
    int fd;
    uint64_t size;
    int refs;
    void *host_view;
} hos_shmem;

/**
 * Create a shared memory object and map a host-side view of it. Returns a
 * handle for the game (pass it as a copy handle in an IPC reply), 0 on failure.
 * `size` must be a page multiple.
 */
HOS_EXPORT uint32_t hos_shmem_create(uint64_t size, void **host_view);

/**
 * The HOS TLS block of the calling thread (IPC buffer at offset 0), or NULL
 * when the thread has no HOS identity.
 */
HOS_EXPORT void *hos_cpu_current_tls(void);

/**
 * Record a range in the HOS memory map that svcQueryMemory reports. Overlapping
 * entries are split. Does not touch host page tables.
 */
HOS_EXPORT hos_result hos_mem_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm,
                                  uint32_t attr);

/**
 * Fill `info` for the region containing `addr`, or the free gap around it.
 */
HOS_EXPORT void hos_mem_query(uint64_t addr, hos_memory_info *info);

/**
 * Log the whole memory map at INFO level.
 */
HOS_EXPORT void hos_mem_dump(void);

#endif /* HOS_CPU_H */
