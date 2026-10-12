#ifndef HOS_GPU_ENGINES_H
#define HOS_GPU_ENGINES_H

#include <stdint.h>

/*
 * The GPU engines a channel drives (docs/gpu.md, M1), executed on the CPU.
 * Every method write lands in a register file per engine class, as on the
 * hardware; a few methods trigger work that reads those registers:
 *
 *   3D (0xB197)          CLEAR_SURFACE into colour render targets,
 *                        report semaphores, inline-to-memory
 *   compute (0xB1C0)     report semaphores, inline-to-memory
 *   inline-to-memory     (0xA140) uploads from the pushbuffer
 *   DMA copy (0xB0B5)    pitch and block-linear copies, constant fills,
 *                        semaphores
 *
 * Drawing (vertex fetch, shaders, rasterisation) is M3 and is not here.
 */

typedef struct gpu_engines gpu_engines;

typedef struct gpu_engine_stats {
    uint64_t releases;
    uint64_t clears;
    uint64_t copies;
    uint64_t inline_uploads;
    uint64_t acquires_skipped;
    uint64_t unsupported;
    uint64_t macros;
} gpu_engine_stats;

gpu_engines *gpu_engines_new(void);
void gpu_engines_free(gpu_engines *e);

/** One method write on a subchannel (methods >= 0x40; host methods stay in nvgpu.c). */
void gpu_engines_method(gpu_engines *e, uint32_t subch, uint32_t method, uint32_t value);

/** End of a submission: run a macro call whose parameters are complete. */
void gpu_engines_flush(gpu_engines *e);

/** SET_OBJECT on a subchannel. */
void gpu_engines_bind(gpu_engines *e, uint32_t subch, uint32_t cls);

const gpu_engine_stats *gpu_engines_stats(const gpu_engines *e);

struct gpu_draw_stats;
const struct gpu_draw_stats *gpu_engines_draw_stats(const gpu_engines *e);

/** Write a semaphore: one word, or four (payload, 0, 64-bit timestamp). */
int gpu_semaphore_release(uint64_t gpu_va, uint32_t payload, int four_words);

#endif /* HOS_GPU_ENGINES_H */
