/*
 * nvdrv: the NVIDIA driver front end. Only the devices a framebuffer client
 * needs are modelled: /dev/nvmap allocates "GPU memory" that is simply the
 * game's own buffer, and /dev/nvhost-ctrl answers sync point queries with
 * "already signalled". There is no GPU behind this; command submission to
 * /dev/nvhost-gpu is reported as unsupported.
 */

#include <pthread.h>
#include <string.h>

#include "common/hos_log.h"
#include "gpu_internal.h"

#define MOD "nvdrv"

#define NV_FD_MAX  64
#define NV_MAP_MAX 1024

/* NvError values returned through the ioctl result word. */
enum {
    NV_OK = 0,
    NV_NOT_IMPLEMENTED = 1,
    NV_NOT_SUPPORTED = 2,
    NV_BAD_PARAMETER = 4,
    NV_INVALID_STATE = 8,
    NV_INVALID_SIZE = 10,
    NV_FILE_OPERATION_FAILED = 0x30003,
    NV_IOCTL_FAILED = 0x30010,
};

typedef enum nv_device {
    NV_DEV_NONE = 0,
    NV_DEV_NVMAP,
    NV_DEV_HOST_CTRL,
    NV_DEV_HOST_CTRL_GPU,
    NV_DEV_HOST_AS_GPU,
    NV_DEV_HOST_GPU,
    NV_DEV_DISP0,
    NV_DEV_DISP1,
} nv_device;

typedef struct nv_fd {
    nv_device dev;
} nv_fd;

typedef struct nv_map {
    int used;
    int allocated;
    uint32_t size;
    uint32_t align;
    uint32_t kind;
    uint64_t addr;
} nv_map;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nv_fd g_fds[NV_FD_MAX];
static nv_map g_maps[NV_MAP_MAX];

static const struct {
    const char *path;
    nv_device dev;
} k_devices[] = {
    {"/dev/nvmap", NV_DEV_NVMAP},
    {"/dev/nvhost-ctrl", NV_DEV_HOST_CTRL},
    {"/dev/nvhost-ctrl-gpu", NV_DEV_HOST_CTRL_GPU},
    {"/dev/nvhost-as-gpu", NV_DEV_HOST_AS_GPU},
    {"/dev/nvhost-gpu", NV_DEV_HOST_GPU},
    {"/dev/nvdisp-disp0", NV_DEV_DISP0},
    {"/dev/nvdisp-disp1", NV_DEV_DISP1},
};

int nv_map_lookup(uint32_t id, uint64_t *addr, uint64_t *size) {
    if (id == 0 || id > NV_MAP_MAX) {
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    nv_map *m = &g_maps[id - 1];
    int ok = m->used && m->allocated;
    if (ok) {
        *addr = m->addr;
        *size = m->size;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ---- ioctls --------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static uint32_t nvmap_ioctl(uint32_t cmd, uint8_t *io, size_t n) {
    switch (cmd) {
    case 0xC0080101: /* NVMAP_IOC_CREATE {u32 size; out u32 handle} */ {
        if (n < 8) {
            return NV_BAD_PARAMETER;
        }
        uint32_t size = rd32(io);
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < NV_MAP_MAX; i++) {
            if (!g_maps[i].used) {
                memset(&g_maps[i], 0, sizeof(g_maps[i]));
                g_maps[i].used = 1;
                g_maps[i].size = size;
                pthread_mutex_unlock(&g_lock);
                wr32(io + 4, i + 1);
                HOS_LOGD(MOD, "nvmap create handle %u size %#x", i + 1, size);
                return NV_OK;
            }
        }
        pthread_mutex_unlock(&g_lock);
        return NV_INVALID_STATE;
    }
    case 0xC0080103: /* NVMAP_IOC_FROM_ID {u32 id; out u32 handle} */
        if (n < 8) {
            return NV_BAD_PARAMETER;
        }
        wr32(io + 4, rd32(io));
        return NV_OK;
    case 0xC0200104: /* NVMAP_IOC_ALLOC {u32 handle; u32 heapmask; u32 flags; u32 align;
                        u8 kind; u8 pad[7]; u64 addr} */
    {
        if (n < 0x20) {
            return NV_BAD_PARAMETER;
        }
        uint32_t handle = rd32(io);
        if (handle == 0 || handle > NV_MAP_MAX) {
            return NV_BAD_PARAMETER;
        }
        pthread_mutex_lock(&g_lock);
        nv_map *m = &g_maps[handle - 1];
        if (!m->used) {
            pthread_mutex_unlock(&g_lock);
            return NV_BAD_PARAMETER;
        }
        m->align = rd32(io + 12);
        m->kind = io[16];
        memcpy(&m->addr, io + 24, 8);
        m->allocated = 1;
        pthread_mutex_unlock(&g_lock);
        HOS_LOGD(MOD, "nvmap alloc handle %u at %#llx kind %u", handle, (unsigned long long)m->addr,
                 m->kind);
        return NV_OK;
    }
    case 0xC0180105: /* NVMAP_IOC_FREE {u32 handle; u32 pad; out u64 addr; out u32 size;
                        out u32 flags} */
    {
        if (n < 0x18) {
            return NV_BAD_PARAMETER;
        }
        uint32_t handle = rd32(io);
        if (handle == 0 || handle > NV_MAP_MAX) {
            return NV_BAD_PARAMETER;
        }
        pthread_mutex_lock(&g_lock);
        nv_map *m = &g_maps[handle - 1];
        memcpy(io + 8, &m->addr, 8);
        wr32(io + 16, m->size);
        wr32(io + 20, 0);
        memset(m, 0, sizeof(*m));
        pthread_mutex_unlock(&g_lock);
        return NV_OK;
    }
    case 0xC00C0109: /* NVMAP_IOC_PARAM {u32 handle; u32 param; out u32 result} */ {
        if (n < 12) {
            return NV_BAD_PARAMETER;
        }
        uint32_t handle = rd32(io), param = rd32(io + 4);
        if (handle == 0 || handle > NV_MAP_MAX || !g_maps[handle - 1].used) {
            return NV_BAD_PARAMETER;
        }
        nv_map *m = &g_maps[handle - 1];
        uint32_t v = 0;
        switch (param) {
        case 1: /* size */
            v = m->size;
            break;
        case 2: /* alignment */
            v = m->align;
            break;
        case 4: /* heap */
            v = 0x40000000;
            break;
        case 5: /* kind */
            v = m->kind;
            break;
        default:
            return NV_BAD_PARAMETER;
        }
        wr32(io + 8, v);
        return NV_OK;
    }
    case 0xC008010E: /* NVMAP_IOC_GET_ID {out u32 id; u32 handle} */
        if (n < 8) {
            return NV_BAD_PARAMETER;
        }
        wr32(io, rd32(io + 4));
        return NV_OK;
    default:
        HOS_LOGW(MOD, "nvmap ioctl %#x not implemented", cmd);
        return NV_NOT_IMPLEMENTED;
    }
}

static uint32_t host_ctrl_ioctl(uint32_t cmd, uint8_t *io, size_t n) {
    switch (cmd) {
    case 0xC0080014: /* SYNCPT_READ {u32 id; out u32 value} */
    case 0xC008001A: /* SYNCPT_READ_MAX */
        if (n < 8) {
            return NV_BAD_PARAMETER;
        }
        wr32(io + 4, 0);
        return NV_OK;
    case 0x40040015: /* SYNCPT_INCR */
    case 0xC00C0016: /* SYNCPT_WAIT {u32 id; u32 thresh; s32 timeout} */
    case 0xC0100019: /* SYNCPT_WAITEX */
    case 0xC004001C: /* EVENT_SIGNAL */
    case 0xC004001F: /* EVENT_REGISTER */
    case 0xC0040020: /* EVENT_UNREGISTER */
    case 0x40080021: /* EVENT_KILL */
        return NV_OK;
    case 0xC010001D: /* EVENT_WAIT {u32 id; u32 thresh; s32 timeout; out u32 value} */
    case 0xC010001E: /* EVENT_WAIT_ASYNC */
        if (n < 16) {
            return NV_BAD_PARAMETER;
        }
        wr32(io + 12, rd32(io + 4)); /* already at the threshold */
        return NV_OK;
    default:
        HOS_LOGW(MOD, "nvhost-ctrl ioctl %#x not implemented", cmd);
        return NV_NOT_IMPLEMENTED;
    }
}

static uint32_t nv_ioctl(nv_device dev, uint32_t cmd, uint8_t *io, size_t n) {
    switch (dev) {
    case NV_DEV_NVMAP:
        return nvmap_ioctl(cmd, io, n);
    case NV_DEV_HOST_CTRL:
        return host_ctrl_ioctl(cmd, io, n);
    default:
        /* FIXME(#24): nvhost-gpu, nvhost-as-gpu and nvhost-ctrl-gpu need a GPU. */
        HOS_LOGW(MOD, "ioctl %#x on device %d: no GPU behind nvdrv", cmd, dev);
        return NV_NOT_SUPPORTED;
    }
}

/* ---- the service -------------------------------------------------------------- */

static hos_result out_error(hos_ipc_ctx *ctx, uint32_t err) {
    hos_ipc_out_write(ctx, &err, 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result cmd_open(hos_ipc_ctx *ctx) {
    const hos_ipc_buffer *b = gpu_in_buffer(ctx, 0);
    char path[64];
    if (!b || !b->addr || b->size == 0) {
        return HOS_KERNEL_INVALID_POINTER;
    }
    size_t n = b->size < sizeof(path) - 1 ? (size_t)b->size : sizeof(path) - 1;
    memcpy(path, (const void *)b->addr, n);
    path[n] = '\0';
    nv_device dev = NV_DEV_NONE;
    for (size_t i = 0; i < sizeof(k_devices) / sizeof(k_devices[0]); i++) {
        if (strcmp(path, k_devices[i].path) == 0) {
            dev = k_devices[i].dev;
        }
    }
    uint32_t out[2] = {0, NV_FILE_OPERATION_FAILED}; /* fd, error */
    if (dev == NV_DEV_NONE) {
        HOS_LOGW(MOD, "open %s: no such device", path);
        hos_ipc_out_write(ctx, out, 8);
        return HOS_RESULT_SUCCESS;
    }
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 1; i < NV_FD_MAX; i++) {
        if (g_fds[i].dev == NV_DEV_NONE) {
            g_fds[i].dev = dev;
            out[0] = i;
            out[1] = NV_OK;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    HOS_LOGD(MOD, "open %s -> fd %u", path, out[0]);
    hos_ipc_out_write(ctx, out, 8);
    return HOS_RESULT_SUCCESS;
}

static nv_device fd_device(uint32_t fd) {
    if (fd == 0 || fd >= NV_FD_MAX) {
        return NV_DEV_NONE;
    }
    return g_fds[fd].dev;
}

static hos_result cmd_ioctl(hos_ipc_ctx *ctx) {
    uint32_t fd = 0, cmd = 0;
    hos_ipc_in_read(ctx, 0, &fd, 4);
    hos_ipc_in_read(ctx, 4, &cmd, 4);
    nv_device dev = fd_device(fd);
    if (dev == NV_DEV_NONE) {
        return out_error(ctx, NV_BAD_PARAMETER);
    }
    const hos_ipc_buffer *in = gpu_in_buffer(ctx, 0);
    const hos_ipc_buffer *out = gpu_out_buffer(ctx, 0);
    uint8_t io[0x200];
    size_t n = 0;
    memset(io, 0, sizeof(io));
    if (in && in->addr && in->size) {
        n = in->size < sizeof(io) ? (size_t)in->size : sizeof(io);
        memcpy(io, (const void *)in->addr, n);
    }
    if (out && out->size > n) {
        n = out->size < sizeof(io) ? (size_t)out->size : sizeof(io);
    }
    uint32_t err = nv_ioctl(dev, cmd, io, n);
    if (out && out->addr && out->size) {
        size_t w = out->size < sizeof(io) ? (size_t)out->size : sizeof(io);
        memcpy((void *)out->addr, io, w);
    }
    return out_error(ctx, err);
}

static hos_result nvdrv_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* Open(path) -> fd, error */
        return cmd_open(ctx);
    case 1:  /* Ioctl(fd, cmd, in, out) -> error */
    case 11: /* Ioctl2 */
    case 12: /* Ioctl3 */
        return cmd_ioctl(ctx);
    case 2: /* Close(fd) -> error */ {
        uint32_t fd = 0;
        hos_ipc_in_read(ctx, 0, &fd, 4);
        if (fd_device(fd) == NV_DEV_NONE) {
            return out_error(ctx, NV_BAD_PARAMETER);
        }
        pthread_mutex_lock(&g_lock);
        g_fds[fd].dev = NV_DEV_NONE;
        pthread_mutex_unlock(&g_lock);
        return out_error(ctx, NV_OK);
    }
    case 3: /* Initialize(transfer mem size, handles) -> error */
    case 8: /* SetAruid */
        return out_error(ctx, NV_OK);
    case 4: /* QueryEvent(fd, event id) -> error, copy handle */ {
        void *ev;
        uint32_t h = hos_ipc_event_create(&ev);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        hos_ipc_event_signal(ev);
        ctx->copy_handles[ctx->num_copy++] = h;
        return out_error(ctx, NV_OK);
    }
    case 5:  /* DumpGraphicsMemoryInfo */
    case 13: /* SetGraphicsFirmwareMemoryMarginEnabled */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_nvdrv_class = {
    .name = "nvdrv",
    .dispatch = nvdrv_dispatch,
    .destroy = NULL,
};
