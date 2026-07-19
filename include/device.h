/*
 * device.h - southbound device worker.
 *
 * Each enabled [device.*] section spawns one worker thread that owns a
 * Modbus TCP connection and scans its assigned points on the configured
 * poll interval. Consecutive register points of the same table are
 * coalesced into a single read request (block polling) to minimise
 * transaction count on the control network.
 *
 * Failure handling: any transaction error closes the socket, marks all
 * device points BAD (quality transitions are reported northbound), and
 * the worker backs off for retry_interval before reconnecting. Because
 * every socket operation is timeout-bounded, a worker can never hang
 * indefinitely.
 */
#ifndef EICD_DEVICE_H
#define EICD_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "config.h"
#include "modbus_tcp.h"

struct app_ctx;                     /* defined in app.h */

typedef struct device {
    device_cfg_t    cfg;
    mb_tcp_t        mb;
    size_t         *pidx;           /* heap: point indices, sorted by (func,addr) */
    size_t          pcount;
    struct app_ctx *app;

    pthread_t       thread;
    bool            started;

    /* runtime state (written by the worker, read by the stats logger) */
    volatile bool   online;
    uint64_t        last_ok_ms;
    uint32_t        consec_fail;
    uint64_t        polls_ok;
    uint64_t        polls_fail;
    uint64_t        points_updated;
} device_t;

int  device_init(device_t *d, const device_cfg_t *cfg, struct app_ctx *app);
int  device_start(device_t *d);
void device_stop(device_t *d);      /* joins the worker thread          */
void device_destroy(device_t *d);

#endif /* EICD_DEVICE_H */
