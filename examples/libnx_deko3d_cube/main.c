/*
 * GPU milestone M3b (docs/gpu.md): a 3D cube. The scene of deko3d's
 * Example03 at a fixed rotation, without its depth buffer (M3c): 24 vertices
 * (position + colour, 3x32 float each) in a vertex buffer, drawn as quads;
 * a uniform buffer with model-view and projection matrices (std140),
 * uploaded with PushConstants; back faces culled, which is enough for a
 * convex cube. tests/check_cube.py recomputes the frame independently.
 *
 * Build: S2PC_EXTRA_LIBS=-ldeko3dd scripts/build_libnx.sh deko3d_cube
 */

#include <deko3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define FB_NUM      2
#define FB_WIDTH    1280
#define FB_HEIGHT   720
#define CODEMEM     (64 * 1024)
#define CMDMEM      (64 * 1024)
#define DATAMEM     (64 * 1024)
#define CUBE_FRAMES 2

/* Fixed animation phase, so the frame is reproducible (Example03 animates these). */
#define PERIOD1 0.1f
#define PERIOD2 0.15f

typedef struct vertex {
    float position[3];
    float color[3];
} vertex;

static const vertex k_cube[24] = {
    {{+1, +1, +1}, {1, 0, 0}}, {{+1, -1, +1}, {0, 1, 0}}, {{+1, -1, -1}, {0, 0, 1}},
    {{+1, +1, -1}, {1, 1, 0}}, {{-1, +1, -1}, {1, 0, 0}}, {{-1, -1, -1}, {0, 1, 0}},
    {{-1, -1, +1}, {0, 0, 1}}, {{-1, +1, +1}, {1, 1, 0}}, {{-1, +1, -1}, {1, 0, 0}},
    {{-1, +1, +1}, {0, 1, 0}}, {{+1, +1, +1}, {0, 0, 1}}, {{+1, +1, -1}, {1, 1, 0}},
    {{-1, -1, +1}, {1, 0, 0}}, {{-1, -1, -1}, {0, 1, 0}}, {{+1, -1, -1}, {0, 0, 1}},
    {{+1, -1, +1}, {1, 1, 0}}, {{-1, +1, +1}, {1, 0, 0}}, {{-1, -1, +1}, {0, 1, 0}},
    {{+1, -1, +1}, {0, 0, 1}}, {{+1, +1, +1}, {1, 1, 0}}, {{+1, +1, -1}, {1, 0, 0}},
    {{+1, -1, -1}, {0, 1, 0}}, {{-1, -1, -1}, {0, 0, 1}}, {{-1, +1, -1}, {1, 1, 0}},
};

/* Column-major 4x4, m[col][row], as glm and GLSL std140 lay it out. */
typedef struct mat4 {
    float m[4][4];
} mat4;

static mat4 identity(void) {
    mat4 r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 4; i++) {
        r.m[i][i] = 1.0f;
    }
    return r;
}

static mat4 mul(const mat4 *a, const mat4 *b) {
    mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) {
                s += a->m[k][row] * b->m[c][k];
            }
            r.m[c][row] = s;
        }
    }
    return r;
}

static mat4 translate(const mat4 *m, float x, float y, float z) {
    mat4 t = identity();
    t.m[3][0] = x;
    t.m[3][1] = y;
    t.m[3][2] = z;
    return mul(m, &t);
}

static mat4 rotate(const mat4 *m, float angle, float x, float y, float z) {
    float c = cosf(angle), s = sinf(angle), k = 1.0f - c;
    mat4 r = identity();
    r.m[0][0] = c + k * x * x;
    r.m[0][1] = k * x * y + s * z;
    r.m[0][2] = k * x * z - s * y;
    r.m[1][0] = k * y * x - s * z;
    r.m[1][1] = c + k * y * y;
    r.m[1][2] = k * y * z + s * x;
    r.m[2][0] = k * z * x + s * y;
    r.m[2][1] = k * z * y - s * x;
    r.m[2][2] = c + k * z * z;
    return mul(m, &r);
}

static mat4 scale(const mat4 *m, float v) {
    mat4 s = identity();
    s.m[0][0] = s.m[1][1] = s.m[2][2] = v;
    return mul(m, &s);
}

/* glm::perspectiveRH_ZO */
static mat4 perspective(float fovy, float aspect, float n, float f) {
    float t = tanf(fovy / 2.0f);
    mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0][0] = 1.0f / (aspect * t);
    r.m[1][1] = 1.0f / t;
    r.m[2][2] = f / (n - f);
    r.m[2][3] = -1.0f;
    r.m[3][2] = -(f * n) / (f - n);
    return r;
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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (R_FAILED(romfsInit())) {
        say("FAIL romfsInit (pass --romfs examples/libnx_deko3d_cube/romfs)");
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
    uint32_t fb_size = dkImageLayoutGetSize(&layout), fb_align = dkImageLayoutGetAlignment(&layout);
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
    if (!load_shader(&vsh, code_mem, &code_off, "romfs:/shaders/transform_vsh.dksh") ||
        !load_shader(&fsh, code_mem, &code_off, "romfs:/shaders/color_fsh.dksh")) {
        return 1;
    }

    /* Data: the uniform buffer at 0 (aligned to 0x100), the vertices at 0x1000. */
    dkMemBlockMakerDefaults(&mm, device, DATAMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock data_mem = dkMemBlockCreate(&mm);
    DkGpuAddr data_gpu = dkMemBlockGetGpuAddr(data_mem);
    uint8_t *data_cpu = dkMemBlockGetCpuAddr(data_mem);
    DkGpuAddr ubo = data_gpu, vbo = data_gpu + 0x1000;
    memcpy(data_cpu + 0x1000, k_cube, sizeof(k_cube));

    mat4 transform[2]; /* model-view, projection */
    const float tau = 6.2831853f;
    transform[0] = identity();
    transform[0] = translate(&transform[0], 0.0f, 0.0f, -3.0f);
    transform[0] = rotate(&transform[0], sinf(PERIOD2 * tau) * tau / 8.0f, 1, 0, 0);
    transform[0] = rotate(&transform[0], -PERIOD1 * tau, 0, 1, 0);
    transform[0] = scale(&transform[0], 0.5f);
    transform[1] = perspective(40.0f * tau / 360.0f, (float)FB_WIDTH / FB_HEIGHT, 0.01f, 1000.0f);

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

    static const DkVtxAttribState attribs[2] = {
        {0, 0, offsetof(vertex, position), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(vertex, color), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
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
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    /* uboSize must be a multiple of DK_UNIFORM_BUF_ALIGNMENT (0x100); the data is 128 bytes */
    dkCmdBufPushConstants(cmdbuf, ubo, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof(transform), transform);
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindUniformBuffer(cmdbuf, DkStage_Vertex, 0, ubo, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindRasterizerState(cmdbuf, &rs);
    dkCmdBufBindColorState(cmdbuf, &cs);
    dkCmdBufBindColorWriteState(cmdbuf, &cws);
    dkCmdBufBindVtxBuffer(cmdbuf, 0, vbo, sizeof(k_cube));
    dkCmdBufBindVtxAttribState(cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(cmdbuf, buffers, 1);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Quads, 24, 1, 0, 0);
    DkCmdList render = dkCmdBufFinishList(cmdbuf);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&qm);
    say("deko3d: cube scene ready");

    for (int frame = 0; frame < CUBE_FRAMES && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, bind_fb[slot]);
        dkQueueSubmitCommands(queue, render);
        dkQueuePresentImage(queue, swapchain, slot);
        say("deko3d: cube frame %d presented", frame);
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkMemBlockDestroy(data_mem);
    dkMemBlockDestroy(code_mem);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(fb_mem);
    dkDeviceDestroy(device);
    romfsExit();
    say("deko3d cube: done");
    return 0;
}
