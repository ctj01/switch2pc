#ifndef HOS_CPU_TLS_LAYOUT_H
#define HOS_CPU_TLS_LAYOUT_H

/*
 * Per-thread HOS Thread Local Region. One page per thread; the game sees the
 * first 0x200 bytes exactly as on hardware. We keep two host-side words in the
 * range HOS marks reserved. Included from assembly, so constants only.
 *
 *   0x000  IPC command buffer (0x100 bytes)
 *   0x100  user exception disable counter
 *   0x108  interrupt flag
 *   0x110  [ours] host tpidr_el0 (glibc TCB), restored while shim code runs
 *   0x118  [ours] hos_thread pointer
 *   0x1E0  reserved for nnSdk
 *   0x1F8  nn::os::ThreadType pointer, written by the game
 */

#define HOS_TLS_SIZE           0x1000
#define HOS_TLS_IPC_BUFFER     0x000
#define HOS_TLS_IPC_SIZE       0x100
#define HOS_TLS_HOST_TP        0x110
#define HOS_TLS_THREAD         0x118
#define HOS_TLS_NN_THREAD_TYPE 0x1F8

/* HOS pseudo-handles. */
#define HOS_HANDLE_CURRENT_THREAD  0xFFFF8000
#define HOS_HANDLE_CURRENT_PROCESS 0xFFFF8001

#endif /* HOS_CPU_TLS_LAYOUT_H */
