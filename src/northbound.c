/*
 * northbound.c - telemetry rendering and the MQTT publisher thread.
 */
#include "northbound.h"
#include "app.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* rendering (device workers, point table lock held)                   */
/* ------------------------------------------------------------------ */

void nb_report_point(struct app_ctx *app, point_t *p, uint64_t now_ms)
{
    char topic[RB_TOPIC_MAX];
    char valbuf[32];
    char json[RB_PAYLOAD_MAX];
    int  tn, n;

    tn = snprintf(topic, sizeof topic, "%s/%s/%s",
                  app->cfg.topic_prefix, p->device, p->tag);
    if (tn <= 0 || (size_t)tn >= sizeof topic) {
        LOG_ERROR("northbound: topic too long for tag '%s'", p->tag);
        return;
    }

    /*
     * Integer dtypes print exactly (double holds uint32 losslessly);
     * f32 prints with full single-precision round-trip precision.
     */
    if (p->dtype == DT_F32)
        snprintf(valbuf, sizeof valbuf, "%.9g", p->value);
    else
        snprintf(valbuf, sizeof valbuf, "%.0f", p->value);

    n = snprintf(json, sizeof json,
                 "{\"ts\":%llu,\"gw\":\"%s\",\"dev\":\"%s\",\"tag\":\"%s\","
                 "\"value\":%s,\"unit\":\"%s\",\"q\":\"%s\"}",
                 (unsigned long long)now_ms,
                 app->cfg.mqtt_client_id, p->device, p->tag,
                 valbuf, p->unit, quality_str(p->quality));
    if (n <= 0 || (size_t)n >= sizeof json) {
        LOG_ERROR("northbound: payload overflow for tag '%s'", p->tag);
        return;
    }

    if (rb_push(&app->txq, topic, json, (size_t)n) != 0) {
        LOG_ERROR("northbound: cannot enqueue tag '%s'", p->tag);
        return;
    }
    point_mark_reported(p, now_ms);
}

/* ------------------------------------------------------------------ */
/* publisher thread                                                    */
/* ------------------------------------------------------------------ */

static void *publisher_main(void *arg)
{
    nb_publisher_t *nb  = arg;
    app_ctx_t      *app = nb->app;
    mqtt_client_t  *m   = &app->mqtt;
    ringbuf_t      *q   = &app->txq;
    const uint8_t   qos    = app->cfg.mqtt_qos;
    const bool      retain = app->cfg.mqtt_retain;
    const uint32_t  bmin = app->cfg.reconnect_min_s;
    const uint32_t  bmax = app->cfg.reconnect_max_s;
    uint32_t        backoff = bmin;
    uint64_t        drain_deadline = 0;
    uint64_t        last_stats     = util_mono_ms();
    rb_msg_t        msg;

    pthread_setname_np(pthread_self(), "eicd-publisher");

    LOG_INFO("publisher: broker %s:%u, prefix '%s', qos %u, queue %zu slots",
             m->cfg.host, m->cfg.port, app->cfg.topic_prefix,
             (unsigned)qos, q->capacity);

    for (;;) {
        /*
         * Shutdown semantics: keep publishing until the queue is empty
         * or the drain deadline expires; if the broker is unreachable
         * at shutdown, give up immediately rather than fight the timer.
         */
        if (app->stop_flag) {
            if (drain_deadline == 0)
                drain_deadline = util_mono_ms() +
                                 (uint64_t)app->cfg.drain_timeout_s * 1000u;
            if (rb_depth(q) == 0 || util_mono_ms() >= drain_deadline)
                break;
        }

        if (!m->connected) {
            if (mqtt_connect(m, 5000) == 0) {
                LOG_INFO("publisher: connected to %s:%u as '%s'",
                         m->cfg.host, m->cfg.port, m->cfg.client_id);
                backoff = bmin;
            } else {
                if (app->stop_flag) {
                    LOG_WARN("publisher: drain aborted, broker unreachable "
                             "(%zu message(s) left)", rb_depth(q));
                    break;
                }
                LOG_WARN("publisher: broker unreachable, retrying in %u s",
                         backoff);
                app_wait_ms(app, backoff * 1000u);
                backoff = (backoff * 2 > bmax) ? bmax : backoff * 2;
                continue;
            }
        }

        int rc = rb_peek(q, &msg, 500);
        if (rc < 0)
            break;                                  /* closed & drained */

        if (rc == 1) {                              /* idle: keepalive  */
            if (mqtt_keepalive_due(m, util_mono_ms()) &&
                mqtt_ping(m, 5000) != 0) {
                LOG_WARN("publisher: keepalive failed, reconnecting");
                mqtt_disconnect(m);
            }
        } else if (mqtt_publish(m, msg.topic, msg.payload, msg.len,
                                qos, retain, 8000) == 0) {
            rb_commit(q, msg.seq);
            nb->published++;
        } else {
            nb->failed++;
            LOG_WARN("publisher: publish failed, message kept, reconnecting");
            mqtt_disconnect(m);                     /* head not retired */
        }

        uint64_t now = util_mono_ms();
        if (now - last_stats >= 60000) {
            uint64_t enq, deq, dropped;

            last_stats = now;
            rb_stats(q, &enq, &deq, &dropped);
            LOG_INFO("publisher: published=%llu failed=%llu depth=%zu "
                     "enqueued=%llu dequeued=%llu dropped=%llu",
                     (unsigned long long)nb->published,
                     (unsigned long long)nb->failed,
                     rb_depth(q),
                     (unsigned long long)enq,
                     (unsigned long long)deq,
                     (unsigned long long)dropped);
        }
    }

    mqtt_disconnect(m);
    LOG_INFO("publisher: stopped (published=%llu failed=%llu pending=%zu)",
             (unsigned long long)nb->published,
             (unsigned long long)nb->failed,
             rb_depth(q));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int nb_start(nb_publisher_t *nb, struct app_ctx *app)
{
    nb->app       = app;
    nb->published = 0;
    nb->failed    = 0;
    nb->started   = false;

    if (pthread_create(&nb->thread, NULL, publisher_main, nb) != 0) {
        LOG_ERROR("northbound: cannot start publisher thread");
        return -1;
    }
    nb->started = true;
    return 0;
}

void nb_stop(nb_publisher_t *nb)
{
    if (nb->started) {
        pthread_join(nb->thread, NULL);
        nb->started = false;
    }
}
