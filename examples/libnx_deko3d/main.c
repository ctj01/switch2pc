/*
 * GPU milestone M0 (docs/gpu.md): a deko3d program with no shaders. It makes
 * a device, two framebuffers, a swapchain and a queue, then for a number of
 * frames records a clear to a changing colour and presents. Under M0 the
 * driver accepts every GPU ioctl and logs the command stream without running
 * it, so the frames are not expected to show the colour yet; the program
 * must get through device creation, submission, presentation and teardown.
 *
 * Build: S2PC_EXTRA_LIBS=-ldeko3dd scripts/build_libnx.sh deko3d
 * (the debug deko3d: it validates more and reports through cbDebug)
 */

#include <deko3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define FB_NUM    2
#define FB_WIDTH  1280
#define FB_HEIGHT 720
#define CMDMEM    (16 * 1024)
#define SCRATCH   (64 * 1024)

#ifndef DEKO_FRAMES
#define DEKO_FRAMES 60
#endif

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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    DkDevice device = dkDeviceCreate(&dm);
    say("deko3d: device %s", device ? "created" : "FAIL");
    if (!device) {
        return 1;
    }

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
    say("deko3d: framebuffer memory %u bytes %s", FB_NUM * fb_size, fb_mem ? "ok" : "FAIL");

    DkImage fbs[FB_NUM];
    DkImage const *fb_ptrs[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        dkImageInitialize(&fbs[i], &layout, fb_mem, (uint32_t)i * fb_size);
        fb_ptrs[i] = &fbs[i];
    }

    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device, nwindowGetDefault(), fb_ptrs, FB_NUM);
    DkSwapchain swapchain = dkSwapchainCreate(&sm);
    say("deko3d: swapchain %s", swapchain ? "created" : "FAIL");

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
    say("deko3d: queue %s", queue ? "created" : "FAIL");

    /* The vsync test asks for a longer run through sdmc:/deko3d_frames.txt: a frame
     * count and, with "light", frames that are presented without being drawn, so
     * the program is faster than a refresh even under emulation. */
    int frames = DEKO_FRAMES, light = 0;
    FILE *wanted = fopen("sdmc:/deko3d_frames.txt", "r");
    if (wanted) {
        char mode[16] = "";
        if (fscanf(wanted, "%d %15s", &frames, mode) < 1 || frames < 1) {
            frames = DEKO_FRAMES;
        }
        light = strcmp(mode, "light") == 0;
        fclose(wanted);
    }
    for (int frame = 0; frame < frames && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        if (!light) {
            DkImageView view;
            dkImageViewDefaults(&view, &fbs[slot]);
            dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
            float t = (float)(frame % 60) / 60.0f;
            dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, t, 0.25f, 1.0f - t, 1.0f);
            dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
        }
        dkQueuePresentImage(queue, swapchain, slot);
        if (frame % 20 == 0) {
            say("deko3d: frame %d presented on slot %d", frame, slot);
        }
        dkCmdBufClear(cmdbuf);
    }

    dkQueueWaitIdle(queue);

    /* M1: inline uploads and DMA copies, each checked byte for byte on the CPU. */
    dkMemBlockMakerDefaults(&mm, device, SCRATCH);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock scratch = dkMemBlockCreate(&mm);
    uint8_t *cpu = dkMemBlockGetCpuAddr(scratch);
    DkGpuAddr gpu = dkMemBlockGetGpuAddr(scratch);
    memset(cpu, 0, SCRATCH);

    static const char inline_msg[16] = "inline to mem!";
    dkCmdBufPushData(cmdbuf, gpu + 0x0000, inline_msg, sizeof(inline_msg));
    for (int i = 0; i < 256; i++) {
        cpu[0x1000 + i] = (uint8_t)(i * 7 + 3);
    }
    dkCmdBufCopyBuffer(cmdbuf, gpu + 0x1000, gpu + 0x2000, 256);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueueWaitIdle(queue);
    dkCmdBufClear(cmdbuf);
    say("deko3d: push data %s", memcmp(cpu, inline_msg, sizeof(inline_msg)) == 0 ? "ok" : "FAIL");
    say("deko3d: copy buffer %s", memcmp(cpu + 0x1000, cpu + 0x2000, 256) == 0 ? "ok" : "FAIL");

    /* A 64x64 pattern into a framebuffer (linear -> block-linear) and back out. */
    uint32_t *pattern = (uint32_t *)(cpu + 0x4000);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            pattern[y * 64 + x] = 0xFF000000u | ((uint32_t)(y * 4) << 8) | (uint32_t)(x * 4);
        }
    }
    int slot = dkQueueAcquireImage(queue, swapchain);
    DkImageView view;
    dkImageViewDefaults(&view, &fbs[slot]);
    dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    DkCopyBuf buf = {gpu + 0x4000, 0, 0};
    DkImageRect rect = {100, 100, 0, 64, 64, 1};
    dkCmdBufCopyBufferToImage(cmdbuf, &buf, &view, &rect, 0);
    DkCopyBuf back = {gpu + 0x8000, 0, 0};
    dkCmdBufCopyImageToBuffer(cmdbuf, &view, &rect, &back, 0);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueuePresentImage(queue, swapchain, slot);
    dkQueueWaitIdle(queue);
    dkCmdBufClear(cmdbuf);
    say("deko3d: image round trip %s",
        memcmp(cpu + 0x4000, cpu + 0x8000, 64 * 64 * 4) == 0 ? "ok" : "FAIL");
    dkMemBlockDestroy(scratch);

    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(fb_mem);
    dkDeviceDestroy(device);
    say("deko3d: done");
    return 0;
}
