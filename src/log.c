/*
 * log.c - implementation of the eicd logger. See log.h for the model.
 */
#include "log.h"
#include "defs.h"
#include "util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* log.h's LOG_DEBUG/LOG_INFO macros collide with syslog's level constants;
 * drop ours here so lvl_to_syslog() sees the real syslog values */
#undef LOG_DEBUG
#undef LOG_INFO
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define LOG_MSG_MAX   1536
#define LOG_LINE_MAX  1792
#define LOG_PATH_MAX  256

typedef struct {
    pthread_mutex_t mtx;
    log_lvl_t       level;
    char            ident[LOG_IDENT_MAX];
    char            path[LOG_PATH_MAX];
    FILE           *fp;
    long            offset;         /* bytes written to current file   */
    long            max_bytes;      /* rotation threshold              */
    bool            to_syslog;
    bool            foreground;
    bool            color;          /* ANSI colors on stderr           */
} logger_t;

static logger_t g_log = {
    .mtx   = PTHREAD_MUTEX_INITIALIZER,
    .level = LOG_LVL_INFO,
};

static const char *const g_lvl_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };

/* ------------------------------------------------------------------ */
/* level helpers                                                       */
/* ------------------------------------------------------------------ */

log_lvl_t log_level_from_str(const char *s)
{
    if (s == NULL)
        return LOG_LVL_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_LVL_DEBUG;
    if (strcasecmp(s, "info")  == 0) return LOG_LVL_INFO;
    if (strcasecmp(s, "warn")  == 0 || strcasecmp(s, "warning") == 0)
        return LOG_LVL_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_LVL_ERROR;
    return LOG_LVL_INFO;
}

bool log_level_valid(const char *s)
{
    if (s == NULL)
        return false;
    return strcasecmp(s, "debug") == 0 || strcasecmp(s, "info") == 0 ||
           strcasecmp(s, "warn")  == 0 || strcasecmp(s, "warning") == 0 ||
           strcasecmp(s, "error") == 0;
}

const char *log_level_to_str(log_lvl_t lvl)
{
    if (lvl < LOG_LVL_DEBUG || lvl > LOG_LVL_ERROR)
        return "INFO";
    return g_lvl_names[lvl];
}

void log_set_level(log_lvl_t lvl)
{
    pthread_mutex_lock(&g_log.mtx);
    g_log.level = lvl;
    pthread_mutex_unlock(&g_log.mtx);
}

static int lvl_to_syslog(log_lvl_t lvl)
{
    switch (lvl) {
    case LOG_LVL_DEBUG: return LOG_DEBUG;
    case LOG_LVL_INFO:  return LOG_INFO;
    case LOG_LVL_WARN:  return LOG_WARNING;
    default:            return LOG_ERR;
    }
}

static const char *lvl_color(log_lvl_t lvl)
{
    switch (lvl) {
    case LOG_LVL_DEBUG: return "\033[90m";      /* bright black  */
    case LOG_LVL_INFO:  return "\033[0m";       /* default       */
    case LOG_LVL_WARN:  return "\033[33m";      /* yellow        */
    default:            return "\033[31m";      /* red           */
    }
}

/* ------------------------------------------------------------------ */
/* file handling (called with the mutex held)                          */
/* ------------------------------------------------------------------ */

static void rotate_locked(void)
{
    char backup[LOG_PATH_MAX + 8];

    if (g_log.fp != NULL) {
        fclose(g_log.fp);
        g_log.fp = NULL;
    }
    snprintf(backup, sizeof backup, "%s.1", g_log.path);
    rename(g_log.path, backup);             /* best effort, keeps 1 gen */
    g_log.fp = fopen(g_log.path, "a");
    g_log.offset = 0;
}

static void reopen_locked(void)
{
    if (g_log.path[0] == '\0')
        return;
    if (g_log.fp != NULL)
        fclose(g_log.fp);
    g_log.fp = fopen(g_log.path, "a");
    if (g_log.fp != NULL) {
        if (fseek(g_log.fp, 0, SEEK_END) == 0)
            g_log.offset = ftell(g_log.fp);
        else
            g_log.offset = 0;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int log_init(const char *ident, const char *path, log_lvl_t lvl,
             bool use_syslog, long max_bytes, bool foreground)
{
    pthread_mutex_lock(&g_log.mtx);

    util_strlcpy(g_log.ident, (ident != NULL) ? ident : EICD_PROGNAME,
                 sizeof g_log.ident);
    g_log.level      = lvl;
    g_log.max_bytes  = (max_bytes > 0) ? max_bytes : 10L * 1024 * 1024;
    g_log.to_syslog  = use_syslog;
    g_log.foreground = foreground;
    g_log.color      = foreground && isatty(STDERR_FILENO);
    g_log.path[0]    = '\0';
    g_log.fp         = NULL;
    g_log.offset     = 0;

    if (path != NULL && path[0] != '\0') {
        util_strlcpy(g_log.path, path, sizeof g_log.path);
        g_log.fp = fopen(path, "a");
        if (g_log.fp == NULL) {
            pthread_mutex_unlock(&g_log.mtx);
            return -1;
        }
        if (fseek(g_log.fp, 0, SEEK_END) == 0)
            g_log.offset = ftell(g_log.fp);
    }

    if (use_syslog)
        openlog(g_log.ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);

    pthread_mutex_unlock(&g_log.mtx);
    return 0;
}

void log_reopen(void)
{
    pthread_mutex_lock(&g_log.mtx);
    reopen_locked();
    pthread_mutex_unlock(&g_log.mtx);
}

void log_close(void)
{
    pthread_mutex_lock(&g_log.mtx);
    if (g_log.fp != NULL) {
        fclose(g_log.fp);
        g_log.fp = NULL;
    }
    if (g_log.to_syslog)
        closelog();
    pthread_mutex_unlock(&g_log.mtx);
}

void log_write(log_lvl_t lvl, const char *fmt, ...)
{
    char            msg[LOG_MSG_MAX];
    char            line[LOG_LINE_MAX];
    char            tsbuf[32];
    struct timespec ts;
    struct tm       tmv;
    va_list         ap;
    int             n;

    /* cheap pre-check; races with log_set_level are benign */
    if (lvl < g_log.level)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    strftime(tsbuf, sizeof tsbuf, "%Y-%m-%d %H:%M:%S", &tmv);

    n = snprintf(line, sizeof line, "%s.%03ld %-5s [%s] %s\n",
                 tsbuf, ts.tv_nsec / 1000000L,
                 g_lvl_names[lvl], g_log.ident, msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof line - 1;

    pthread_mutex_lock(&g_log.mtx);

    if (g_log.foreground || (g_log.fp == NULL && !g_log.to_syslog)) {
        if (g_log.color)
            fprintf(stderr, "%s%s\033[0m", lvl_color(lvl), line);
        else
            fputs(line, stderr);
        fflush(stderr);
    }

    if (g_log.fp != NULL) {
        if (g_log.offset + n > g_log.max_bytes)
            rotate_locked();
        if (g_log.fp != NULL) {
            if (fwrite(line, 1, (size_t)n, g_log.fp) == (size_t)n)
                g_log.offset += n;
            fflush(g_log.fp);
        }
    }

    if (g_log.to_syslog)
        syslog(lvl_to_syslog(lvl), "%s", msg);

    pthread_mutex_unlock(&g_log.mtx);
}
