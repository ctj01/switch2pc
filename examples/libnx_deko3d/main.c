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

    for (int frame = 0; frame < DEKO_FRAMES && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        DkImageView view;
        dkImageViewDefaults(&view, &fbs[slot]);
        dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
        float t = (float)(frame % 60) / 60.0f;
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, t, 0.25f, 1.0f - t, 1.0f);
        dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
        dkQueuePresentImage(queue, swapchain, slot);
        if (frame % 20 == 0) {
            say("deko3d: frame %d presented on slot %d", frame, slot);
        }
        dkCmdBufClear(cmdbuf);
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(fb_mem);
    dkDeviceDestroy(device);
    say("deko3d: done");
    return 0;
}
