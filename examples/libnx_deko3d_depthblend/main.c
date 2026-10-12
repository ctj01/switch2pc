/*
 * GPU milestone M3c (docs/gpu.md): depth testing and blending. Four quads in
 * NDC, each with its own state, over a Z24S8 depth buffer cleared to 1.0:
 *
 *   A red,    z 0.3, depth test LESS + write, opaque
 *   B green,  z 0.6, same state, drawn after A: loses where they overlap
 *   C blue,   z 0.1, alpha 0.5, depth test without write, blended with
 *             SrcAlpha / InvSrcAlpha
 *   D yellow, z 0.9, test + write: hidden behind A and B, but drawn over
 *             C wherever C covered only background, since C wrote no depth
 *
 * tests/check_depthblend.py recomputes the frame.
 *
 * Build: S2PC_EXTRA_LIBS=-ldeko3dd scripts/build_libnx.sh deko3d_depthblend
 */

#include <deko3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define FB_NUM    2
#define FB_WIDTH  1280
#define FB_HEIGHT 720
#define CODEMEM   (64 * 1024)
#define CMDMEM    (64 * 1024)
#define DATAMEM   (64 * 1024)
#define FRAMES    2

typedef struct vertex {
    float pos[3];
    float color[4];
} vertex;

#define QUAD(x0, y0, x1, y1, z, r, g, b, a)                                                    \
    {{x0, y0, z}, {r, g, b, a}}, {{x1, y0, z}, {r, g, b, a}}, {{x1, y1, z}, {r, g, b, a}},      \
        {{x0, y1, z}, {r, g, b, a}}

static const vertex k_quads[16] = {
    QUAD(-0.6f, -0.5f, 0.2f, 0.5f, 0.3f, 1, 0, 0, 1),   /* A */
    QUAD(-0.2f, -0.3f, 0.6f, 0.7f, 0.6f, 0, 1, 0, 1),   /* B */
    QUAD(0.0f, -0.7f, 0.8f, 0.1f, 0.1f, 0, 0, 1, 0.5f), /* C */
    QUAD(-0.9f, -0.9f, 0.9f, -0.6f, 0.9f, 1, 1, 0, 1),  /* D */
};

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
    say("deko3d: %s: result %d: %s", context ? context : "?", (int)result,
        message ? message : "");
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
        say("FAIL romfsInit (pass --romfs examples/libnx_deko3d_depthblend/romfs)");
        return 1;
    }
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    DkDevice device = dkDeviceCreate(&dm);

    /* Colour targets and a depth buffer in one image memory block. */
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_WIDTH;
    lm.dimensions[1] = FB_HEIGHT;
    DkImageLayout color_layout;
    dkImageLayoutInitialize(&color_layout, &lm);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_Z24S8;
    DkImageLayout depth_layout;
    dkImageLayoutInitialize(&depth_layout, &lm);
    uint32_t align = dkImageLayoutGetAlignment(&color_layout);
    if (dkImageLayoutGetAlignment(&depth_layout) > align) {
        align = dkImageLayoutGetAlignment(&depth_layout);
    }
    uint32_t color_size = aligned(dkImageLayoutGetSize(&color_layout), align);
    uint32_t depth_size = aligned(dkImageLayoutGetSize(&depth_layout), align);

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, device, FB_NUM * color_size + depth_size);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock image_mem = dkMemBlockCreate(&mm);
    DkImage fbs[FB_NUM], depth;
    DkImage const *fb_ptrs[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        dkImageInitialize(&fbs[i], &color_layout, image_mem, (uint32_t)i * color_size);
        fb_ptrs[i] = &fbs[i];
    }
    dkImageInitialize(&depth, &depth_layout, image_mem, FB_NUM * color_size);
    DkSwapchainMaker scm;
    dkSwapchainMakerDefaults(&scm, device, nwindowGetDefault(), fb_ptrs, FB_NUM);
    DkSwapchain swapchain = dkSwapchainCreate(&scm);

    dkMemBlockMakerDefaults(&mm, device, CODEMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock code_mem = dkMemBlockCreate(&mm);
    uint32_t code_off = 0;
    DkShader vsh, fsh;
    if (!load_shader(&vsh, code_mem, &code_off, "romfs:/shaders/passthrough_vsh.dksh") ||
        !load_shader(&fsh, code_mem, &code_off, "romfs:/shaders/color_fsh.dksh")) {
        return 1;
    }

    dkMemBlockMakerDefaults(&mm, device, DATAMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock data_mem = dkMemBlockCreate(&mm);
    memcpy(dkMemBlockGetCpuAddr(data_mem), k_quads, sizeof(k_quads));
    DkGpuAddr vbo = dkMemBlockGetGpuAddr(data_mem);

    dkMemBlockMakerDefaults(&mm, device, CMDMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(cmdbuf, cmd_mem, 0, CMDMEM);

    DkImageView depth_view;
    dkImageViewDefaults(&depth_view, &depth);
    DkCmdList bind_fb[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        DkImageView view;
        dkImageViewDefaults(&view, &fbs[i]);
        dkCmdBufBindRenderTarget(cmdbuf, &view, &depth_view);
        bind_fb[i] = dkCmdBufFinishList(cmdbuf);
    }

    static const DkVtxAttribState attribs[2] = {
        {0, 0, offsetof(vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(vertex, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffers[1] = {{sizeof(vertex), 0}};
    DkViewport viewport = {0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT, 0.0f, 1.0f};
    DkScissor scissor = {0, 0, FB_WIDTH, FB_HEIGHT};
    DkShader const *shaders[] = {&vsh, &fsh};
    DkRasterizerState rs;
    DkColorWriteState cws;
    dkRasterizerStateDefaults(&rs);
    dkColorWriteStateDefaults(&cws);

    DkColorState opaque, blended;
    dkColorStateDefaults(&opaque);
    dkColorStateDefaults(&blended);
    blended.blendEnableMask = 1;
    DkBlendState alpha_blend;
    dkBlendStateDefaults(&alpha_blend); /* SrcAlpha, InvSrcAlpha, Add */
    DkDepthStencilState test_write, test_only;
    dkDepthStencilStateDefaults(&test_write); /* test + write, LESS */
    dkDepthStencilStateDefaults(&test_only);
    test_only.depthWriteEnable = 0;

    dkCmdBufSetViewports(cmdbuf, 0, &viewport, 1);
    dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.1f, 0.1f, 0.1f, 1.0f);
    dkCmdBufClearDepthStencil(cmdbuf, true, 1.0f, 0xFF, 0);
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindRasterizerState(cmdbuf, &rs);
    dkCmdBufBindColorWriteState(cmdbuf, &cws);
    dkCmdBufBindBlendStates(cmdbuf, 0, &alpha_blend, 1);
    dkCmdBufBindVtxBuffer(cmdbuf, 0, vbo, sizeof(k_quads));
    dkCmdBufBindVtxAttribState(cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(cmdbuf, buffers, 1);

    dkCmdBufBindColorState(cmdbuf, &opaque);
    dkCmdBufBindDepthStencilState(cmdbuf, &test_write);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 0, 0); /* A */
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 4, 0); /* B */
    dkCmdBufBindColorState(cmdbuf, &blended);
    dkCmdBufBindDepthStencilState(cmdbuf, &test_only);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 8, 0); /* C */
    dkCmdBufBindColorState(cmdbuf, &opaque);
    dkCmdBufBindDepthStencilState(cmdbuf, &test_write);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 4, 1, 12, 0); /* D */
    DkCmdList render = dkCmdBufFinishList(cmdbuf);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&qm);
    say("deko3d: depth/blend scene ready");

    for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, bind_fb[slot]);
        dkQueueSubmitCommands(queue, render);
        dkQueuePresentImage(queue, swapchain, slot);
        say("deko3d: depth/blend frame %d presented", frame);
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
    say("deko3d depth/blend: done");
    return 0;
}
