/*
 * log.h - leveled, thread-safe logging to stderr, a file with size
 * rotation, and/or syslog.
 *
 * One global logger: log_init() once at startup before any thread logs,
 * log_reopen() from the SIGHUP path (logrotate). The LOG_* macros wrap
 * log_write(); messages below the current level are dropped before any
 * formatting work happens.
 */
#ifndef EICD_LOG_H
#define EICD_LOG_H

#include <stdbool.h>

#define LOG_IDENT_MAX   32

typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR
} log_lvl_t;

#define LOG_DEBUG(...) log_write(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LVL_ERROR, __VA_ARGS__)

/* path NULL/empty logs to stderr only; returns -1 if the file cannot open */
int         log_init(const char *ident, const char *path, log_lvl_t lvl,
                     bool use_syslog, long max_bytes, bool foreground);
void        log_reopen(void);
void        log_close(void);
void        log_write(log_lvl_t lvl, const char *fmt, ...)
                      __attribute__((format(printf, 2, 3)));
void        log_set_level(log_lvl_t lvl);

log_lvl_t   log_level_from_str(const char *s);  /* unknown -> LOG_LVL_INFO */
const char *log_level_to_str(log_lvl_t lvl);
bool        log_level_valid(const char *s);

#endif /* EICD_LOG_H */
