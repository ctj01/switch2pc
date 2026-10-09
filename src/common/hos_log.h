#ifndef HOS_COMMON_LOG_H
#define HOS_COMMON_LOG_H

/*
 * One logger for the tool, the loader and every shim. Writes a single line to
 * stderr per call, so lines from different threads never interleave.
 *
 * Level comes from the HOS_LOG_LEVEL environment variable: a digit 0-4 or one
 * of error, warn, info, debug, trace. Default is info.
 */

typedef enum hos_log_level {
    HOS_LOG_ERROR = 0,
    HOS_LOG_WARN = 1,
    HOS_LOG_INFO = 2,
    HOS_LOG_DEBUG = 3,
    HOS_LOG_TRACE = 4,
} hos_log_level;

/**
 * Set the current log level.
 */
void hos_log_set_level(hos_log_level level);

/**
 * Return the current log level.
 */
hos_log_level hos_log_get_level(void);

/**
 * Read HOS_LOG_LEVEL from the environment and apply it. Safe to call more than once.
 */
void hos_log_init_from_env(void);

/**
 * Emit one log line if `level` is enabled.
 *
 * @param level   Severity
 * @param module  Short module tag, e.g. "nso", "cpu"
 * @param fmt     printf format
 */
void hos_log(hos_log_level level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define HOS_LOGE(mod, ...) hos_log(HOS_LOG_ERROR, mod, __VA_ARGS__)
#define HOS_LOGW(mod, ...) hos_log(HOS_LOG_WARN, mod, __VA_ARGS__)
#define HOS_LOGI(mod, ...) hos_log(HOS_LOG_INFO, mod, __VA_ARGS__)
#define HOS_LOGD(mod, ...) hos_log(HOS_LOG_DEBUG, mod, __VA_ARGS__)
#define HOS_LOGT(mod, ...) hos_log(HOS_LOG_TRACE, mod, __VA_ARGS__)

#endif /* HOS_COMMON_LOG_H */
