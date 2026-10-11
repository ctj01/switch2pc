/*
 * The GPU half of nvdrv (milestone M0 in docs/gpu.md): /dev/nvhost-ctrl-gpu,
 * /dev/nvhost-as-gpu and /dev/nvhost-gpu. The driver accepts the whole
 * conversation a GPU client has with the kernel: characteristics and zcull
 * queries, a GPU virtual address space whose mappings point at nvmap
 * memory (which is the game's own memory here), channels with a GPFIFO, and
 * submissions that return syncpoint fences. Submissions are decoded and
 * checked (every GPFIFO entry must resolve to mapped memory, every method
 * header must parse) and run: host methods (semaphores, syncpoint
 * increments) here, engine methods in gpu_engines.c (clears, copies, inline
 * uploads, report semaphores; M1). Fences complete as the work is done at
 * submit time.
 *
 * Ioctl numbers follow Linux's _IOC layout: dir (2 bits) | size (14) | type
 * (8) | nr (8). Unknown ones on these devices are logged with their size and
 * answered with success and their input echoed, which is how the ones that
 * matter were found.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/hos_log.h"
#include "gpu_engines.h"
#include "gpu_internal.h"

#define MOD "nvgpu"

#define NV_OK               0
#define NV_BAD_PARAMETER    4
#define NV_INSUFFICIENT_MEM 0xA

#define IOC_NR(c)   ((c) & 0xFF)
#define IOC_TYPE(c) (((c) >> 8) & 0xFF)
#define IOC_SIZE(c) (((c) >> 16) & 0x3FFF)

#define MAX_MAPPINGS 4096
#define MAX_CHANNELS 16
#define MAX_SYNCPTS  192
#define FIRST_SYNCPT 48 /* channel syncpoints; low ids belong to the display */

/* GPU VA layout reported by GET_VA_REGIONS (40-bit space, as on GM20B). */
#define VA_SMALL_BASE 0x0004000000ull
#define VA_BIG_BASE   0x0400000000ull
#define VA_END        0x10000000000ull
#define SMALL_PAGE    0x1000u
#define BIG_PAGE      0x20000u

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static void wr64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* ---- syncpoints (shared with nvhost-ctrl) ------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_syncpt[MAX_SYNCPTS];
static uint32_t g_next_syncpt = FIRST_SYNCPT;

/*
 * M0 runs nothing, so every piece of work is done the moment anyone asks:
 * a wait for a threshold raises the syncpoint to it. Clients compute
 * thresholds themselves from the increments they put in their pushbuffers
 * (deko3d does), so the submit-time count alone would never get there.
 */
uint32_t nv_syncpt_reach(uint32_t id, uint32_t threshold) {
    pthread_mutex_lock(&g_lock);
    uint32_t v = 0;
    if (id < MAX_SYNCPTS) {
        if ((int32_t)(threshold - g_syncpt[id]) > 0) {
            g_syncpt[id] = threshold;
        }
        v = g_syncpt[id];
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint32_t nv_syncpt_read(uint32_t id) {
    pthread_mutex_lock(&g_lock);
    uint32_t v = id < MAX_SYNCPTS ? g_syncpt[id] : 0;
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* ---- address space ----------------------------------------------------------------- */

typedef struct gpu_mapping {
    int used;
    uint64_t va;
    uint64_t size;
    uint32_t handle;
    uint64_t buffer_offset;
    uint32_t kind; /* memory kind: pitch, generic 16Bx2 (0xfe), compressible... */
} gpu_mapping;

static gpu_mapping g_maps[MAX_MAPPINGS];
static uint64_t g_small_cursor = VA_SMALL_BASE;
static uint64_t g_big_cursor = VA_BIG_BASE;
static uint32_t g_big_page = BIG_PAGE; /* INITIALIZE_EX may pick 64 KiB (deko3d does) */

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

/*
 * g_lock held. Bump allocation, aligned to the page size the client asked for
 * (clients check that alignment); the space is large enough that reuse can
 * wait. Big-page requests come from the upper region, as on hardware.
 */
static uint64_t va_alloc(uint64_t size, uint64_t align, uint32_t page_size) {
    if (align < page_size) {
        align = page_size;
    }
    if (align < SMALL_PAGE || (align & (align - 1)) != 0) {
        align = SMALL_PAGE;
    }
    uint64_t *cursor = page_size >= g_big_page ? &g_big_cursor : &g_small_cursor;
    uint64_t limit = page_size >= g_big_page ? VA_END : VA_BIG_BASE;
    uint64_t va = align_up(*cursor, align);
    if (va + size > limit) {
        return 0;
    }
    *cursor = align_up(va + size, page_size >= g_big_page ? g_big_page : SMALL_PAGE);
    return va;
}

/* GPU VA -> host pointer, or NULL if nothing is mapped there for `len` bytes. */
void *nv_gpu_va_to_host(uint64_t va, uint64_t len) {
    pthread_mutex_lock(&g_lock);
    void *p = NULL;
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        gpu_mapping *m = &g_maps[i];
        if (m->used && va >= m->va && va - m->va < m->size && len <= m->size - (va - m->va)) {
            uint64_t addr, size;
            if (nv_map_lookup(m->handle, &addr, &size) &&
                m->buffer_offset + (va - m->va) + len <= size) {
                p = (void *)(addr + m->buffer_offset + (va - m->va));
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return p;
}

static uint32_t as_map_buffer_ex(uint8_t *io, size_t n) {
    /* {u32 flags; u32 kind; u32 nvmap handle; u32 page size; u64 buffer offset;
     *  u64 mapping size; inout u64 gpu va} */
    if (n < 0x28) {
        return NV_BAD_PARAMETER;
    }
    uint32_t flags = rd32(io), kind = rd32(io + 4), handle = rd32(io + 8),
             page_size = rd32(io + 12);
    uint64_t buffer_offset = rd64(io + 16), size = rd64(io + 24), offset = rd64(io + 32);

    if (flags & 0x100) {
        /* Modify: change part of an existing mapping in place. With handle 0 only the
         * kind changes (deko3d does this to give an image its compressible kind). */
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_MAPPINGS; i++) {
            gpu_mapping *m = &g_maps[i];
            if (m->used && offset >= m->va && offset - m->va < m->size) {
                if (handle) {
                    m->handle = handle;
                    m->buffer_offset = buffer_offset - (offset - m->va);
                }
                m->kind = kind;
                pthread_mutex_unlock(&g_lock);
                HOS_LOGD(MOD, "remap gpu va %#" PRIx64 " size %#" PRIx64 " kind %#x", offset, size,
                         kind);
                return NV_OK;
            }
        }
        pthread_mutex_unlock(&g_lock);
        HOS_LOGW(MOD, "remap of gpu va %#" PRIx64 ": nothing mapped there", offset);
        return NV_BAD_PARAMETER;
    }

    uint64_t addr, nvsize;
    if (!nv_map_lookup(handle, &addr, &nvsize)) {
        HOS_LOGW(MOD, "map: nvmap handle %u not allocated", handle);
        return NV_BAD_PARAMETER;
    }
    if (size == 0) {
        size = nvsize - buffer_offset;
    }
    pthread_mutex_lock(&g_lock);
    uint64_t va = (flags & 1) ? offset : va_alloc(size, 0, page_size ? page_size : SMALL_PAGE);
    int slot = -1;
    for (int i = 0; i < MAX_MAPPINGS && va; i++) {
        if (!g_maps[i].used) {
            slot = i;
            break;
        }
    }
    if (!va || slot < 0) {
        pthread_mutex_unlock(&g_lock);
        return NV_INSUFFICIENT_MEM;
    }
    g_maps[slot] = (gpu_mapping){.used = 1,
                                 .va = va,
                                 .size = size,
                                 .handle = handle,
                                 .buffer_offset = buffer_offset,
                                 .kind = kind};
    pthread_mutex_unlock(&g_lock);
    wr64(io + 32, va);
    HOS_LOGD(MOD, "map handle %u +%#" PRIx64 " size %#" PRIx64 " kind %#x at gpu va %#" PRIx64,
             handle, buffer_offset, size, kind, va);
    return NV_OK;
}

static uint32_t as_unmap(uint8_t *io, size_t n) {
    if (n < 8) {
        return NV_BAD_PARAMETER;
    }
    uint64_t va = rd64(io);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (g_maps[i].used && g_maps[i].va == va) {
            g_maps[i].used = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NV_OK;
}

static uint32_t as_ioctl(uint32_t cmd, uint8_t *io, size_t n) {
    switch (IOC_NR(cmd)) {
    case 0x01: /* BIND_CHANNEL {u32 channel fd} */
    case 0x03: /* FREE_SPACE */
        return NV_OK;
    case 0x09: /* INITIALIZE_EX {u32 flags; s32 as_fd; u32 big_page_size; ...} */
        if (n >= 12 && rd32(io + 8) >= SMALL_PAGE && (rd32(io + 8) & (rd32(io + 8) - 1)) == 0) {
            pthread_mutex_lock(&g_lock);
            g_big_page = rd32(io + 8);
            pthread_mutex_unlock(&g_lock);
            HOS_LOGD(MOD, "address space: big pages of %#x", g_big_page);
        }
        return NV_OK;
    case 0x02: /* ALLOC_SPACE {u32 pages; u32 page_size; u32 flags; u32 pad; u64 offset/align} */ {
        if (n < 0x18) {
            return NV_BAD_PARAMETER;
        }
        uint32_t pages = rd32(io), page_size = rd32(io + 4), flags = rd32(io + 8);
        uint64_t size = (uint64_t)pages * page_size;
        uint64_t va = rd64(io + 16); /* fixed offset, or the alignment wanted */
        if (!(flags & 1)) {
            pthread_mutex_lock(&g_lock);
            va = va_alloc(size, va, page_size);
            pthread_mutex_unlock(&g_lock);
            if (!va) {
                return NV_INSUFFICIENT_MEM;
            }
        }
        wr64(io + 16, va);
        HOS_LOGD(MOD, "reserve %#" PRIx64 " bytes at gpu va %#" PRIx64, size, va);
        return NV_OK;
    }
    case 0x05: /* UNMAP_BUFFER {u64 offset} */
        return as_unmap(io, n);
    case 0x06: /* MAP_BUFFER_EX */
        return as_map_buffer_ex(io, n);
    case 0x08: /* GET_VA_REGIONS {u64 unused; u32 size; u32 pad; region[2]} */ {
        if (n < 0x40) {
            return NV_BAD_PARAMETER;
        }
        wr32(io + 8, 0x30);
        uint8_t *r = io + 0x10;
        wr64(r, VA_SMALL_BASE);
        wr32(r + 8, SMALL_PAGE);
        wr32(r + 12, 0);
        wr64(r + 16, (VA_BIG_BASE - VA_SMALL_BASE) / SMALL_PAGE);
        wr64(r + 24, VA_BIG_BASE);
        wr32(r + 32, BIG_PAGE);
        wr32(r + 36, 0);
        wr64(r + 40, (VA_END - VA_BIG_BASE) / BIG_PAGE);
        return NV_OK;
    }
    default:
        HOS_LOGW(MOD, "nvhost-as-gpu ioctl %#x (nr %#x, %u bytes) accepted as a no-op", cmd,
                 IOC_NR(cmd), IOC_SIZE(cmd));
        return NV_OK;
    }
}

/* ---- ctrl-gpu ------------------------------------------------------------------------- */

static void write_characteristics(uint8_t *c) {
    memset(c, 0, 0xA0);
    wr32(c + 0x00, 0x120);           /* arch: GM200 */
    wr32(c + 0x04, 0xB);             /* impl: GM20B */
    wr32(c + 0x08, 0xA1);            /* rev */
    wr32(c + 0x0C, 1);               /* num_gpc */
    wr64(c + 0x10, 0x40000);         /* L2 cache size */
    wr64(c + 0x18, 0);               /* on-board video memory */
    wr32(c + 0x20, 2);               /* num_tpc_per_gpc */
    wr32(c + 0x24, 0x20);            /* bus type: AXI */
    wr32(c + 0x28, BIG_PAGE);        /* big page size */
    wr32(c + 0x2C, BIG_PAGE);        /* compression page size */
    wr32(c + 0x30, 0x1B);            /* pde coverage bits */
    wr32(c + 0x34, 0x30000);         /* available big page sizes */
    wr32(c + 0x38, 1);               /* gpc mask */
    wr32(c + 0x3C, 0x503);           /* sm arch sm version */
    wr32(c + 0x40, 0x503);           /* sm arch spa version */
    wr32(c + 0x44, 0x80);            /* warp count */
    wr32(c + 0x48, 0x28);            /* gpu va bit count */
    wr64(c + 0x50, 0x55);            /* flags */
    wr32(c + 0x58, 0x902D);          /* 2D class */
    wr32(c + 0x5C, 0xB197);          /* 3D class */
    wr32(c + 0x60, 0xB1C0);          /* compute class */
    wr32(c + 0x64, 0xB06F);          /* GPFIFO class */
    wr32(c + 0x68, 0xA140);          /* inline-to-memory class */
    wr32(c + 0x6C, 0xB0B5);          /* DMA copy class */
    wr32(c + 0x70, 1);               /* max fbps */
    wr32(c + 0x78, 2);               /* max ltc per fbp */
    wr32(c + 0x7C, 1);               /* max lts per ltc */
    wr32(c + 0x84, 1);               /* max gpc count */
    wr32(c + 0x88, 0x21D70);         /* rop l2 en mask 0 */
    wr64(c + 0x90, 0x6230326D67ull); /* "gm20b" */
}

static uint32_t ctrl_gpu_ioctl(uint32_t cmd, uint8_t *io, size_t n, uint8_t *extra_out,
                               size_t extra_n) {
    switch (IOC_NR(cmd)) {
    case 0x01: /* ZCULL_GET_CTX_SIZE -> u32 */
        if (n < 4) {
            return NV_BAD_PARAMETER;
        }
        wr32(io, 0x9900);
        return NV_OK;
    case 0x02: /* ZCULL_GET_INFO */ {
        static const uint32_t info[10] = {0x20, 0x20, 0x400, 0x800, 0x20,
                                          0x20, 0xC0, 0x20,  0x40,  0x10};
        if (n < sizeof(info)) {
            return NV_BAD_PARAMETER;
        }
        memcpy(io, info, sizeof(info));
        return NV_OK;
    }
    case 0x05: /* GET_CHARACTERISTICS {u64 size; u64 addr; characteristics} */
        if (n < 0xB0) {
            return NV_BAD_PARAMETER;
        }
        wr64(io, 0xA0);
        write_characteristics(io + 0x10);
        if (extra_out && extra_n >= 0xA0) {
            write_characteristics(extra_out);
        }
        return NV_OK;
    case 0x06: /* GET_TPC_MASKS {u32 size; u32 pad[3]; u64 addr} -> tpc mask */
        if (n >= 0x18) {
            wr32(io + 0x10, 0x3);
        }
        if (extra_out && extra_n >= 4) {
            wr32(extra_out, 0x3);
        }
        return NV_OK;
    case 0x14: /* ZBC_GET_ACTIVE_SLOT_MASK */
        if (n >= 8) {
            wr32(io, 0x07);
            wr32(io + 4, 0);
        }
        return NV_OK;
    case 0x1C: /* GET_GPU_TIME -> u64 ns */ {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (n >= 8) {
            wr64(io, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
        }
        return NV_OK;
    }
    case 0x03: /* ZBC_SET_TABLE */
    case 0x04: /* ZBC_QUERY_TABLE */
        return NV_OK;
    default:
        HOS_LOGW(MOD, "nvhost-ctrl-gpu ioctl %#x (nr %#x, %u bytes) accepted as a no-op", cmd,
                 IOC_NR(cmd), IOC_SIZE(cmd));
        return NV_OK;
    }
}

/* ---- channels ------------------------------------------------------------------------- */

/* Host-method state; the engines (3D, DMA, ...) keep theirs in gpu_engines. */
typedef struct sem_regs {
    uint64_t addr;
    uint32_t payload;
} sem_regs;

typedef struct gpu_channel {
    int used;
    uint32_t fd;
    uint32_t syncpt;
    uint64_t submits;
    sem_regs host_sem; /* GPFIFO class 0xB06F, methods 0x04-0x07 */
    uint32_t host_syncpt_payload;
    uint64_t host_releases, syncpt_incrs;
    gpu_engines *engines;
} gpu_channel;

static gpu_channel g_channels[MAX_CHANNELS];
static int g_trace_methods = -1; /* HOS_GPU_TRACE=1: log every method write at trace level */

static gpu_channel *channel_for(uint32_t fd) {
    pthread_mutex_lock(&g_lock);
    gpu_channel *free_slot = NULL;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].used && g_channels[i].fd == fd) {
            pthread_mutex_unlock(&g_lock);
            return &g_channels[i];
        }
        if (!g_channels[i].used && !free_slot) {
            free_slot = &g_channels[i];
        }
    }
    if (free_slot && g_next_syncpt < MAX_SYNCPTS) {
        gpu_engines *eng = gpu_engines_new();
        if (!eng) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        *free_slot = (gpu_channel){.used = 1, .fd = fd, .syncpt = g_next_syncpt++, .engines = eng};
        HOS_LOGD(MOD, "channel fd %u uses syncpoint %u", fd, free_slot->syncpt);
    } else {
        free_slot = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return free_slot;
}

void nv_gpu_channel_close(uint32_t fd) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].used && g_channels[i].fd == fd) {
            gpu_engines_free(g_channels[i].engines);
            g_channels[i].engines = NULL;
            g_channels[i].used = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void syncpt_incr(gpu_channel *ch, uint32_t id) {
    pthread_mutex_lock(&g_lock);
    if (id < MAX_SYNCPTS) {
        g_syncpt[id]++;
    }
    pthread_mutex_unlock(&g_lock);
    ch->syncpt_incrs++;
}

/* One method write: host methods (< 0x40) here, everything else to the engines. */
static void method_write(gpu_channel *ch, uint32_t subch, uint32_t method, uint32_t v) {
    if (g_trace_methods) {
        HOS_LOGT(MOD, "  subch %u method %#05x = %#010x", subch, method, v);
    }
    if (method == 0) { /* SET_OBJECT */
        gpu_engines_bind(ch->engines, subch, v & 0xFFFF);
        return;
    }
    if (method >= 0x40) {
        gpu_engines_method(ch->engines, subch, method, v);
        return;
    }
    switch (method) {
    case 0x04:
        ch->host_sem.addr = (ch->host_sem.addr & 0xFFFFFFFFull) | ((uint64_t)(v & 0xFF) << 32);
        break;
    case 0x05:
        ch->host_sem.addr = (ch->host_sem.addr & ~0xFFFFFFFFull) | (v & ~3u);
        break;
    case 0x06:
        ch->host_sem.payload = v;
        break;
    case 0x07: /* SEMAPHORED: [2:0] operation (1 acquire, 2 release, 4 acquire >=), [24] 4-byte */
        if ((v & 7) == 2 &&
            gpu_semaphore_release(ch->host_sem.addr, ch->host_sem.payload, !((v >> 24) & 1)) == 0) {
            ch->host_releases++;
        }
        break;
    case 0x1C:
        ch->host_syncpt_payload = v;
        break;
    case 0x1D: /* SYNCPOINTB: [0] increment, [19:8] syncpoint index */
        if (v & 1) {
            syncpt_incr(ch, (v >> 8) & 0xFFF);
        }
        break;
    default:
        break;
    }
}

/*
 * Run one pushbuffer segment. Method headers are type[31:29]
 * count/immediate[28:16] subchannel[15:13] method[12:0]: 1 incrementing,
 * 3 non-incrementing, 4 immediate (the data is the count field), 5 increment
 * once; 0 and 2 (tertiary-op forms) are counted like 1. Returns the number of
 * headers, or -1 if the segment does not parse.
 */
static long run_pushbuffer(gpu_channel *ch, const uint32_t *w, uint32_t words) {
    long headers = 0;
    uint32_t i = 0;
    while (i < words) {
        uint32_t h = w[i++];
        uint32_t type = h >> 29, count = (h >> 16) & 0x1FFF;
        uint32_t subch = (h >> 13) & 7, method = h & 0x1FFF;
        headers++;
        if (type == 4) {
            method_write(ch, subch, method, count);
            continue;
        }
        if (type == 0 || type == 2) {
            /* Grp0/Grp2 "use tertiary": counted like an incrementing header; the all-zero
             * word deko3d pads segments with is one of these with no data (a NOP). */
            type = 1;
        }
        if (type != 1 && type != 3 && type != 5) {
            return -1;
        }
        if (count > words - i) {
            return -1;
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t m = type == 1 ? method + k : type == 5 ? method + (k ? 1 : 0) : method;
            method_write(ch, subch, m, w[i + k]);
        }
        i += count;
    }
    return headers;
}

/* The M1 command processor goes here; M0 checks the stream and signals at once. */
static void gpu_submit(gpu_channel *ch, const uint64_t *entries, uint32_t num) {
    uint64_t words = 0;
    long headers = 0;
    int bad = 0;
    for (uint32_t i = 0; i < num; i++) {
        uint64_t e = entries[i];
        uint64_t va = e & 0xFFFFFFFFFCull;
        uint32_t len = (uint32_t)((e >> 42) & 0x1FFFFF);
        if (len == 0) {
            continue; /* control entry */
        }
        const uint32_t *pb = nv_gpu_va_to_host(va, (uint64_t)len * 4);
        if (!pb) {
            if (!bad++) {
                HOS_LOGW(MOD,
                         "submit: gpfifo entry %u at gpu va %#" PRIx64 " (%u words) is not mapped",
                         i, va, len);
            }
            continue;
        }
        long h = run_pushbuffer(ch, pb, len);
        if (h < 0 && !bad++) {
            HOS_LOGW(MOD, "submit: pushbuffer at gpu va %#" PRIx64 " does not parse", va);
        }
        headers += h > 0 ? h : 0;
        words += len;
    }
    gpu_engines_flush(ch->engines);
    ch->submits++;
    if (ch->submits <= 3 || ch->submits % 100 == 0) {
        const gpu_engine_stats *st = gpu_engines_stats(ch->engines);
        HOS_LOGI(MOD,
                 "submit %" PRIu64 " on channel fd %u: %u entries, %" PRIu64
                 " words, %ld method headers; so far %" PRIu64 " macros, %" PRIu64
                 " clears, %" PRIu64 " copies, %" PRIu64 " inline uploads, %" PRIu64
                 " semaphore releases%s",
                 ch->submits, ch->fd, num, words, headers, st->macros, st->clears, st->copies,
                 st->inline_uploads, st->releases + ch->host_releases, bad ? ", with errors" : "");
    }
}

static int trace_methods_enabled(void) {
    if (g_trace_methods < 0) {
        const char *t = getenv("HOS_GPU_TRACE");
        g_trace_methods = t && *t && strcmp(t, "0") != 0;
    }
    return g_trace_methods;
}

static uint32_t channel_submit(gpu_channel *ch, uint8_t *io, size_t n, const uint8_t *extra_in,
                               size_t extra_n) {
    /* {u64 gpfifo; u32 num_entries; u32 flags; fence {u32 id; u32 value}} + inline entries */
    if (n < 0x18) {
        return NV_BAD_PARAMETER;
    }
    uint32_t num = rd32(io + 8), flags = rd32(io + 12);
    trace_methods_enabled();
    const uint8_t *src = NULL;
    if (extra_in && extra_n >= (size_t)num * 8) {
        src = extra_in;
    } else if (n >= 0x18 + (size_t)num * 8) {
        src = io + 0x18;
    } else {
        HOS_LOGW(MOD, "submit: %u entries but no room for them", num);
        return NV_BAD_PARAMETER;
    }
    uint64_t entries[256];
    uint32_t done = 0;
    while (done < num) {
        uint32_t chunk = num - done < 256 ? num - done : 256;
        memcpy(entries, src + (size_t)done * 8, (size_t)chunk * 8);
        gpu_submit(ch, entries, chunk);
        done += chunk;
    }
    /* FENCE_GET (bit 1): the kernel appends a syncpoint increment and returns its value. */
    pthread_mutex_lock(&g_lock);
    uint32_t value = g_syncpt[ch->syncpt] += (flags & 2) ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    wr32(io + 16, ch->syncpt);
    wr32(io + 20, value);
    return NV_OK;
}

static uint32_t channel_ioctl(uint32_t fd, uint32_t cmd, uint8_t *io, size_t n,
                              const uint8_t *extra_in, size_t extra_n) {
    gpu_channel *ch = channel_for(fd);
    if (!ch) {
        return NV_INSUFFICIENT_MEM;
    }
    switch (IOC_NR(cmd)) {
    case 0x08: /* SUBMIT_GPFIFO (entries inline or, with Ioctl2, in the second buffer) */
    case 0x1B: /* SUBMIT_GPFIFO_EX: same header, what deko3d uses */
        return channel_submit(ch, io, n, extra_in, extra_n);
    case 0x09: /* ALLOC_OBJ_CTX {u32 class; u32 flags; out u64 obj_id} */
        if (n < 0x10) {
            return NV_BAD_PARAMETER;
        }
        HOS_LOGD(MOD, "channel fd %u: object class %#x", fd, rd32(io));
        wr64(io + 8, 0x1000 + rd32(io));
        return NV_OK;
    case 0x1A: /* ALLOC_GPFIFO_EX2 {u32 entries; u32 flags; u32 unk; out fence; u32 unk[3]} */
        if (n < 0x14) {
            return NV_BAD_PARAMETER;
        }
        HOS_LOGD(MOD, "channel fd %u: gpfifo of %u entries", fd, rd32(io));
        wr32(io + 12, ch->syncpt);
        wr32(io + 16, nv_syncpt_read(ch->syncpt));
        return NV_OK;
    case 0x01: /* SET_NVMAP_FD */
    case 0x03: /* SET_TIMEOUT */
    case 0x0B: /* ZCULL_BIND */
    case 0x0C: /* SET_ERROR_NOTIFIER */
    case 0x0D: /* SET_PRIORITY */
    case 0x14: /* SET_USER_DATA */
    case 0x1D: /* SET_TIMESLICE */
        return NV_OK;
    case 0x15: /* GET_USER_DATA */
    case 0x16: /* GET_ERROR_NOTIFICATION */
        memset(io, 0, n);
        return NV_OK;
    default:
        HOS_LOGW(MOD, "nvhost-gpu ioctl %#x (nr %#x, %u bytes) accepted as a no-op", cmd,
                 IOC_NR(cmd), IOC_SIZE(cmd));
        return NV_OK;
    }
}

uint32_t nv_gpu_ioctl(int device, uint32_t fd, uint32_t cmd, uint8_t *io, size_t n,
                      const uint8_t *extra_in, size_t extra_in_n, uint8_t *extra_out,
                      size_t extra_out_n) {
    if (hos_log_get_level() >= HOS_LOG_TRACE) {
        char hex[3 * 48 + 1];
        size_t k = 0;
        for (size_t i = 0; i < n && i < 48; i++) {
            k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02x ", io[i]);
        }
        hex[k] = 0;
        HOS_LOGT(MOD, "dev %d fd %u ioctl %#x nr %#x size %u in2 %zu out2 %zu: %s", device, fd, cmd,
                 IOC_NR(cmd), IOC_SIZE(cmd), extra_in_n, extra_out_n, hex);
    }
    switch (device) {
    case NV_GPU_DEV_CTRL:
        return ctrl_gpu_ioctl(cmd, io, n, extra_out, extra_out_n);
    case NV_GPU_DEV_AS:
        return as_ioctl(cmd, io, n);
    case NV_GPU_DEV_CHANNEL:
        return channel_ioctl(fd, cmd, io, n, extra_in, extra_in_n);
    default:
        return NV_BAD_PARAMETER;
    }
}
