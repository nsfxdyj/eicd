/*
 * northbound.h - telemetry reporting and the MQTT publisher thread.
 *
 * nb_report_point() is called by device workers (with the point table
 * lock held) whenever a point meets its reporting criteria; it renders
 * the JSON frame and pushes it into the store-and-forward queue.
 *
 * The publisher thread owns the MQTT client and drains the queue with
 * peek/commit semantics, exponential reconnect backoff and keepalive
 * maintenance. On shutdown it drains remaining messages up to the
 * configured drain_timeout.
 */
#ifndef EICD_NORTHBOUND_H
#define EICD_NORTHBOUND_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "points.h"

struct app_ctx;                     /* defined in app.h */

typedef struct {
    struct app_ctx *app;
    pthread_t       thread;
    bool            started;
    uint64_t        published;
    uint64_t        failed;
} nb_publisher_t;

int  nb_start(nb_publisher_t *nb, struct app_ctx *app);
void nb_stop(nb_publisher_t *nb);   /* joins the publisher thread       */

/* called with the point table lock held */
void nb_report_point(struct app_ctx *app, point_t *p, uint64_t now_ms);

#endif /* EICD_NORTHBOUND_H */
