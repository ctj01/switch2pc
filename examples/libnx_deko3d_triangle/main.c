/*
 * GPU milestone M3a (docs/gpu.md): the first triangle. The same scene as
 * devkitPro's deko_basic example: a vertex shader that picks positions and
 * colours from constant arrays by gl_VertexID, a fragment shader that writes
 * the interpolated colour, one draw of three vertices over a cleared
 * 1280x720 target. The shaders are compiled by uam (scripts/build_libnx.sh)
 * into romfs:/shaders.
 *
 * Build: S2PC_EXTRA_LIBS=-ldeko3dd scripts/build_libnx.sh deko3d_triangle
 * Run:   hosrun --romfs examples/libnx_deko3d_triangle/romfs deko3d_triangle.nso
 */

#include <deko3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define FB_NUM     2
#define FB_WIDTH   1280
#define FB_HEIGHT  720
#define CODEMEM    (64 * 1024)
#define CMDMEM     (16 * 1024)
#define TRI_FRAMES 3

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
    say("deko3d: loaded %s (%u bytes at %u)", path, size, at);
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (R_FAILED(romfsInit())) {
        say("FAIL romfsInit (pass --romfs examples/libnx_deko3d_triangle/romfs)");
        return 1;
    }

    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    DkDevice device = dkDeviceCreate(&dm);

    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_WIDTH;
    lm.dimensions[1] = FB_HEIGHT;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    uint32_t fb_size = dkImageLayoutGetSize(&layout);
    uint32_t fb_align = dkImageLayoutGetAlignment(&layout);
    fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, device, FB_NUM * fb_size);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock fb_mem = dkMemBlockCreate(&mm);
    DkImage fbs[FB_NUM];
    DkImage const *fb_ptrs[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        dkImageInitialize(&fbs[i], &layout, fb_mem, (uint32_t)i * fb_size);
        fb_ptrs[i] = &fbs[i];
    }
    DkSwapchainMaker scm;
    dkSwapchainMakerDefaults(&scm, device, nwindowGetDefault(), fb_ptrs, FB_NUM);
    DkSwapchain swapchain = dkSwapchainCreate(&scm);

    dkMemBlockMakerDefaults(&mm, device, CODEMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock code_mem = dkMemBlockCreate(&mm);
    uint32_t code_off = 0;
    DkShader vsh, fsh;
    if (!load_shader(&vsh, code_mem, &code_off, "romfs:/shaders/triangle_vsh.dksh") ||
        !load_shader(&fsh, code_mem, &code_off, "romfs:/shaders/color_fsh.dksh")) {
        return 1;
    }

    dkMemBlockMakerDefaults(&mm, device, CMDMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(cmdbuf, cmd_mem, 0, CMDMEM);

    DkCmdList bind_fb[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        DkImageView view;
        dkImageViewDefaults(&view, &fbs[i]);
        dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
        bind_fb[i] = dkCmdBufFinishList(cmdbuf);
    }

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
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.125f, 0.294f, 0.478f, 1.0f);
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindRasterizerState(cmdbuf, &rs);
    dkCmdBufBindColorState(cmdbuf, &cs);
    dkCmdBufBindColorWriteState(cmdbuf, &cws);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
    DkCmdList render = dkCmdBufFinishList(cmdbuf);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&qm);
    say("deko3d: triangle scene ready");

    for (int frame = 0; frame < TRI_FRAMES && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, bind_fb[slot]);
        dkQueueSubmitCommands(queue, render);
        dkQueuePresentImage(queue, swapchain, slot);
        say("deko3d: triangle frame %d presented", frame);
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkMemBlockDestroy(code_mem);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(fb_mem);
    dkDeviceDestroy(device);
    romfsExit();
    say("deko3d triangle: done");
    return 0;
}
