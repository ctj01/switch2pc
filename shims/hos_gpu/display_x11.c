/*
 * X11 window for the vi null compositor; see display_x11.h for the contract.
 *
 * Threads: the game thread that queues a buffer calls display_x11_present,
 * which scales into an XImage and puts it on the window; an event thread owns
 * the keyboard and window-manager events, so input keeps flowing even while
 * the game is not presenting (loading screens). XInitThreads makes the two
 * safe to share the connection.
 */

#include "display_x11.h"

#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "x11"

#ifdef HAVE_X11

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <poll.h>
#include <pthread.h>

/* Switch button bits (libnx HidNpadButton). */
enum {
    BTN_A = 0,
    BTN_B = 1,
    BTN_X = 2,
    BTN_Y = 3,
    BTN_L = 6,
    BTN_R = 7,
    BTN_PLUS = 10,
    BTN_MINUS = 11,
    BTN_LEFT = 12,
    BTN_UP = 13,
    BTN_RIGHT = 14,
    BTN_DOWN = 15,
};
#define STICK_MAX 32767

typedef struct key_map {
    KeySym sym;
    int bit;   /* button bit, or -1 */
    int stick; /* 0 none, 1 left x-, 2 left x+, 3 left y+, 4 left y- */
} key_map;

static const key_map k_keys[] = {
    {XK_z, BTN_A, 0},
    {XK_Control_L, BTN_A, 0},
    {XK_Control_R, BTN_A, 0},
    {XK_x, BTN_B, 0},
    {XK_space, BTN_B, 0},
    {XK_c, BTN_X, 0},
    {XK_v, BTN_Y, 0},
    {XK_q, BTN_L, 0},
    {XK_e, BTN_R, 0},
    {XK_Return, BTN_PLUS, 0},
    {XK_Escape, BTN_MINUS, 0},
    {XK_Left, BTN_LEFT, 0},
    {XK_Up, BTN_UP, 0},
    {XK_Right, BTN_RIGHT, 0},
    {XK_Down, BTN_DOWN, 0},
    {XK_a, -1, 1},
    {XK_d, -1, 2},
    {XK_w, -1, 3},
    {XK_s, -1, 4},
};
#define NUM_KEYS (sizeof(k_keys) / sizeof(k_keys[0]))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* window size, image */
static int g_state;                                        /* 0 untried, 1 open, -1 off */
static Display *g_dpy;
static Window g_win;
static GC g_gc;
static Visual *g_vis;
static int g_depth;
static Atom g_wm_delete;
static XImage *g_img;
static uint32_t g_win_w, g_win_h;
static int g_shift[3]; /* r, g, b */
static int g_closed;
static uint8_t g_down[NUM_KEYS];
static pthread_t g_events;

static int mask_shift(unsigned long m) {
    int s = 0;
    while (m && !(m & 1)) {
        m >>= 1;
        s++;
    }
    return s;
}

static void publish_pad(void) {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0;
    for (size_t i = 0; i < NUM_KEYS; i++) {
        if (!g_down[i]) {
            continue;
        }
        if (k_keys[i].bit >= 0) {
            buttons |= 1ull << k_keys[i].bit;
        }
        switch (k_keys[i].stick) {
        case 1:
            lx -= STICK_MAX;
            break;
        case 2:
            lx += STICK_MAX;
            break;
        case 3:
            ly += STICK_MAX;
            break;
        case 4:
            ly -= STICK_MAX;
            break;
        default:
            break;
        }
    }
    lx = lx < -STICK_MAX ? -STICK_MAX : lx > STICK_MAX ? STICK_MAX : lx;
    ly = ly < -STICK_MAX ? -STICK_MAX : ly > STICK_MAX ? STICK_MAX : ly;
    hos_pad_external_set(buttons, lx, ly, 0, 0);
}

static void handle_key(XKeyEvent *ev, int down) {
    KeySym sym = XLookupKeysym(ev, 0);
    if (sym >= XK_A && sym <= XK_Z) {
        sym += XK_a - XK_A;
    }
    for (size_t i = 0; i < NUM_KEYS; i++) {
        if (k_keys[i].sym == sym && g_down[i] != down) {
            g_down[i] = (uint8_t)down;
            publish_pad();
        }
    }
}

/* g_lock held. (Re)create the image for the current window size. */
static int make_image(void) {
    if (g_img) {
        XDestroyImage(g_img); /* frees the pixel buffer too */
        g_img = NULL;
    }
    char *pixels = calloc((size_t)g_win_w * g_win_h, 4);
    if (!pixels) {
        return -1;
    }
    g_img =
        XCreateImage(g_dpy, g_vis, (unsigned)g_depth, ZPixmap, 0, pixels, g_win_w, g_win_h, 32, 0);
    if (!g_img || g_img->bits_per_pixel != 32) {
        if (g_img) {
            XDestroyImage(g_img);
            g_img = NULL;
        } else {
            free(pixels);
        }
        return -1;
    }
    return 0;
}

/* A protocol error (say, drawing after another client destroyed the window)
 * must not kill the game the way Xlib's default handler does. */
static int on_x_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    HOS_LOGD(MOD, "X error %u (request %u) ignored", ev->error_code, ev->request_code);
    return 0;
}

static void window_gone(const char *why) {
    if (!g_closed) {
        g_closed = 1;
        HOS_LOGI(MOD, "%s; asking the game to exit", why);
        hos_applet_request_exit();
    }
}

static void *event_thread(void *arg) {
    (void)arg;
    struct pollfd pfd = {.fd = ConnectionNumber(g_dpy), .events = POLLIN};
    for (;;) {
        poll(&pfd, 1, 20);
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            switch (ev.type) {
            case KeyPress:
            case KeyRelease:
                handle_key(&ev.xkey, ev.type == KeyPress);
                break;
            case FocusOut:
                memset(g_down, 0, sizeof(g_down)); /* no stuck keys after alt-tab */
                publish_pad();
                break;
            case ConfigureNotify:
                pthread_mutex_lock(&g_lock);
                if ((uint32_t)ev.xconfigure.width != g_win_w ||
                    (uint32_t)ev.xconfigure.height != g_win_h) {
                    g_win_w = (uint32_t)ev.xconfigure.width;
                    g_win_h = (uint32_t)ev.xconfigure.height;
                    make_image();
                }
                pthread_mutex_unlock(&g_lock);
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == g_wm_delete) {
                    window_gone("window closed");
                }
                break;
            case DestroyNotify:
                window_gone("window destroyed");
                break;
            default:
                break;
            }
        }
    }
    return NULL;
}

static int window_wanted(void) {
    const char *w = getenv("HOS_WINDOW");
    return w && (strcmp(w, "1") == 0 || strcmp(w, "x11") == 0);
}

int display_x11_wanted(void) {
    return window_wanted();
}

/* First frame: open the display and the window at frame size x scale. */
static int open_window(uint32_t fw, uint32_t fh) {
    if (!window_wanted()) {
        return -1;
    }
    if (!XInitThreads()) {
        HOS_LOGW(MOD, "XInitThreads failed");
        return -1;
    }
    XSetErrorHandler(on_x_error);
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        HOS_LOGW(MOD, "cannot open display %s", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return -1;
    }
    int screen = DefaultScreen(g_dpy);
    g_vis = DefaultVisual(g_dpy, screen);
    g_depth = DefaultDepth(g_dpy, screen);
    if (g_vis->class != TrueColor || (g_depth != 24 && g_depth != 32)) {
        HOS_LOGW(MOD, "need a 24/32-bit TrueColor visual (depth %d)", g_depth);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }
    g_shift[0] = mask_shift(g_vis->red_mask);
    g_shift[1] = mask_shift(g_vis->green_mask);
    g_shift[2] = mask_shift(g_vis->blue_mask);

    const char *sc = getenv("HOS_WINDOW_SCALE");
    unsigned scale = sc ? (unsigned)strtoul(sc, NULL, 10) : 2;
    if (scale < 1 || scale > 8) {
        scale = 2;
    }
    g_win_w = fw * scale;
    g_win_h = fh * scale;

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = BlackPixel(g_dpy, screen);
    attrs.event_mask =
        KeyPressMask | KeyReleaseMask | FocusChangeMask | StructureNotifyMask | ExposureMask;
    g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, screen), 0, 0, g_win_w, g_win_h, 0, g_depth,
                          InputOutput, g_vis, CWBackPixel | CWEventMask, &attrs);
    XStoreName(g_dpy, g_win, "switch2pc");
    g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);
    XkbSetDetectableAutoRepeat(g_dpy, True, NULL); /* held keys: no release/press pairs */
    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    if (make_image() != 0) {
        HOS_LOGW(MOD, "cannot allocate a %ux%u image", g_win_w, g_win_h);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }
    XMapWindow(g_dpy, g_win);
    XFlush(g_dpy);
    if (pthread_create(&g_events, NULL, event_thread, NULL) == 0) {
        pthread_detach(g_events);
    }
    hos_pad_external_set(0, 0, 0, 0, 0);
    HOS_LOGI(MOD, "window %ux%u for %ux%u frames", g_win_w, g_win_h, fw, fh);
    return 0;
}

int display_x11_present(const uint8_t *rgb, uint32_t fw, uint32_t fh) {
    if (g_state == 0) {
        g_state = open_window(fw, fh) == 0 ? 1 : -1;
    }
    if (g_state != 1 || g_closed) {
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_img) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    /* Fit the frame into the window, keeping its aspect ratio. */
    uint32_t ww = g_win_w, wh = g_win_h;
    uint32_t dw = ww, dh = (uint32_t)((uint64_t)ww * fh / fw);
    if (dh > wh) {
        dh = wh;
        dw = (uint32_t)((uint64_t)wh * fw / fh);
    }
    uint32_t ox = (ww - dw) / 2, oy = (wh - dh) / 2;
    char *base = g_img->data;
    size_t pitch = (size_t)g_img->bytes_per_line;
    memset(base, 0, pitch * wh);
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *src = rgb + (size_t)((uint64_t)y * fh / dh) * fw * 3;
        uint32_t *dst = (uint32_t *)(base + (size_t)(y + oy) * pitch) + ox;
        for (uint32_t x = 0; x < dw; x++) {
            const uint8_t *p = src + (size_t)((uint64_t)x * fw / dw) * 3;
            dst[x] = ((uint32_t)p[0] << g_shift[0]) | ((uint32_t)p[1] << g_shift[1]) |
                     ((uint32_t)p[2] << g_shift[2]);
        }
    }
    XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0, ww, wh);
    XFlush(g_dpy);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void display_x11_quit(void) {
    /* The event thread may be inside Xlib; leave the connection to process exit. */
    if (g_state == 1 && g_dpy && !g_closed) {
        g_closed = 1;
        XUnmapWindow(g_dpy, g_win);
        XFlush(g_dpy);
    }
}

#else /* !HAVE_X11: built without libX11 for the target */

int display_x11_wanted(void) {
    const char *w = getenv("HOS_WINDOW"); /* so present() can say there is no X11 */
    return w && *w && strcmp(w, "0") != 0;
}

int display_x11_present(const uint8_t *rgb, uint32_t width, uint32_t height) {
    static int warned;
    (void)rgb;
    (void)width;
    (void)height;
    const char *w = getenv("HOS_WINDOW");
    if (w && *w && strcmp(w, "0") != 0 && !warned) {
        warned = 1;
        HOS_LOGW(MOD, "HOS_WINDOW is set but this build has no X11 (install libx11-dev:arm64)");
    }
    return -1;
}

void display_x11_quit(void) {
}

#endif /* HAVE_X11 */
