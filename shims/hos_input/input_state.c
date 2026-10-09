#include "input_state.h"
#include "hos_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <linux/input.h>
#endif

#include "common/hos_log.h"

#define MOD "input"

/* Offsets measured against libnx's HidSharedMemory with the real compiler. */
#define SHM_NPAD               39424
#define NPAD_ENTRY_SIZE        20480
#define NPAD_STYLE_SET         0
#define NPAD_JOY_ASSIGN        4
#define NPAD_FULL_KEY_LIFO     40
#define NPAD_DEVICE_TYPE       16776
#define NPAD_SYSTEM_PROPERTIES 16784
#define NPAD_BATTERY           16796
#define LIFO_BUFFER_COUNT      8
#define LIFO_TAIL              16
#define LIFO_COUNT             24
#define LIFO_STORAGE           32
#define LIFO_ENTRIES           17
#define STORAGE_SIZE           48
#define STATE_OFF              8
#define STATE_BUTTONS          8
#define STATE_LSTICK           16
#define STATE_RSTICK           24
#define STATE_ATTR             32

#define STYLE_FULL_KEY  1u
#define ATTR_CONNECTED  1u
#define ATTR_WIRED      2u
#define DEVICE_FULL_KEY 1u
#define BATTERY_FULL    4u
#define STICK_MAX       32767

#define SCRIPT_MAX_LINES 4096

typedef struct script_line {
    uint64_t at_ms;
    int exit_request; /* "<ms> exit": ask the game to leave (hos_applet_request_exit) */
    hos_input_state state;
} script_line;

static pthread_t g_thread;
static int g_started;
static uint8_t *g_shm;

static script_line *g_script;
static size_t g_script_count;
static size_t g_script_next;

static int g_evdev_fd = -1;
static hos_input_state g_evdev_state;

/* ---- script source ------------------------------------------------------------ */

static void script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        HOS_LOGW(MOD, "HOS_INPUT_SCRIPT=%s: %s", path, strerror(errno));
        return;
    }
    g_script = mmap(NULL, sizeof(script_line) * SCRIPT_MAX_LINES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_script == MAP_FAILED) {
        g_script = NULL;
        fclose(f);
        return;
    }
    char line[256];
    while (g_script_count < SCRIPT_MAX_LINES && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        script_line *s = &g_script[g_script_count];
        memset(s, 0, sizeof(*s));
        unsigned long long ms = 0, buttons = 0;
        char word[8] = {0};
        if (sscanf(line, "%llu %7s", &ms, word) == 2 && strcmp(word, "exit") == 0) {
            s->at_ms = ms;
            s->exit_request = 1;
            g_script_count++;
            continue;
        }
        int n = sscanf(line, "%llu %llx %d %d %d %d", &ms, &buttons, &s->state.lx, &s->state.ly,
                       &s->state.rx, &s->state.ry);
        if (n < 2) {
            continue;
        }
        s->at_ms = ms;
        s->state.buttons = buttons;
        g_script_count++;
    }
    fclose(f);
    HOS_LOGI(MOD, "input script %s: %zu steps", path, g_script_count);
}

static int script_apply(uint64_t now_ms, hos_input_state *state) {
    int changed = 0;
    while (g_script_next < g_script_count && g_script[g_script_next].at_ms <= now_ms) {
        const script_line *s = &g_script[g_script_next++];
        if (s->exit_request) {
            HOS_LOGI(MOD, "script: exit at %llu ms", (unsigned long long)now_ms);
            hos_applet_request_exit();
            continue;
        }
        *state = s->state;
        changed = 1;
    }
    return changed;
}

/* ---- evdev source ------------------------------------------------------------- */

#ifdef __linux__
static int evdev_is_gamepad(int fd) {
    unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) {
        return 0;
    }
    return (keys[BTN_GAMEPAD / (8 * sizeof(unsigned long))] >>
            (BTN_GAMEPAD % (8 * sizeof(unsigned long)))) &
           1;
}

static void evdev_open(const char *spec) {
    if (strcmp(spec, "auto") != 0) {
        g_evdev_fd = open(spec, O_RDONLY | O_NONBLOCK);
        if (g_evdev_fd < 0) {
            HOS_LOGW(MOD, "HOS_INPUT_EVDEV=%s: %s", spec, strerror(errno));
        }
        return;
    }
    DIR *d = opendir("/dev/input");
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) {
            continue;
        }
        char path[sizeof("/dev/input/") + sizeof(e->d_name)];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (evdev_is_gamepad(fd)) {
            g_evdev_fd = fd;
            HOS_LOGI(MOD, "using gamepad %s", path);
            break;
        }
        close(fd);
    }
    closedir(d);
    if (g_evdev_fd < 0) {
        HOS_LOGW(MOD, "HOS_INPUT_EVDEV=auto: no gamepad found");
    }
}

static int32_t scale_axis(int32_t v, int32_t min, int32_t max, int invert) {
    if (max <= min) {
        return 0;
    }
    int64_t r = ((int64_t)(v - min) * 2 * STICK_MAX) / (max - min) - STICK_MAX;
    if (r > STICK_MAX) {
        r = STICK_MAX;
    }
    if (r < -STICK_MAX) {
        r = -STICK_MAX;
    }
    return (int32_t)(invert ? -r : r);
}

static void evdev_poll(hos_input_state *state) {
    struct input_event ev;
    while (read(g_evdev_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY) {
            int bit = -1;
            switch (ev.code) {
            case BTN_EAST:
                bit = 0; /* A */
                break;
            case BTN_SOUTH:
                bit = 1; /* B */
                break;
            case BTN_NORTH:
                bit = 2; /* X */
                break;
            case BTN_WEST:
                bit = 3; /* Y */
                break;
            case BTN_THUMBL:
                bit = 4;
                break;
            case BTN_THUMBR:
                bit = 5;
                break;
            case BTN_TL:
                bit = 6;
                break;
            case BTN_TR:
                bit = 7;
                break;
            case BTN_TL2:
                bit = 8;
                break;
            case BTN_TR2:
                bit = 9;
                break;
            case BTN_START:
                bit = 10; /* Plus */
                break;
            case BTN_SELECT:
                bit = 11; /* Minus */
                break;
            case BTN_DPAD_LEFT:
                bit = 12;
                break;
            case BTN_DPAD_UP:
                bit = 13;
                break;
            case BTN_DPAD_RIGHT:
                bit = 14;
                break;
            case BTN_DPAD_DOWN:
                bit = 15;
                break;
            default:
                break;
            }
            if (bit >= 0) {
                if (ev.value) {
                    g_evdev_state.buttons |= 1ull << bit;
                } else {
                    g_evdev_state.buttons &= ~(1ull << bit);
                }
            }
        } else if (ev.type == EV_ABS) {
            struct input_absinfo info;
            if (ioctl(g_evdev_fd, EVIOCGABS(ev.code), &info) < 0) {
                continue;
            }
            switch (ev.code) {
            case ABS_X:
                g_evdev_state.lx = scale_axis(ev.value, info.minimum, info.maximum, 0);
                break;
            case ABS_Y:
                g_evdev_state.ly = scale_axis(ev.value, info.minimum, info.maximum, 1);
                break;
            case ABS_RX:
                g_evdev_state.rx = scale_axis(ev.value, info.minimum, info.maximum, 0);
                break;
            case ABS_RY:
                g_evdev_state.ry = scale_axis(ev.value, info.minimum, info.maximum, 1);
                break;
            case ABS_HAT0X:
                g_evdev_state.buttons &= ~((1ull << 12) | (1ull << 14));
                if (ev.value < 0) {
                    g_evdev_state.buttons |= 1ull << 12;
                } else if (ev.value > 0) {
                    g_evdev_state.buttons |= 1ull << 14;
                }
                break;
            case ABS_HAT0Y:
                g_evdev_state.buttons &= ~((1ull << 13) | (1ull << 15));
                if (ev.value < 0) {
                    g_evdev_state.buttons |= 1ull << 13;
                } else if (ev.value > 0) {
                    g_evdev_state.buttons |= 1ull << 15;
                }
                break;
            default:
                break;
            }
        }
    }
    *state = g_evdev_state;
}
#endif

/* ---- shared memory writer ---------------------------------------------------------- */

static void wr32(uint8_t *p, uint32_t v) {
    __atomic_store_n((uint32_t *)p, v, __ATOMIC_RELEASE);
}

static void wr64(uint8_t *p, uint64_t v) {
    __atomic_store_n((uint64_t *)p, v, __ATOMIC_RELEASE);
}

static uint64_t rd64(const uint8_t *p) {
    return __atomic_load_n((const uint64_t *)p, __ATOMIC_ACQUIRE);
}

static void npad_init(uint8_t *npad) {
    wr32(npad + NPAD_STYLE_SET, STYLE_FULL_KEY);
    wr32(npad + NPAD_JOY_ASSIGN, 0);
    wr32(npad + NPAD_DEVICE_TYPE, DEVICE_FULL_KEY);
    wr64(npad + NPAD_SYSTEM_PROPERTIES, 0);
    for (int i = 0; i < 3; i++) {
        wr32(npad + NPAD_BATTERY + i * 4, BATTERY_FULL);
    }
    wr64(npad + NPAD_FULL_KEY_LIFO + LIFO_BUFFER_COUNT, LIFO_ENTRIES);
}

/* Push one sample into a LIFO: write the entry, then publish tail and count. */
static void lifo_push(uint8_t *lifo, uint64_t sample, const hos_input_state *s) {
    uint64_t tail = rd64(lifo + LIFO_TAIL);
    uint64_t count = rd64(lifo + LIFO_COUNT);
    uint64_t next = (tail + 1) % LIFO_ENTRIES;
    uint8_t *entry = lifo + LIFO_STORAGE + next * STORAGE_SIZE;
    uint8_t *state = entry + STATE_OFF;
    wr64(state + 0, sample);
    wr64(state + STATE_BUTTONS, s->buttons);
    wr32(state + STATE_LSTICK, (uint32_t)s->lx);
    wr32(state + STATE_LSTICK + 4, (uint32_t)s->ly);
    wr32(state + STATE_RSTICK, (uint32_t)s->rx);
    wr32(state + STATE_RSTICK + 4, (uint32_t)s->ry);
    wr32(state + STATE_ATTR, ATTR_CONNECTED | ATTR_WIRED);
    wr32(state + STATE_ATTR + 4, 0);
    wr64(entry, sample);
    wr64(lifo + LIFO_TAIL, next);
    wr64(lifo + LIFO_COUNT, count < LIFO_ENTRIES ? count + 1 : LIFO_ENTRIES);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- file source (a viewer writes "<buttons hex> lx ly rx ry", or "quit") --------- */

static const char *g_input_file;

static void file_apply(hos_input_state *state) {
    FILE *f = fopen(g_input_file, "r");
    if (!f) {
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "quit", 4) == 0) {
            fclose(f);
            HOS_LOGI(MOD, "viewer asked to quit");
            exit(0);
        }
        unsigned long long buttons = 0;
        int lx = 0, ly = 0, rx = 0, ry = 0;
        if (sscanf(line, "%llx %d %d %d %d", &buttons, &lx, &ly, &rx, &ry) >= 1) {
            state->buttons = buttons;
            state->lx = lx;
            state->ly = ly;
            state->rx = rx;
            state->ry = ry;
        }
    }
    fclose(f);
}

static void *writer_thread(void *arg) {
    (void)arg;
    uint8_t *npad = g_shm + SHM_NPAD; /* player 1 */
    npad_init(npad);
    hos_input_state state;
    memset(&state, 0, sizeof(state));
    uint64_t sample = 0;
    uint64_t start = now_ms();
    uint64_t last_buttons = 0;
    for (;;) {
        uint64_t t = now_ms() - start;
        if (g_script) {
            script_apply(t, &state);
        }
        if (g_input_file) {
            file_apply(&state);
        }
#ifdef __linux__
        if (g_evdev_fd >= 0) {
            evdev_poll(&state);
        }
#endif
        if (state.buttons != last_buttons) {
            HOS_LOGI(MOD, "buttons %#llx at %llu ms", (unsigned long long)state.buttons,
                     (unsigned long long)t);
            last_buttons = state.buttons;
        }
        lifo_push(npad + NPAD_FULL_KEY_LIFO, ++sample, &state);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16666667};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void hos_input_start(void *shmem_view) {
    if (g_started || !shmem_view) {
        return;
    }
    g_started = 1;
    g_shm = shmem_view;
    const char *script = getenv("HOS_INPUT_SCRIPT");
    if (script && *script) {
        script_load(script);
    }
    const char *file = getenv("HOS_INPUT_FILE");
    if (file && *file) {
        g_input_file = file;
        HOS_LOGI(MOD, "reading pad state from %s", file);
    }
#ifdef __linux__
    const char *evdev = getenv("HOS_INPUT_EVDEV");
    if (evdev && *evdev) {
        evdev_open(evdev);
    }
#endif
    if (pthread_create(&g_thread, NULL, writer_thread, NULL) == 0) {
        pthread_detach(g_thread);
    } else {
        HOS_LOGE(MOD, "cannot start the input thread");
    }
}
