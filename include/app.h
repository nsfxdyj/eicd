/*
 * app.h - application context shared by all eicd modules.
 *
 * One static instance lives in main.c (g_app); signal handlers only
 * touch the two sig_atomic_t flags, everything else is driven from the
 * main loop and the worker threads.
 *
 * Lock order (must be respected to avoid deadlock):
 *   point_table_t.mtx  ->  ringbuf_t.mtx
 * i.e. code holding the point table lock may push into the queue, never
 * the other way around. The MQTT client takes no internal locks.
 */
#ifndef EICD_APP_H
#define EICD_APP_H

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>

#include "defs.h"
#include "config.h"
#include "points.h"
#include "ringbuf.h"
#include "mqtt_client.h"
#include "device.h"
#include "northbound.h"
#include "util.h"

typedef struct app_ctx {
    eicd_config_t         cfg;
    char                  conf_path[EICD_PATH_MAX_LEN];
    bool                  foreground;

    point_table_t         points;
    ringbuf_t             txq;          /* southbound -> northbound      */
    mqtt_client_t         mqtt;
    nb_publisher_t        nb;

    device_t              devices[EICD_MAX_DEVICES];
    size_t                device_count;

    volatile sig_atomic_t stop_flag;    /* SIGTERM/SIGINT                */
    volatile sig_atomic_t reload_flag;  /* SIGHUP                        */
} app_ctx_t;

static inline bool app_should_stop(const app_ctx_t *app)
{
    return app->stop_flag != 0;
}

/*
 * Interruptible sleep in ~100 ms slices. Returns true if the stop flag
 * was raised during the wait, false when the full interval elapsed.
 * Keeps shutdown latency below ~100 ms regardless of poll/retry timers.
 */
static inline bool app_wait_ms(const app_ctx_t *app, uint32_t ms)
{
    uint64_t until = util_mono_ms() + ms;
    while (!app->stop_flag) {
        uint64_t now = util_mono_ms();
        if (now >= until)
            return false;
        uint64_t rem = until - now;
        util_sleep_ms((uint32_t)(rem > 100 ? 100 : rem));
    }
    return true;
}

#endif /* EICD_APP_H */
