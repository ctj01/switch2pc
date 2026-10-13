/*
 * bsd:u / bsd:s: the socket service, over host sockets (#39). HOS's network
 * stack is FreeBSD-derived and libnx passes its FreeBSD headers' values
 * straight through: sockaddr starts with a length byte, and the MSG_*,
 * SOL_SOCKET/SO_*, IP_* and POLLWR* constants are FreeBSD's, so those are
 * translated here. Two values are not FreeBSD's and were found by running
 * libnx against this service: fcntl's O_NONBLOCK arrives as 0x800, and errno
 * is expected in Linux numbering (libnx maps it to newlib's), so host errno
 * passes through unchanged. Only IPv4 addresses are supported.
 *
 * Calls that can wait (connect, accept, recv*, send*, poll, read, write)
 * release the IPC request lock while they wait, so other game threads keep
 * running; the guest-to-host descriptor table has its own lock for that.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "bsd"

#define MAX_SOCKETS 256
#define MAX_POLL    64

/* ---- FreeBSD (and HOS) constants ----------------------------------------------------------- */

enum {
    BSD_AF_INET = 2,
    BSD_SOCK_CLOEXEC = 0x10000000,
    BSD_SOCK_NONBLOCK = 0x20000000,
    BSD_O_NONBLOCK = 0x800, /* HOS value (what libnx sends), not FreeBSD 0x4 */
    BSD_SOL_SOCKET = 0xffff,
    BSD_MSG_DONTWAIT = 0x80,
    BSD_MSG_WAITALL = 0x40,
    BSD_MSG_NOSIGNAL = 0x20000,
    BSD_POLLWRNORM = 0x4,
    BSD_POLLWRBAND = 0x100,
};

typedef struct opt_map {
    int bsd, linux_;
} opt_map;

static const opt_map k_so[] = {
    {0x0001, SO_DEBUG},     {0x0002, SO_ACCEPTCONN}, {0x0004, SO_REUSEADDR}, {0x0008, SO_KEEPALIVE},
    {0x0010, SO_DONTROUTE}, {0x0020, SO_BROADCAST},  {0x0080, SO_LINGER},    {0x0100, SO_OOBINLINE},
    {0x0200, SO_REUSEPORT}, {0x1001, SO_SNDBUF},     {0x1002, SO_RCVBUF},    {0x1003, SO_SNDLOWAT},
    {0x1004, SO_RCVLOWAT},  {0x1005, SO_SNDTIMEO},   {0x1006, SO_RCVTIMEO},  {0x1007, SO_ERROR},
    {0x1008, SO_TYPE},
};
static const opt_map k_ip[] = {
    {3, IP_TOS},
    {4, IP_TTL},
    {9, IP_MULTICAST_IF},
    {10, IP_MULTICAST_TTL},
    {11, IP_MULTICAST_LOOP},
    {12, IP_ADD_MEMBERSHIP},
    {13, IP_DROP_MEMBERSHIP},
};
static const opt_map k_tcp[] = {
    {1, TCP_NODELAY},       {2, TCP_MAXSEG},      {0x100, TCP_KEEPIDLE},
    {0x200, TCP_KEEPINTVL}, {0x400, TCP_KEEPCNT},
};

static int map_opt(const opt_map *m, size_t n, int bsd, int *out) {
    for (size_t i = 0; i < n; i++) {
        if (m[i].bsd == bsd) {
            *out = m[i].linux_;
            return 1;
        }
    }
    return 0;
}

/* (level, option) from FreeBSD to Linux; 0 if unknown. */
static int sockopt_to_linux(int level, int opt, int *llevel, int *lopt) {
    switch (level) {
    case BSD_SOL_SOCKET:
        *llevel = SOL_SOCKET;
        return map_opt(k_so, sizeof(k_so) / sizeof(k_so[0]), opt, lopt);
    case IPPROTO_IP:
        *llevel = IPPROTO_IP;
        return map_opt(k_ip, sizeof(k_ip) / sizeof(k_ip[0]), opt, lopt);
    case IPPROTO_TCP:
        *llevel = IPPROTO_TCP;
        return map_opt(k_tcp, sizeof(k_tcp) / sizeof(k_tcp[0]), opt, lopt);
    default:
        *llevel = level;
        *lopt = opt;
        return 1;
    }
}

static int msg_flags_to_linux(int f) {
    int out = f & (MSG_OOB | MSG_PEEK);
    if (f & BSD_MSG_DONTWAIT) {
        out |= MSG_DONTWAIT;
    }
    if (f & BSD_MSG_WAITALL) {
        out |= MSG_WAITALL;
    }
    return out | MSG_NOSIGNAL; /* a closed peer must not SIGPIPE the whole emulator */
}

static short poll_to_linux(short e) {
    short out = (short)(e & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL |
                             POLLRDNORM | POLLRDBAND));
    if (e & BSD_POLLWRBAND) {
        out |= POLLWRBAND;
    }
    return out;
}

static short poll_to_bsd(short e) {
    short out = (short)(e & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL |
                             POLLRDNORM | POLLRDBAND));
    if (e & POLLWRNORM) {
        out |= BSD_POLLWRNORM;
    }
    if (e & POLLWRBAND) {
        out |= BSD_POLLWRBAND;
    }
    return out;
}

/* FreeBSD sockaddr_in: u8 len, u8 family, u16 port, u32 addr, 8 zero bytes. */
static int addr_to_linux(const uint8_t *b, size_t n, struct sockaddr_in *out) {
    if (n < 8 || b[1] != BSD_AF_INET) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, b + 2, 2);
    memcpy(&out->sin_addr, b + 4, 4);
    return 0;
}

/* Returns the FreeBSD length written (16), clipped to cap. */
static uint32_t addr_to_bsd(const struct sockaddr_in *in, uint8_t *b, size_t cap) {
    uint8_t tmp[16];
    memset(tmp, 0, sizeof(tmp));
    tmp[0] = sizeof(tmp);
    tmp[1] = BSD_AF_INET;
    memcpy(tmp + 2, &in->sin_port, 2);
    memcpy(tmp + 4, &in->sin_addr, 4);
    memcpy(b, tmp, cap < sizeof(tmp) ? cap : sizeof(tmp));
    return sizeof(tmp);
}

/* ---- descriptor table ------------------------------------------------------------ */

static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fds[MAX_SOCKETS]; /* host fd + 1; 0 = free */

static int fd_alloc(int host) {
    pthread_mutex_lock(&g_fd_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_fds[i]) {
            g_fds[i] = host + 1;
            pthread_mutex_unlock(&g_fd_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_fd_lock);
    return -1;
}

static int fd_host(int guest) {
    if (guest < 0 || guest >= MAX_SOCKETS) {
        return -1;
    }
    pthread_mutex_lock(&g_fd_lock);
    int h = g_fds[guest] - 1;
    pthread_mutex_unlock(&g_fd_lock);
    return h;
}

static int fd_release(int guest) {
    pthread_mutex_lock(&g_fd_lock);
    int h = (guest >= 0 && guest < MAX_SOCKETS) ? g_fds[guest] - 1 : -1;
    if (h >= 0) {
        g_fds[guest] = 0;
    }
    pthread_mutex_unlock(&g_fd_lock);
    return h;
}

/* ---- IPC helpers ----------------------------------------------------------------- */

/* With HipcAutoSelect every buffer comes as an A/X (in) or B/C (out) pair, one null. */
static hos_ipc_buffer in_buf(const hos_ipc_ctx *ctx, uint32_t k) {
    if (k < ctx->num_a && ctx->a[k].addr) {
        return ctx->a[k];
    }
    if (k < ctx->num_x && ctx->x[k].addr) {
        return ctx->x[k];
    }
    return (hos_ipc_buffer){0, 0};
}

static hos_ipc_buffer out_buf(const hos_ipc_ctx *ctx, uint32_t k) {
    if (k < ctx->num_b && ctx->b[k].addr) {
        return ctx->b[k];
    }
    if (k < ctx->num_c && ctx->c[k].addr) {
        return ctx->c[k];
    }
    return (hos_ipc_buffer){0, 0};
}

static int32_t in_i32(const hos_ipc_ctx *ctx, size_t idx) {
    int32_t v = 0;
    hos_ipc_in_read(ctx, idx * 4, &v, 4);
    return v;
}

/* Every bsd reply starts with (s32 ret, s32 errno); some add a u32 length. */
static hos_result reply(hos_ipc_ctx *ctx, long ret, int host_errno) {
    int32_t out[2] = {(int32_t)ret, ret < 0 ? host_errno : 0};
    hos_ipc_out_write(ctx, out, sizeof(out));
    return HOS_RESULT_SUCCESS;
}

static hos_result reply_bsd(hos_ipc_ctx *ctx, int err) {
    int32_t out[2] = {-1, err};
    hos_ipc_out_write(ctx, out, sizeof(out));
    return HOS_RESULT_SUCCESS;
}

static hos_result reply_len(hos_ipc_ctx *ctx, long ret, int host_errno, uint32_t len) {
    reply(ctx, ret, host_errno);
    hos_ipc_out_write(ctx, &len, 4);
    return HOS_RESULT_SUCCESS;
}

#define HOST_OR_EBADF(guest, host)                                                                 \
    int host = fd_host(guest);                                                                     \
    if (host < 0) {                                                                                \
        return reply_bsd(ctx, EBADF);                                                              \
    }

/* ---- commands -------------------------------------------------------------------- */

static hos_result do_socket(hos_ipc_ctx *ctx) {
    int domain = in_i32(ctx, 0), type = in_i32(ctx, 1), proto = in_i32(ctx, 2);
    if (domain != BSD_AF_INET) {
        return reply_bsd(ctx, EAFNOSUPPORT);
    }
    int ltype = type & 0xFFFF;
    if (type & BSD_SOCK_NONBLOCK) {
        ltype |= SOCK_NONBLOCK;
    }
    int h = socket(AF_INET, ltype | SOCK_CLOEXEC, proto);
    if (h < 0) {
        return reply(ctx, -1, errno);
    }
    int g = fd_alloc(h);
    if (g < 0) {
        close(h);
        return reply_bsd(ctx, EMFILE);
    }
    HOS_LOGD(MOD, "socket(%d, %#x, %d) -> %d", domain, type, proto, g);
    return reply(ctx, g, 0);
}

static hos_result do_addr_call(hos_ipc_ctx *ctx, int is_connect) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    hos_ipc_buffer b = in_buf(ctx, 0);
    struct sockaddr_in sa;
    if (!b.addr || addr_to_linux((const uint8_t *)b.addr, b.size, &sa) != 0) {
        return reply_bsd(ctx, EAFNOSUPPORT);
    }
    int r;
    if (is_connect) {
        hos_ipc_blocking_begin();
        r = connect(h, (struct sockaddr *)&sa, sizeof(sa));
        int e = errno;
        hos_ipc_blocking_end();
        return reply(ctx, r, e);
    }
    r = bind(h, (struct sockaddr *)&sa, sizeof(sa));
    return reply(ctx, r, errno);
}

static hos_result do_name(hos_ipc_ctx *ctx, int peer) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    int r = peer ? getpeername(h, (struct sockaddr *)&sa, &len)
                 : getsockname(h, (struct sockaddr *)&sa, &len);
    int e = errno;
    uint32_t blen = 0;
    hos_ipc_buffer b = out_buf(ctx, 0);
    if (r == 0 && b.addr) {
        blen = addr_to_bsd(&sa, (uint8_t *)b.addr, b.size);
    }
    return reply_len(ctx, r, e, blen);
}

static hos_result do_accept(hos_ipc_ctx *ctx) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    hos_ipc_blocking_begin();
    int nh = accept4(h, (struct sockaddr *)&sa, &len, SOCK_CLOEXEC);
    int e = errno;
    hos_ipc_blocking_end();
    if (nh < 0) {
        return reply_len(ctx, -1, e, 0);
    }
    int g = fd_alloc(nh);
    if (g < 0) {
        close(nh);
        reply_bsd(ctx, EMFILE);
        uint32_t zero = 0;
        hos_ipc_out_write(ctx, &zero, 4);
        return HOS_RESULT_SUCCESS;
    }
    uint32_t blen = 0;
    hos_ipc_buffer b = out_buf(ctx, 0);
    if (b.addr) {
        blen = addr_to_bsd(&sa, (uint8_t *)b.addr, b.size);
    }
    return reply_len(ctx, g, 0, blen);
}

static hos_result do_recv(hos_ipc_ctx *ctx, int with_addr, int is_read) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    int flags = is_read ? MSG_NOSIGNAL : msg_flags_to_linux(in_i32(ctx, 1));
    hos_ipc_buffer b = out_buf(ctx, 0);
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    hos_ipc_blocking_begin();
    ssize_t n = recvfrom(h, (void *)b.addr, (size_t)b.size, flags,
                         with_addr ? (struct sockaddr *)&sa : NULL, with_addr ? &len : NULL);
    int e = errno;
    hos_ipc_blocking_end();
    if (!with_addr) {
        return reply(ctx, n, e);
    }
    uint32_t blen = 0;
    hos_ipc_buffer ab = out_buf(ctx, 1);
    if (n >= 0 && ab.addr && sa.sin_family == AF_INET) {
        blen = addr_to_bsd(&sa, (uint8_t *)ab.addr, ab.size);
    }
    return reply_len(ctx, n, e, blen);
}

static hos_result do_send(hos_ipc_ctx *ctx, int with_addr, int is_write) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    int flags = is_write ? MSG_NOSIGNAL : msg_flags_to_linux(in_i32(ctx, 1));
    hos_ipc_buffer b = in_buf(ctx, 0);
    struct sockaddr_in sa;
    if (with_addr) {
        hos_ipc_buffer ab = in_buf(ctx, 1);
        if (!ab.addr || addr_to_linux((const uint8_t *)ab.addr, ab.size, &sa) != 0) {
            return reply_bsd(ctx, EAFNOSUPPORT);
        }
    }
    hos_ipc_blocking_begin();
    ssize_t n = sendto(h, (const void *)b.addr, (size_t)b.size, flags,
                       with_addr ? (struct sockaddr *)&sa : NULL, with_addr ? sizeof(sa) : 0);
    int e = errno;
    hos_ipc_blocking_end();
    return reply(ctx, n, e);
}

static hos_result do_poll(hos_ipc_ctx *ctx) {
    int nfds = in_i32(ctx, 0), timeout = in_i32(ctx, 1);
    hos_ipc_buffer ib = in_buf(ctx, 0), ob = out_buf(ctx, 0);
    if (nfds < 0 || nfds > MAX_POLL || (nfds && (!ib.addr || !ob.addr)) ||
        ib.size < (uint64_t)nfds * sizeof(struct pollfd) ||
        ob.size < (uint64_t)nfds * sizeof(struct pollfd)) {
        return reply_bsd(ctx, EINVAL);
    }
    struct pollfd g[MAX_POLL], hfds[MAX_POLL];
    memcpy(g, (const void *)ib.addr, (size_t)nfds * sizeof(struct pollfd));
    for (int i = 0; i < nfds; i++) {
        hfds[i].fd = g[i].fd < 0 ? -1 : fd_host(g[i].fd);
        hfds[i].events = poll_to_linux(g[i].events);
        hfds[i].revents = 0;
    }
    hos_ipc_blocking_begin();
    int r = poll(hfds, (nfds_t)nfds, timeout);
    int e = errno;
    hos_ipc_blocking_end();
    for (int i = 0; i < nfds; i++) {
        int invalid = g[i].fd >= 0 && hfds[i].fd < 0;
        g[i].revents = invalid ? POLLNVAL : poll_to_bsd(hfds[i].revents);
        if (invalid && r >= 0) {
            r++;
        }
    }
    memcpy((void *)ob.addr, g, (size_t)nfds * sizeof(struct pollfd));
    return reply(ctx, r, e);
}

static hos_result do_sockopt(hos_ipc_ctx *ctx, int set) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    int level = in_i32(ctx, 1), opt = in_i32(ctx, 2), llevel, lopt;
    if (!sockopt_to_linux(level, opt, &llevel, &lopt)) {
        HOS_LOGW(MOD, "%ssockopt(level %#x, option %#x) unknown", set ? "set" : "get", level, opt);
        reply_bsd(ctx, ENOPROTOOPT);
        if (!set) {
            uint32_t zero = 0;
            hos_ipc_out_write(ctx, &zero, 4);
        }
        return HOS_RESULT_SUCCESS;
    }
    if (set) {
        hos_ipc_buffer b = in_buf(ctx, 0);
        int r = setsockopt(h, llevel, lopt, (const void *)b.addr, (socklen_t)b.size);
        return reply(ctx, r, errno);
    }
    hos_ipc_buffer b = out_buf(ctx, 0);
    uint8_t tmp[64];
    socklen_t len = b.size < sizeof(tmp) ? (socklen_t)b.size : (socklen_t)sizeof(tmp);
    int r = getsockopt(h, llevel, lopt, tmp, &len);
    int e = errno; /* SO_ERROR's value is an errno too: Linux numbering, as is */
    if (r == 0 && b.addr) {
        memcpy((void *)b.addr, tmp, len);
    }
    return reply_len(ctx, r, e, r == 0 ? len : 0);
}

static hos_result do_fcntl(hos_ipc_ctx *ctx) {
    HOST_OR_EBADF(in_i32(ctx, 0), h);
    int cmd = in_i32(ctx, 1), arg = in_i32(ctx, 2);
    if (cmd == F_GETFL) {
        int fl = fcntl(h, F_GETFL);
        if (fl < 0) {
            return reply(ctx, -1, errno);
        }
        return reply(ctx, (fl & O_NONBLOCK) ? BSD_O_NONBLOCK | 2 /* O_RDWR */ : 2, 0);
    }
    if (cmd == F_SETFL) {
        int fl = fcntl(h, F_GETFL);
        fl = (arg & BSD_O_NONBLOCK) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        return reply(ctx, fcntl(h, F_SETFL, fl), errno);
    }
    return reply_bsd(ctx, EINVAL);
}

static hos_result do_close(hos_ipc_ctx *ctx) {
    int h = fd_release(in_i32(ctx, 0));
    if (h < 0) {
        return reply_bsd(ctx, EBADF);
    }
    return reply(ctx, close(h), errno);
}

static hos_result bsd_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* RegisterClient(config, pid, tmem size, tmem) -> u64 pid */ {
        uint64_t pid = ctx->has_pid ? ctx->pid : 0x50;
        hos_ipc_out_write(ctx, &pid, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* StartMonitoring(pid) */
        return HOS_RESULT_SUCCESS;
    case 2: /* Socket */
    case 3: /* SocketExempt */
        return do_socket(ctx);
    case 6:
        return do_poll(ctx);
    case 8: /* Recv */
        return do_recv(ctx, 0, 0);
    case 9: /* RecvFrom */
        return do_recv(ctx, 1, 0);
    case 10: /* Send */
        return do_send(ctx, 0, 0);
    case 11: /* SendTo */
        return do_send(ctx, 1, 0);
    case 12:
        return do_accept(ctx);
    case 13: /* Bind */
        return do_addr_call(ctx, 0);
    case 14: /* Connect */
        return do_addr_call(ctx, 1);
    case 15: /* GetPeerName */
        return do_name(ctx, 1);
    case 16: /* GetSockName */
        return do_name(ctx, 0);
    case 17: /* GetSockOpt */
        return do_sockopt(ctx, 0);
    case 18: /* Listen */ {
        HOST_OR_EBADF(in_i32(ctx, 0), h);
        return reply(ctx, listen(h, in_i32(ctx, 1)), errno);
    }
    case 20:
        return do_fcntl(ctx);
    case 21: /* SetSockOpt */
        return do_sockopt(ctx, 1);
    case 22: /* Shutdown(fd, how): SHUT_RD/WR/RDWR are 0/1/2 on both */ {
        HOST_OR_EBADF(in_i32(ctx, 0), h);
        return reply(ctx, shutdown(h, in_i32(ctx, 1)), errno);
    }
    case 24: /* Write */
        return do_send(ctx, 0, 1);
    case 25: /* Read */
        return do_recv(ctx, 0, 1);
    case 26:
        return do_close(ctx);
    case 27: /* DuplicateSocket(fd, owner pid) */ {
        HOST_OR_EBADF(in_i32(ctx, 0), h);
        int nh = fcntl(h, F_DUPFD_CLOEXEC, 0);
        if (nh < 0) {
            return reply(ctx, -1, errno);
        }
        int g = fd_alloc(nh);
        if (g < 0) {
            close(nh);
            return reply_bsd(ctx, EMFILE);
        }
        return reply(ctx, g, 0);
    }
    case 4:  /* Open (special files) */
    case 5:  /* Select */
    case 7:  /* Sysctl */
    case 19: /* Ioctl */
    case 23: /* ShutdownAllSockets */
        HOS_LOGW(MOD, "bsd command %u is not supported", ctx->cmd);
        return reply_bsd(ctx, EOPNOTSUPP);
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_bsd_class = {
    .name = "bsd:u",
    .dispatch = bsd_dispatch,
    .destroy = NULL,
};
