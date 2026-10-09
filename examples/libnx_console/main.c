/*
 * libnx console: the classic homebrew hello world, unchanged in spirit.
 * consoleInit brings up vi, nvdrv, a 1280x720 RGBA framebuffer and the
 * IGraphicBufferProducer dance; printf draws into it; consoleUpdate queues the
 * frame. With HOS_FRAMES set, hosrun writes each frame as a PPM.
 *
 * Build: scripts/build_libnx.sh console
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    printf("Hello from switch2pc!\n");
    printf("This text was drawn by libnx into a block-linear framebuffer.\n");
    for (int i = 0; i < 3; i++) {
        printf("frame %d\n", i);
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    const char msg[] = "libnx console: done";
    svcOutputDebugString(msg, sizeof(msg) - 1);
    return 0;
}
