/*
 * Sockets through libnx's own BSD client (bsd:u): a TCP listener and client
 * on loopback inside one process, poll, non-blocking reads, and UDP. Uses the
 * plain POSIX API that homebrew uses; hosrun serves it with host sockets.
 *
 * Build: scripts/build_libnx.sh sockets
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>

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

static struct sockaddr_in loopback(uint16_t port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    return sa;
}

static int tcp_test(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = loopback(0);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(srv, 4) != 0) {
        say("FAIL bind/listen errno=%d", errno);
        return 0;
    }
    socklen_t len = sizeof(sa);
    getsockname(srv, (struct sockaddr *)&sa, &len);
    uint16_t port = ntohs(sa.sin_port);
    say("tcp: listening on port %u", port);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = loopback(port);
    if (connect(cli, (struct sockaddr *)&to, sizeof(to)) != 0) {
        say("FAIL connect errno=%d", errno);
        return 0;
    }
    struct sockaddr_in peer;
    len = sizeof(peer);
    int acc = accept(srv, (struct sockaddr *)&peer, &len);
    char ip[32];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    say("tcp: accepted %d from %s", acc >= 0, ip);

    send(cli, "ping", 4, 0);
    struct pollfd pfd = {.fd = acc, .events = POLLIN};
    int ready = poll(&pfd, 1, 1000);
    char buf[16] = {0};
    ssize_t n = recv(acc, buf, sizeof(buf) - 1, 0);
    say("tcp: poll %d revents %#x, received %zd \"%s\"", ready, pfd.revents, n, buf);

    fcntl(acc, F_SETFL, fcntl(acc, F_GETFL) | O_NONBLOCK);
    n = recv(acc, buf, sizeof(buf), 0);
    int recv_errno = errno;
    say("tcp: empty non-blocking recv %zd would-block %d (errno %d)", n,
        n < 0 && (recv_errno == EWOULDBLOCK || recv_errno == EAGAIN), recv_errno);

    close(cli);
    n = recv(acc, buf, sizeof(buf), 0);
    say("tcp: after peer close recv %zd", n);
    close(acc);
    close(srv);
    return strcmp(buf, "ping") == 0;
}

static int udp_test(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = loopback(0);
    bind(s, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t len = sizeof(sa);
    getsockname(s, (struct sockaddr *)&sa, &len);
    sendto(s, "datagram", 8, 0, (struct sockaddr *)&sa, sizeof(sa));
    char buf[16] = {0};
    struct sockaddr_in from;
    len = sizeof(from);
    ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &len);
    say("udp: received %zd \"%s\" from port %u", n, buf, ntohs(from.sin_port));
    close(s);
    return n == 8 && ntohs(from.sin_port) == ntohs(sa.sin_port);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result rc = socketInitializeDefault();
    say("socketInitializeDefault rc=%#x", rc);
    if (R_FAILED(rc)) {
        say("FAIL sockets unavailable");
        return 1;
    }
    int ok = tcp_test();
    ok &= udp_test();
    socketExit();
    say(ok ? "libnx sockets: done" : "FAIL libnx sockets");
    return 0;
}
