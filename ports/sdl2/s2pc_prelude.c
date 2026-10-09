/*
 * Prelude linked into unmodified SDL2 games run under switch2pc: mounts the
 * romfs (so "romfs:/..." paths work without the game calling romfsInit) and
 * routes stdout/stderr to svcOutputDebugString so the game's own logging
 * shows up in the hosrun log. A constructor, so no game source changes.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/iosupport.h>

static char s2pc_line[512];
static size_t s2pc_line_len;

static void s2pc_flush(void) {
    if (s2pc_line_len) {
        svcOutputDebugString(s2pc_line, s2pc_line_len);
        s2pc_line_len = 0;
    }
}

static ssize_t s2pc_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    (void)r;
    (void)fd;
    for (size_t i = 0; i < len; i++) {
        char c = ptr[i];
        if (c == '\n' || s2pc_line_len == sizeof(s2pc_line) - 1) {
            if (c != '\n') {
                s2pc_line[s2pc_line_len++] = c;
            }
            s2pc_flush();
        } else {
            s2pc_line[s2pc_line_len++] = c;
        }
    }
    return (ssize_t)len;
}

static const devoptab_t s2pc_dotab = {
    .name = "s2pclog",
    .structSize = 0,
    .write_r = s2pc_write,
};

__attribute__((constructor)) static void s2pc_prelude_init(void) {
    devoptab_list[STD_OUT] = &s2pc_dotab;
    devoptab_list[STD_ERR] = &s2pc_dotab;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        fprintf(stderr, "s2pc_prelude: romfsInit failed: 0x%x (pass --romfs to hosrun)\n", rc);
    }
}

__attribute__((destructor)) static void s2pc_prelude_fini(void) {
    fflush(stdout);
    s2pc_flush();
}
