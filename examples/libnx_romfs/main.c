/*
 * libnx romfs: romfsInit mounts the data storage (fsp-srv cmd 200) and parses
 * the romfs tables through IStorage reads; newlib then opens files through
 * libnx's romfs device. With HOS_ROMFS pointing at a directory, hosrun builds
 * the image on the fly.
 *
 * Build: scripts/build_libnx.sh romfs
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result rc = romfsInit();
    say("romfsInit rc=%#x", rc);
    if (R_SUCCEEDED(rc)) {
        FILE *f = fopen("romfs:/hello.txt", "r");
        if (f) {
            char line[64] = {0};
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                say("romfs read: \"%s\"", line);
            }
            fclose(f);
        } else {
            say("FAIL romfs:/hello.txt not found");
        }

        f = fopen("romfs:/sub/dir/nested.bin", "rb");
        if (f) {
            unsigned char bytes[8] = {0};
            size_t n = fread(bytes, 1, sizeof(bytes), f);
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            say("romfs nested: %zu bytes read, first %02x %02x %02x, size %ld", n, bytes[0],
                bytes[1], bytes[2], size);
            fclose(f);
        } else {
            say("FAIL romfs:/sub/dir/nested.bin not found");
        }

        DIR *d = opendir("romfs:/sub");
        if (d) {
            struct dirent *e;
            int count = 0;
            while ((e = readdir(d)) != NULL) {
                say("romfs:/sub entry: %s", e->d_name);
                count++;
            }
            closedir(d);
            say("romfs:/sub has %d entries", count);
        } else {
            say("FAIL romfs:/sub not listable");
        }
        romfsExit();
    }
    say("libnx romfs: done");
    return 0;
}
