/*
 * vi: the display service, with the Android binder/IGraphicBufferProducer
 * protocol the runtime uses to hand frames to the compositor. There is no
 * screen: a queued buffer is consumed on the spot (the "null compositor"),
 * and when HOS_FRAMES names a directory each frame is deswizzled from the
 * game's memory and written there as a PPM image.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "display_x11.h"
#include "gpu_internal.h"
#include "parcel.h"
#include "swizzle.h"

#define MOD "vi"

#define VI_MAX_LAYERS       8
#define BQ_SLOTS            16
#define GRAPHIC_BUFFER_SIZE 0x16C
#define DISPLAY_WIDTH       1280
#define DISPLAY_HEIGHT      720
#define FB_BLOCK_HEIGHT     16

/* IGraphicBufferProducer transaction codes. */
enum {
    BQ_REQUEST_BUFFER = 1,
    BQ_SET_BUFFER_COUNT = 2,
    BQ_DEQUEUE_BUFFER = 3,
    BQ_DETACH_BUFFER = 4,
    BQ_DETACH_NEXT_BUFFER = 5,
    BQ_ATTACH_BUFFER = 6,
    BQ_QUEUE_BUFFER = 7,
    BQ_CANCEL_BUFFER = 8,
    BQ_QUERY = 9,
    BQ_CONNECT = 10,
    BQ_DISCONNECT = 11,
    BQ_ALLOCATE_BUFFERS = 13,
    BQ_SET_PREALLOCATED_BUFFER = 14,
};

enum slot_state { SLOT_EMPTY = 0, SLOT_FREE, SLOT_DEQUEUED, SLOT_QUEUED };

typedef struct bq_slot {
    int state;
    uint8_t buffer[GRAPHIC_BUFFER_SIZE];
} bq_slot;

typedef struct vi_layer {
    int used;
    uint64_t id;
    bq_slot slots[BQ_SLOTS];
    int connected;
    void *buffer_event;
    uint32_t buffer_event_handle;
    uint32_t frames;
} vi_layer;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vi_layer g_layers[VI_MAX_LAYERS]; /* binder id = index + 1 */
static uint64_t g_next_layer_id = 1;

static void *g_vsync_event;
static uint32_t g_vsync_handle;
static pthread_t g_vsync_thread;

static uint8_t *g_linear;
static size_t g_linear_size;
static uint32_t g_frame_counter;

/* ---- frame dump ------------------------------------------------------------------ */

static uint32_t gb_u32(const uint8_t *gb, size_t off) {
    uint32_t v;
    memcpy(&v, gb + off, 4);
    return v;
}

/*
 * The graphic buffer as it is flattened into the parcel: the Android
 * GraphicBuffer header (magic, width, height, stride, format, usage, pid,
 * refcount, fd count, int count: 10 words) followed by the libnx
 * NvGraphicBuffer body without its NativeHandle header. So nvmap id sits at
 * 0x2C, the pixel format at 0x44, and plane 0 at 0x5C with width, height,
 * layout (1 pitch, 3 block linear), pitch in bytes, offset and block height.
 */
#define GB_NVMAP_ID            0x2C
#define GB_FORMAT              0x44
#define GB_PLANE_WIDTH         0x5C
#define GB_PLANE_HEIGHT        0x60
#define GB_PLANE_LAYOUT        0x6C /* color_format before it is a 64-bit enum */
#define GB_PLANE_PITCH         0x70
#define GB_PLANE_OFFSET        0x78
#define GB_PLANE_BLOCK_LOG2    0x80
#define NV_LAYOUT_PITCH        1
#define NV_LAYOUT_BLOCK_LINEAR 3

static uint8_t *g_rgb;
static size_t g_rgb_size;

/*
 * Live view: the frame as a bottom-up 24-bit BMP, written next to `path` and
 * renamed into place so a viewer never sees a partial file.
 */
static void write_bmp(const char *path, const uint8_t *rgb, uint32_t width, uint32_t height) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    uint32_t row = (width * 3 + 3) & ~3u;
    uint32_t image = row * height;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    uint32_t v = 54 + image;
    memcpy(hdr + 2, &v, 4);
    v = 54;
    memcpy(hdr + 10, &v, 4);
    v = 40;
    memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &width, 4);
    memcpy(hdr + 22, &height, 4);
    uint16_t planes = 1, bits = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bits, 2);
    memcpy(hdr + 34, &image, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t pad[3] = {0, 0, 0};
    for (uint32_t y = height; y > 0; y--) {
        const uint8_t *src = rgb + (size_t)(y - 1) * width * 3;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t bgr[3] = {src[x * 3 + 2], src[x * 3 + 1], src[x * 3]};
            fwrite(bgr, 1, 3, f);
        }
        fwrite(pad, 1, row - width * 3, f);
    }
    fclose(f);
    rename(tmp, path);
}

/* At exit: how many frames the program presented (tools count these). */
static void report_frames(void) {
    if (g_frame_counter) {
        HOS_LOGI(MOD, "frames presented: %u", g_frame_counter);
    }
}

static void dump_frame(vi_layer *layer, const uint8_t *gb) {
    static int registered;
    if (!registered) {
        registered = 1;
        atexit(report_frames);
    }
    uint32_t width = gb_u32(gb, GB_PLANE_WIDTH), height = gb_u32(gb, GB_PLANE_HEIGHT);
    uint32_t stride = gb_u32(gb, GB_PLANE_PITCH), layout = gb_u32(gb, GB_PLANE_LAYOUT);
    uint32_t block_height = 1u << (gb_u32(gb, GB_PLANE_BLOCK_LOG2) & 7);
    uint32_t format = gb_u32(gb, GB_FORMAT);
    uint32_t nvmap_id = gb_u32(gb, GB_NVMAP_ID), offset = gb_u32(gb, GB_PLANE_OFFSET);
    uint64_t addr, size;

    layer->frames++;
    g_frame_counter++;
    if (!nv_map_lookup(nvmap_id, &addr, &size)) {
        HOS_LOGW(MOD, "frame %u: nvmap handle %u unknown", g_frame_counter, nvmap_id);
        return;
    }
    if (width == 0 || height == 0 || width > 8192 || height > 8192 || offset > size) {
        HOS_LOGW(MOD, "frame %u: bad geometry %ux%u", g_frame_counter, width, height);
        return;
    }
    /* Android pixel formats: 1 RGBA8888, 2 RGBX8888, 3 RGB888, 4 RGB565, 5 BGRA8888 */
    uint32_t bpp = (format == 4) ? 2 : (format == 3) ? 3 : 4;
    uint32_t row_bytes = width * bpp;
    size_t need = (size_t)row_bytes * height;
    if (need > g_linear_size) {
        if (g_linear) {
            munmap(g_linear, g_linear_size);
        }
        g_linear = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_linear == MAP_FAILED) {
            g_linear = NULL;
            g_linear_size = 0;
            return;
        }
        g_linear_size = need;
    }
    const uint8_t *pixels = (const uint8_t *)(addr + offset);
    size_t avail = (size_t)(size - offset);
    if (layout == NV_LAYOUT_PITCH) {
        if ((uint64_t)stride * height > avail) {
            HOS_LOGW(MOD, "frame %u: pitch surface exceeds its nvmap buffer", g_frame_counter);
            return;
        }
        for (uint32_t y = 0; y < height; y++) {
            memcpy(g_linear + (size_t)y * row_bytes, pixels + (size_t)y * stride, row_bytes);
        }
    } else if (swizzle_decode(pixels, avail, g_linear, row_bytes, height, stride, block_height) !=
               0) {
        HOS_LOGW(MOD, "frame %u: cannot deswizzle (layout %u, stride %u, block height %u)",
                 g_frame_counter, layout, stride, block_height);
        return;
    }
    /* Convert to 8-bit RGB once; the dumps and the live view share it. */
    size_t rgb_size = (size_t)width * height * 3;
    if (rgb_size > g_rgb_size) {
        if (g_rgb) {
            munmap(g_rgb, g_rgb_size);
        }
        g_rgb = mmap(NULL, rgb_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_rgb == MAP_FAILED) {
            g_rgb = NULL;
            g_rgb_size = 0;
            return;
        }
        g_rgb_size = rgb_size;
    }
    size_t lit = 0;
    uint8_t *out = g_rgb;
    for (size_t i = 0; i < need; i += bpp, out += 3) {
        const uint8_t *p = g_linear + i;
        if (bpp == 2) {
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            out[0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            out[1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
            out[2] = (uint8_t)((v & 0x1F) << 3);
        } else if (format == 5) {
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
        } else {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
        }
        if (out[0] | out[1] | out[2]) {
            lit++;
        }
    }

    /* The window (HOS_WINDOW=1) opens on the first frame; see display_x11.h. */
    display_x11_present(g_rgb, width, height);

    const char *dir = getenv("HOS_FRAMES");
    const char *every_s = getenv("HOS_FRAMES_EVERY");
    unsigned every = every_s ? (unsigned)strtoul(every_s, NULL, 10) : 1;
    if (dir && *dir && every && (g_frame_counter % every) == 0) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/frame_%04u.ppm", dir, g_frame_counter);
        FILE *f = fopen(path, "wb");
        if (!f) {
            HOS_LOGW(MOD, "cannot write %s", path);
        } else {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            fwrite(g_rgb, 1, rgb_size, f);
            fclose(f);
        }
    }
    const char *live = getenv("HOS_FRAMES_LIVE");
    if (live && *live) {
        write_bmp(live, g_rgb, width, height);
    }
    int noisy = g_frame_counter <= 10 || (g_frame_counter % 100) == 0;
    hos_log(noisy ? HOS_LOG_INFO : HOS_LOG_DEBUG, MOD,
            "frame %u: %ux%u format %u, %zu non-black pixels", g_frame_counter, width, height,
            format, lit);
}

/* ---- buffer queue ------------------------------------------------------------------ */

static vi_layer *layer_from_binder(int32_t id) {
    if (id < 1 || id > VI_MAX_LAYERS || !g_layers[id - 1].used) {
        return NULL;
    }
    return &g_layers[id - 1];
}

static void signal_buffer_event(vi_layer *layer) {
    if (layer->buffer_event) {
        hos_ipc_event_signal(layer->buffer_event);
    }
}

/* Returns the Android status for the reply. */
static int32_t bq_transact(vi_layer *layer, uint32_t code, parcel_reader *in, parcel_writer *out) {
    switch (code) {
    case BQ_CONNECT: {
        int32_t listener = parcel_read_i32(in);
        int32_t api = parcel_read_i32(in);
        int32_t controlled = parcel_read_i32(in);
        (void)listener;
        (void)controlled;
        layer->connected = 1;
        uint32_t output[4] = {DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0};
        parcel_write_data(out, output, sizeof(output));
        HOS_LOGD(MOD, "layer %" PRIu64 ": connect api %d", layer->id, api);
        return 0;
    }
    case BQ_DISCONNECT:
        layer->connected = 0;
        for (int i = 0; i < BQ_SLOTS; i++) {
            layer->slots[i].state = SLOT_EMPTY;
        }
        return 0;
    case BQ_SET_PREALLOCATED_BUFFER: {
        int32_t slot = parcel_read_i32(in);
        int32_t has_buffer = parcel_read_i32(in);
        if (slot < 0 || slot >= BQ_SLOTS) {
            return -22; /* EINVAL */
        }
        bq_slot *s = &layer->slots[slot];
        if (has_buffer) {
            memset(s->buffer, 0, sizeof(s->buffer));
            if (parcel_read_flattened(in, s->buffer, sizeof(s->buffer)) < 0) {
                return -22;
            }
            s->state = SLOT_FREE;
            HOS_LOGD(MOD, "layer %" PRIu64 ": slot %d = %ux%u pitch %u layout %u nvmap %u",
                     layer->id, slot, gb_u32(s->buffer, GB_PLANE_WIDTH),
                     gb_u32(s->buffer, GB_PLANE_HEIGHT), gb_u32(s->buffer, GB_PLANE_PITCH),
                     gb_u32(s->buffer, GB_PLANE_LAYOUT), gb_u32(s->buffer, GB_NVMAP_ID));
        } else {
            s->state = SLOT_EMPTY;
        }
        return 0;
    }
    case BQ_SET_BUFFER_COUNT:
        return 0;
    case BQ_REQUEST_BUFFER: {
        int32_t slot = parcel_read_i32(in);
        if (slot < 0 || slot >= BQ_SLOTS || layer->slots[slot].state == SLOT_EMPTY) {
            parcel_write_i32(out, 0);
            return -22;
        }
        parcel_write_i32(out, 1);
        parcel_write_flattened(out, layer->slots[slot].buffer, GRAPHIC_BUFFER_SIZE);
        return 0;
    }
    case BQ_DEQUEUE_BUFFER: {
        int32_t async = parcel_read_i32(in);
        uint32_t width = parcel_read_u32(in), height = parcel_read_u32(in);
        uint32_t format = parcel_read_u32(in), usage = parcel_read_u32(in);
        (void)async;
        (void)format;
        (void)usage;
        /* The client always expects a flattened NvMultiFence; ours has no entries. */
        uint8_t no_fence[36];
        memset(no_fence, 0, sizeof(no_fence));
        for (int i = 0; i < BQ_SLOTS; i++) {
            if (layer->slots[i].state == SLOT_FREE) {
                layer->slots[i].state = SLOT_DEQUEUED;
                parcel_write_i32(out, i);
                parcel_write_i32(out, 1);
                parcel_write_flattened(out, no_fence, sizeof(no_fence));
                return 0;
            }
        }
        HOS_LOGW(MOD, "layer %" PRIu64 ": dequeue %ux%u with no free buffer", layer->id, width,
                 height);
        parcel_write_i32(out, -1);
        parcel_write_i32(out, 1);
        parcel_write_flattened(out, no_fence, sizeof(no_fence));
        return -16; /* EBUSY */
    }
    case BQ_QUEUE_BUFFER: {
        int32_t slot = parcel_read_i32(in);
        if (slot < 0 || slot >= BQ_SLOTS || layer->slots[slot].state != SLOT_DEQUEUED) {
            return -22;
        }
        /* QueueBufferInput follows (timestamp, crop, scaling, transform, fence); consumed now */
        dump_frame(layer, layer->slots[slot].buffer);
        layer->slots[slot].state = SLOT_FREE;
        signal_buffer_event(layer);
        uint32_t output[4] = {DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0};
        parcel_write_data(out, output, sizeof(output));
        return 0;
    }
    case BQ_CANCEL_BUFFER: {
        int32_t slot = parcel_read_i32(in);
        if (slot >= 0 && slot < BQ_SLOTS && layer->slots[slot].state == SLOT_DEQUEUED) {
            layer->slots[slot].state = SLOT_FREE;
        }
        signal_buffer_event(layer);
        return 0;
    }
    case BQ_DETACH_BUFFER: {
        int32_t slot = parcel_read_i32(in);
        if (slot >= 0 && slot < BQ_SLOTS) {
            layer->slots[slot].state = SLOT_EMPTY;
        }
        return 0;
    }
    case BQ_QUERY: {
        int32_t what = parcel_read_i32(in);
        int32_t value = 0;
        switch (what) {
        case 0: /* NATIVE_WINDOW_WIDTH */
            value = DISPLAY_WIDTH;
            break;
        case 1: /* NATIVE_WINDOW_HEIGHT */
            value = DISPLAY_HEIGHT;
            break;
        case 2:        /* NATIVE_WINDOW_FORMAT */
            value = 1; /* RGBA_8888 */
            break;
        case 3: /* NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS */
            value = 1;
            break;
        default:
            break;
        }
        parcel_write_i32(out, value);
        return 0;
    }
    default:
        HOS_LOGW(MOD, "IGraphicBufferProducer transaction %u not implemented", code);
        return -38; /* ENOSYS */
    }
}

/* ---- IHOSBinderDriver ----------------------------------------------------------- */

static hos_result binder_transact(hos_ipc_ctx *ctx) {
    int32_t id = 0;
    uint32_t code = 0;
    hos_ipc_in_read(ctx, 0, &id, 4);
    hos_ipc_in_read(ctx, 4, &code, 4);
    const hos_ipc_buffer *in = gpu_in_buffer(ctx, 0);
    const hos_ipc_buffer *out = gpu_out_buffer(ctx, 0);
    if (!in || !in->addr || !out || !out->addr) {
        HOS_LOGW(MOD,
                 "transaction %u: buffers a=%u b=%u x=%u c=%u (in %#" PRIx64 " out %#" PRIx64 ")",
                 code, ctx->num_a, ctx->num_b, ctx->num_x, ctx->num_c, in ? in->addr : 0,
                 out ? out->addr : 0);
        return HOS_KERNEL_INVALID_POINTER;
    }
    parcel_reader r;
    if (!parcel_reader_init(&r, (const uint8_t *)in->addr, (size_t)in->size)) {
        HOS_LOGW(MOD, "malformed parcel for transaction %u", code);
        return HOS_KERNEL_INVALID_ARGUMENT;
    }
    /* strict mode policy word, then the interface token */
    parcel_read_i32(&r);
    parcel_skip_string16(&r);

    uint8_t reply[0x400];
    parcel_writer w;
    parcel_writer_init(&w, reply, sizeof(reply));

    pthread_mutex_lock(&g_lock);
    vi_layer *layer = layer_from_binder(id);
    int32_t status = layer ? bq_transact(layer, code, &r, &w) : -22;
    pthread_mutex_unlock(&g_lock);

    parcel_write_i32(&w, status);
    size_t n = parcel_writer_finish(&w);
    HOS_LOGT(MOD,
             "binder %d code %u: in %" PRIu64 " bytes (payload %zu, consumed %zu), status %d, "
             "reply %zu bytes into %" PRIu64,
             id, code, in->size, r.size, r.pos, status, n, out->size);
    if (n == 0 || r.error) {
        HOS_LOGW(MOD, "transaction %u: parcel error (in %d, out %d)", code, r.error, w.error);
        return HOS_KERNEL_INVALID_ARGUMENT;
    }
    size_t cap = out->size;
    memcpy((void *)out->addr, reply, n < cap ? n : cap);
    return HOS_RESULT_SUCCESS;
}

static hos_result binder_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* TransactParcel */
    case 3: /* TransactParcelAuto */
        return binder_transact(ctx);
    case 1: /* AdjustRefcount */
        return HOS_RESULT_SUCCESS;
    case 2: /* GetNativeHandle(id, type) -> event */ {
        int32_t id = 0;
        hos_ipc_in_read(ctx, 0, &id, 4);
        pthread_mutex_lock(&g_lock);
        vi_layer *layer = layer_from_binder(id);
        if (layer && !layer->buffer_event_handle) {
            layer->buffer_event_handle = hos_ipc_event_create(&layer->buffer_event);
            if (layer->buffer_event) {
                hos_ipc_event_signal(layer->buffer_event);
            }
        }
        uint32_t h = layer ? hos_ipc_copy_handle(layer->buffer_event_handle) : 0;
        pthread_mutex_unlock(&g_lock);
        if (!h) {
            return HOS_KERNEL_INVALID_HANDLE;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_binder_class = {
    .name = "vi:IHOSBinderDriver",
    .dispatch = binder_dispatch,
};

/* ---- layers ------------------------------------------------------------------------ */

static vi_layer *layer_create(uint64_t id) {
    for (int i = 0; i < VI_MAX_LAYERS; i++) {
        if (!g_layers[i].used) {
            vi_layer *l = &g_layers[i];
            memset(l, 0, sizeof(*l));
            l->used = 1;
            l->id = id ? id : g_next_layer_id;
            if (l->id >= g_next_layer_id) {
                g_next_layer_id = l->id + 1;
            }
            return l;
        }
    }
    return NULL;
}

static vi_layer *layer_by_id(uint64_t id) {
    for (int i = 0; i < VI_MAX_LAYERS; i++) {
        if (g_layers[i].used && g_layers[i].id == id) {
            return &g_layers[i];
        }
    }
    return NULL;
}

/* The NativeWindow parcel the runtime turns into a binder: magic, pid, binder id, "dispdrv". */
static hos_result write_native_window(hos_ipc_ctx *ctx, const vi_layer *layer) {
    const hos_ipc_buffer *out = gpu_out_buffer(ctx, 0);
    HOS_LOGD(MOD, "native window parcel: a=%u b=%u x=%u c=%u out %#" PRIx64 "+%#" PRIx64,
             ctx->num_a, ctx->num_b, ctx->num_x, ctx->num_c, out ? out->addr : 0,
             out ? out->size : 0);
    if (!out || !out->addr || out->size < 0x40) {
        return HOS_KERNEL_INVALID_POINTER;
    }
    uint8_t buf[0x100];
    parcel_writer w;
    parcel_writer_init(&w, buf, sizeof(buf));
    uint32_t words[6] = {2, 1, (uint32_t)((layer - g_layers) + 1), 0, 0, 0};
    parcel_write_data(&w, words, sizeof(words));
    parcel_write_data(&w, "dispdrv", 8);
    uint64_t zero = 0;
    parcel_write_data(&w, &zero, 8);
    size_t n = parcel_writer_finish(&w);
    memset((void *)out->addr, 0, out->size);
    memcpy((void *)out->addr, buf, n);
    uint64_t window_size = n;
    hos_ipc_out_write(ctx, &window_size, 8);
    return HOS_RESULT_SUCCESS;
}

static void *vsync_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16666667};
        nanosleep(&ts, NULL);
        hos_ipc_event_signal(g_vsync_event);
    }
    return NULL;
}

static hos_result out_u64(hos_ipc_ctx *ctx, uint64_t v) {
    hos_ipc_out_write(ctx, &v, 8);
    return HOS_RESULT_SUCCESS;
}

/* ---- IApplicationDisplayService ------------------------------------------------------ */

static hos_result display_dispatch(hos_object *obj, hos_ipc_ctx *ctx);

static const hos_service_class g_system_display_class;
static const hos_service_class g_manager_display_class;

static hos_result indirect_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    return hos_ipc_unimplemented(obj, ctx);
}

static const hos_service_class g_indirect_class = {
    .name = "vi:IIndirectDisplayTransactionService",
    .dispatch = indirect_dispatch,
};

static hos_result application_display_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 100: /* GetRelayService */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_binder_class, NULL));
    case 103: /* GetIndirectDisplayTransactionService */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_indirect_class, NULL));
    case 101: /* GetSystemDisplayService */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_system_display_class, NULL));
    case 102: /* GetManagerDisplayService */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_manager_display_class, NULL));
    case 1000: /* ListDisplays -> buffer of DisplayInfo, out u64 count */ {
        const hos_ipc_buffer *b = gpu_out_buffer(ctx, 0);
        if (b && b->addr && b->size >= 0x60) {
            uint8_t *info = (uint8_t *)b->addr;
            memset(info, 0, 0x60);
            memcpy(info, "Default", 8);
            info[0x40] = 1; /* has layer limit */
            uint64_t v = 1;
            memcpy(info + 0x48, &v, 8); /* max layers */
            v = DISPLAY_WIDTH;
            memcpy(info + 0x50, &v, 8);
            v = DISPLAY_HEIGHT;
            memcpy(info + 0x58, &v, 8);
        }
        return out_u64(ctx, 1);
    }
    case 1010: /* OpenDisplay(name) -> u64 display id */
    case 1011: /* OpenDefaultDisplay */
        return out_u64(ctx, 0);
    case 1020: /* CloseDisplay */
    case 1101: /* SetDisplayEnabled */
    case 2021: /* CloseLayer */
    case 2031: /* DestroyStrayLayer */
    case 2101: /* SetLayerScalingMode */
        return HOS_RESULT_SUCCESS;
    case 1102: /* GetDisplayResolution -> s64 width, s64 height */
        out_u64(ctx, DISPLAY_WIDTH);
        return out_u64(ctx, DISPLAY_HEIGHT);
    case 2020: /* OpenLayer(name[0x40], u64 layer id, u64 aruid) -> native window size + parcel */ {
        uint64_t layer_id = 0;
        hos_ipc_in_read(ctx, 0x40, &layer_id, 8);
        pthread_mutex_lock(&g_lock);
        vi_layer *layer = layer_by_id(layer_id);
        if (!layer) {
            layer = layer_create(layer_id);
        }
        pthread_mutex_unlock(&g_lock);
        if (!layer) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        HOS_LOGD(MOD, "open layer %" PRIu64, layer->id);
        return write_native_window(ctx, layer);
    }
    case 2030: /* CreateStrayLayer(u32 flags, u64 display) -> u64 layer id, u64 size + parcel */ {
        pthread_mutex_lock(&g_lock);
        vi_layer *layer = layer_create(0);
        pthread_mutex_unlock(&g_lock);
        if (!layer) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        out_u64(ctx, layer->id);
        return write_native_window(ctx, layer);
    }
    case 5202: /* GetDisplayVsyncEvent(u64 display) -> event */
    case 5203: /* GetDisplayVsyncEventForDebug */ {
        pthread_mutex_lock(&g_lock);
        if (!g_vsync_handle) {
            g_vsync_handle = hos_ipc_event_create(&g_vsync_event);
            if (g_vsync_handle) {
                pthread_create(&g_vsync_thread, NULL, vsync_thread, NULL);
                pthread_detach(g_vsync_thread);
            }
        }
        uint32_t h = g_vsync_handle ? hos_ipc_copy_handle(g_vsync_handle) : 0;
        pthread_mutex_unlock(&g_lock);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_application_display_class = {
    .name = "vi:IApplicationDisplayService",
    .dispatch = application_display_dispatch,
};

static hos_result system_display_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 2201: /* SetLayerPosition */
    case 2203: /* SetLayerSize */
    case 2205: /* SetLayerZ */
    case 2207: /* SetLayerVisibility */
    case 2312: /* CreateStrayLayer (system) */
        return HOS_RESULT_SUCCESS;
    case 3200: /* GetDisplayMode -> {u32 w, u32 h, f32 refresh, u32 unk} */ {
        uint32_t mode[4] = {DISPLAY_WIDTH, DISPLAY_HEIGHT, 0x42700000 /* 60.0f */, 0};
        hos_ipc_out_write(ctx, mode, sizeof(mode));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_system_display_class = {
    .name = "vi:ISystemDisplayService",
    .dispatch = system_display_dispatch,
};

static hos_result manager_display_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 2010: /* CreateManagedLayer(u32 flags, u64 display, u64 aruid) -> u64 layer id */ {
        pthread_mutex_lock(&g_lock);
        vi_layer *layer = layer_create(0);
        pthread_mutex_unlock(&g_lock);
        if (!layer) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        return out_u64(ctx, layer->id);
    }
    case 2011: /* DestroyManagedLayer */
    case 6000: /* AddToLayerStack */
    case 6002: /* SetLayerVisibility */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_manager_display_class = {
    .name = "vi:IManagerDisplayService",
    .dispatch = manager_display_dispatch,
};

/* ---- vi:u / vi:m / vi:s ------------------------------------------------------------- */

static hos_result display_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    /* vi:u cmd 0, vi:s cmd 1, vi:m cmd 2: all GetDisplayService(u32 policy) */
    if (ctx->cmd <= 2) {
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_application_display_class, NULL));
    }
    return hos_ipc_unimplemented(obj, ctx);
}

const hos_service_class hos_vi_u_class = {.name = "vi:u", .dispatch = display_dispatch};
const hos_service_class hos_vi_m_class = {.name = "vi:m", .dispatch = display_dispatch};
const hos_service_class hos_vi_s_class = {.name = "vi:s", .dispatch = display_dispatch};

__attribute__((destructor)) static void vi_cleanup(void) {
    display_x11_quit();
}
