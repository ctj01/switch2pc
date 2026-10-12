/*
 * GPU milestone M4 (docs/gpu.md): textures. A 64x64 RGBA8 texture made on the
 * CPU (red = 4x, green = 4y, blue = an 8x8 checkerboard) is copied into a
 * block-linear image, described with image and sampler descriptors, and
 * sampled by a fragment shader on two quads:
 *
 *   left   nearest filtering, clamp to edge, uv 0..1
 *   right  linear filtering, repeat, uv 0..2 (the texture tiles 2x2)
 *   bottom a 16x16 BC1 texture (4x4 blocks; even blocks in 4-colour mode,
 *          odd blocks in 3-colour + transparent mode), nearest, clamp
 *
 * tests/check_texture.py recomputes the frame.
 *
 * Build: S2PC_EXTRA_LIBS=-ldeko3dd scripts/build_libnx.sh deko3d_texture
 */

#include <deko3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define FB_NUM    2
#define FB_WIDTH  1280
#define FB_HEIGHT 720
#define TEX_SIZE  64
#define CODEMEM   (64 * 1024)
#define CMDMEM    (64 * 1024)
#define DATAMEM   (64 * 1024)
#define FRAMES    2

/* data block layout */
#define OFF_VERTICES 0x0000
#define OFF_IMAGES   0x0800 /* image descriptor set (32-byte aligned) */
#define OFF_SAMPLERS 0x0900 /* sampler descriptor set */
#define OFF_TEXELS   0x1000 /* linear staging copy of the texture */
#define OFF_BC1      0x6000 /* BC1 blocks, 4x4 of them */
#define BC1_SIZE     16

typedef struct vertex {
    float pos[3];
    float uv[2];
} vertex;

#define QUAD(x0, y0, x1, y1, u1, v1)                                                               \
    {{x0, y0, 0.5f}, {0, v1}}, {{x1, y0, 0.5f}, {u1, v1}}, {{x1, y1, 0.5f}, {u1, 0}}, {            \
        {x0, y1, 0.5f}, {                                                                          \
            0, 0                                                                                   \
        }                                                                                          \
    }

static const vertex k_quads[12] = {
    QUAD(-0.9f, -0.4f, -0.1f, 0.4f, 1.0f, 1.0f),   /* nearest, clamp */
    QUAD(0.1f, -0.4f, 0.9f, 0.4f, 2.0f, 2.0f),     /* linear, repeat */
    QUAD(-0.4f, -0.95f, 0.4f, -0.55f, 1.0f, 1.0f), /* BC1, nearest, clamp */
};

static uint16_t rgb565(uint32_t r5, uint32_t g6, uint32_t b5) {
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* Block (bx, by): two colours, swapped on odd blocks to select the 3-colour mode;
 * texel (x, y) of the block uses index (x + 2y + bx) & 3. */
static void make_bc1(uint8_t *out) {
    for (int by = 0; by < BC1_SIZE / 4; by++) {
        for (int bx = 0; bx < BC1_SIZE / 4; bx++) {
            uint16_t hi = rgb565(31, (uint32_t)(by * 21), (uint32_t)(bx * 10));
            uint16_t lo = rgb565(2, 5, (uint32_t)(31 - bx * 7));
            int odd = (bx + by) & 1;
            uint16_t c0 = odd ? lo : hi, c1 = odd ? hi : lo;
            uint8_t *b = out + (by * (BC1_SIZE / 4) + bx) * 8;
            b[0] = (uint8_t)c0;
            b[1] = (uint8_t)(c0 >> 8);
            b[2] = (uint8_t)c1;
            b[3] = (uint8_t)(c1 >> 8);
            for (int y = 0; y < 4; y++) {
                uint8_t row = 0;
                for (int x = 0; x < 4; x++) {
                    row |= (uint8_t)(((x + 2 * y + bx) & 3) << (2 * x));
                }
                b[4 + y] = row;
            }
        }
    }
}

static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        svcOutputDebugString(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static void on_debug(void *user, const char *context, DkResult result, const char *message) {
    (void)user;
    say("deko3d: %s: result %d: %s", context ? context : "?", (int)result, message ? message : "");
}

static int load_shader(DkShader *shader, DkMemBlock code, uint32_t *offset, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        say("FAIL cannot open %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    rewind(f);
    uint32_t at = *offset;
    *offset += (size + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    fread((uint8_t *)dkMemBlockGetCpuAddr(code) + at, size, 1, f);
    fclose(f);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, code, at);
    dkShaderInitialize(shader, &sm);
    return 1;
}

static uint32_t aligned(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (R_FAILED(romfsInit())) {
        say("FAIL romfsInit (pass --romfs examples/libnx_deko3d_texture/romfs)");
        return 1;
    }
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    DkDevice device = dkDeviceCreate(&dm);

    /* Framebuffers and the texture share one image memory block. */
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_WIDTH;
    lm.dimensions[1] = FB_HEIGHT;
    DkImageLayout fb_layout;
    dkImageLayoutInitialize(&fb_layout, &lm);
    dkImageLayoutMakerDefaults(&lm, device);
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = TEX_SIZE;
    lm.dimensions[1] = TEX_SIZE;
    DkImageLayout tex_layout;
    dkImageLayoutInitialize(&tex_layout, &lm);
    lm.format = DkImageFormat_RGBA_BC1;
    lm.dimensions[0] = BC1_SIZE;
    lm.dimensions[1] = BC1_SIZE;
    DkImageLayout bc1_layout;
    dkImageLayoutInitialize(&bc1_layout, &lm);
    uint32_t align = dkImageLayoutGetAlignment(&fb_layout);
    if (dkImageLayoutGetAlignment(&tex_layout) > align) {
        align = dkImageLayoutGetAlignment(&tex_layout);
    }
    uint32_t fb_size = aligned(dkImageLayoutGetSize(&fb_layout), align);
    if (dkImageLayoutGetAlignment(&bc1_layout) > align) {
        align = dkImageLayoutGetAlignment(&bc1_layout);
    }
    uint32_t tex_size = aligned(dkImageLayoutGetSize(&tex_layout), align);
    uint32_t bc1_size = aligned(dkImageLayoutGetSize(&bc1_layout), align);

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, device, FB_NUM * fb_size + tex_size + bc1_size);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock image_mem = dkMemBlockCreate(&mm);
    DkImage fbs[FB_NUM], tex, bc1;
    DkImage const *fb_ptrs[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        dkImageInitialize(&fbs[i], &fb_layout, image_mem, (uint32_t)i * fb_size);
        fb_ptrs[i] = &fbs[i];
    }
    dkImageInitialize(&tex, &tex_layout, image_mem, FB_NUM * fb_size);
    dkImageInitialize(&bc1, &bc1_layout, image_mem, FB_NUM * fb_size + tex_size);
    DkSwapchainMaker scm;
    dkSwapchainMakerDefaults(&scm, device, nwindowGetDefault(), fb_ptrs, FB_NUM);
    DkSwapchain swapchain = dkSwapchainCreate(&scm);

    dkMemBlockMakerDefaults(&mm, device, CODEMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock code_mem = dkMemBlockCreate(&mm);
    uint32_t code_off = 0;
    DkShader vsh, fsh;
    if (!load_shader(&vsh, code_mem, &code_off, "romfs:/shaders/texcoord_vsh.dksh") ||
        !load_shader(&fsh, code_mem, &code_off, "romfs:/shaders/texture_fsh.dksh")) {
        return 1;
    }

    dkMemBlockMakerDefaults(&mm, device, DATAMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock data_mem = dkMemBlockCreate(&mm);
    uint8_t *data = dkMemBlockGetCpuAddr(data_mem);
    DkGpuAddr data_gpu = dkMemBlockGetGpuAddr(data_mem);
    memcpy(data + OFF_VERTICES, k_quads, sizeof(k_quads));
    uint8_t *texels = data + OFF_TEXELS;
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint8_t *p = texels + (y * TEX_SIZE + x) * 4;
            p[0] = (uint8_t)(x * 4);
            p[1] = (uint8_t)(y * 4);
            p[2] = ((x / 8 + y / 8) & 1) ? 255 : 0;
            p[3] = 255;
        }
    }

    make_bc1(data + OFF_BC1);

    /* Two image descriptors (RGBA8, BC1), two sampler descriptors. */
    DkImageView tex_view, bc1_view;
    dkImageViewDefaults(&tex_view, &tex);
    dkImageViewDefaults(&bc1_view, &bc1);
    DkImageDescriptor image_desc[2];
    dkImageDescriptorInitialize(&image_desc[0], &tex_view, false, false);
    dkImageDescriptorInitialize(&image_desc[1], &bc1_view, false, false);
    memcpy(data + OFF_IMAGES, image_desc, sizeof(image_desc));
    DkSampler samplers[2];
    dkSamplerDefaults(&samplers[0]);
    samplers[0].wrapMode[0] = samplers[0].wrapMode[1] = DkWrapMode_ClampToEdge;
    dkSamplerDefaults(&samplers[1]);
    samplers[1].minFilter = samplers[1].magFilter = DkFilter_Linear;
    for (int i = 0; i < 2; i++) {
        DkSamplerDescriptor sd;
        dkSamplerDescriptorInitialize(&sd, &samplers[i]);
        memcpy(data + OFF_SAMPLERS + i * sizeof(sd), &sd, sizeof(sd));
    }

    dkMemBlockMakerDefaults(&mm, device, CMDMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(cmdbuf, cmd_mem, 0, CMDMEM);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&qm);

    /* Upload the texture once. */
    DkCopyBuf src = {data_gpu + OFF_TEXELS, 0, 0};
    DkImageRect rect = {0, 0, 0, TEX_SIZE, TEX_SIZE, 1};
    dkCmdBufCopyBufferToImage(cmdbuf, &src, &tex_view, &rect, 0);
    DkCopyBuf bc1_src = {data_gpu + OFF_BC1, 0, 0};
    DkImageRect bc1_rect = {0, 0, 0, BC1_SIZE, BC1_SIZE, 1};
    dkCmdBufCopyBufferToImage(cmdbuf, &bc1_src, &bc1_view, &bc1_rect, 0);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueueWaitIdle(queue);
    dkCmdBufClear(cmdbuf);

    DkCmdList bind_fb[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        DkImageView view;
        dkImageViewDefaults(&view, &fbs[i]);
        dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
        bind_fb[i] = dkCmdBufFinishList(cmdbuf);
    }

    static const DkVtxAttribState attribs[2] = {
        {0, 0, offsetof(vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffers[1] = {{sizeof(vertex), 0}};
    DkViewport viewport = {0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT, 0.0f, 1.0f};
    DkScissor scissor = {0, 0, FB_WIDTH, FB_HEIGHT};
    DkShader const *shaders[] = {&vsh, &fsh};
    DkRasterizerState rs;
    DkColorState cs;
    DkColorWriteState cws;
    dkRasterizerStateDefaults(&rs);
    dkColorStateDefaults(&cs);
    dkColorWriteStateDefaults(&cws);

    dkCmdBufSetViewports(cmdbuf, 0, &viewport, 1);
    dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.1f, 0.1f, 0.1f, 1.0f);
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindRasterizerState(cmdbuf, &rs);
    dkCmdBufBindColorState(cmdbuf, &cs);
    dkCmdBufBindColorWriteState(cmdbuf, &cws);
    dkCmdBufBindVtxBuffer(cmdbuf, 0, data_gpu + OFF_VERTICES, sizeof(k_quads));
    dkCmdBufBindVtxAttribState(cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(cmdbuf, buffers, 1);
    dkCmdBufBindImageDescriptorSet(cmdbuf, data_gpu + OFF_IMAGES, 2);
    dkCmdBufBindSamplerDescriptorSet(cmdbuf, data_gpu + OFF_SAMPLERS, 2);
    dkCmdBufBindTexture(cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 0, 0);
    dkCmdBufBindTexture(cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(0, 1));
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 4, 0);
    dkCmdBufBindTexture(cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(1, 0));
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 8, 0);
    DkCmdList render = dkCmdBufFinishList(cmdbuf);
    say("deko3d: texture scene ready");

    for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, bind_fb[slot]);
        dkQueueSubmitCommands(queue, render);
        dkQueuePresentImage(queue, swapchain, slot);
        say("deko3d: texture frame %d presented", frame);
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkMemBlockDestroy(data_mem);
    dkMemBlockDestroy(code_mem);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(image_mem);
    dkDeviceDestroy(device);
    romfsExit();
    say("deko3d texture: done");
    return 0;
}
