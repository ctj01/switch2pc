#include "common/hos_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hos_log_level g_level = HOS_LOG_INFO;

static const char g_level_char[] = {'E', 'W', 'I', 'D', 'T'};

void hos_log_set_level(hos_log_level level) {
    if (level > HOS_LOG_TRACE) {
        level = HOS_LOG_TRACE;
    }
    g_level = level;
}

hos_log_level hos_log_get_level(void) {
    return g_level;
}

void hos_log_init_from_env(void) {
    const char *env = getenv("HOS_LOG_LEVEL");
    if (!env || !*env) {
        return;
    }
    if (env[0] >= '0' && env[0] <= '4' && env[1] == '\0') {
        hos_log_set_level((hos_log_level)(env[0] - '0'));
        return;
    }
    static const struct {
        const char *name;
        hos_log_level level;
    } names[] = {
        {"error", HOS_LOG_ERROR}, {"warn", HOS_LOG_WARN},   {"info", HOS_LOG_INFO},
        {"debug", HOS_LOG_DEBUG}, {"trace", HOS_LOG_TRACE},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(env, names[i].name) == 0) {
            hos_log_set_level(names[i].level);
            return;
        }
    }
    hos_log(HOS_LOG_WARN, "log", "ignoring unknown HOS_LOG_LEVEL=%s", env);
}

void hos_log(hos_log_level level, const char *module, const char *fmt, ...) {
    if (level > g_level) {
        return;
    }
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[s2pc %-5s] %c: ", module, g_level_char[level]);
    if (n < 0) {
        return;
    }
    size_t used = (size_t)n;
    if (used >= sizeof(buf) - 1) {
        used = sizeof(buf) - 2;
    }
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(buf + used, sizeof(buf) - 1 - used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    used += (size_t)n;
    if (used >= sizeof(buf) - 1) {
        used = sizeof(buf) - 2;
    }
    buf[used++] = '\n';
    /* One write per line. Partial writes to stderr are not worth retrying. */
    ssize_t rc = write(STDERR_FILENO, buf, used);
    (void)rc;
}

void hos_log_guest(hos_log_level level, const char *source, const char *text, size_t len) {
    if (level > g_level) {
        return;
    }
    char line[900];
    size_t n = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? text[i] : '\n';
        if (c == '\n' || c == '\r' || c == '\0' || n == sizeof(line) - 1) {
            if (n) {
                line[n] = '\0';
                hos_log(level, "game", "%s%s%s", source && *source ? source : "",
                        source && *source ? ": " : "", line);
            }
            n = 0;
            if (c == '\0' && i < len) {
                break; /* a C string ends here */
            }
            if (c == '\n' || c == '\r' || c == '\0') {
                continue;
            }
        }
        line[n++] = ((unsigned char)c < 0x20 && c != '\t') || c == 0x7F ? '?' : c;
    }
}
