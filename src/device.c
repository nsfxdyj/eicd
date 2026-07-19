/*
 * device.c - southbound polling engine. See device.h for the model.
 */
#include "device.h"
#include "app.h"
#include "log.h"
#include "northbound.h"
#include "points.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* point index sorting: by (function, address) so block polling can    */
/* coalesce consecutive registers of the same table                    */
/* ------------------------------------------------------------------ */

static int cmp_pidx(const void *a, const void *b, void *thunk)
{
    const point_table_t *t  = thunk;
    const point_t       *pa = &t->items[*(const size_t *)a];
    const point_t       *pb = &t->items[*(const size_t *)b];

    if (pa->func != pb->func)
        return (int)pa->func - (int)pb->func;
    return (int)pa->addr - (int)pb->addr;
}

/* ------------------------------------------------------------------ */
/* quality propagation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Mark every point of the device BAD and report the transition. Called
 * once per connect/read failure; points already reported BAD do not
 * re-report until the integrity interval expires, so a dead PLC does
 * not flood the broker.
 */
static void mark_all_bad(device_t *d, uint64_t now_ms)
{
    app_ctx_t     *app = d->app;
    point_table_t *t   = &app->points;
    uint32_t       integ_ms = app->cfg.report_interval_s * 1000u;

    pthread_mutex_lock(&t->mtx);
    for (size_t i = 0; i < d->pcount; i++) {
        point_t *p = &t->items[d->pidx[i]];

        p->quality = QUALITY_BAD;
        if (point_should_report(p, now_ms, integ_ms))
            nb_report_point(app, p, now_ms);
    }
    pthread_mutex_unlock(&t->mtx);
}

/* ------------------------------------------------------------------ */
/* one scan cycle                                                      */
/* ------------------------------------------------------------------ */

static int poll_once(device_t *d)
{
    app_ctx_t     *app       = d->app;
    point_table_t *t         = &app->points;
    const uint32_t integ_ms  = app->cfg.report_interval_s * 1000u;
    const uint64_t now       = util_epoch_ms();
    size_t         i         = 0;

    while (i < d->pcount) {
        point_t *first = &t->items[d->pidx[i]];

        /* ---- bit access: one read per point (addresses are discrete) */
        if (first->func == MB_FC_COIL || first->func == MB_FC_DISCRETE) {
            uint8_t bits[1] = { 0 };
            int     rc;

            rc = mb_tcp_read_bits(&d->mb, (uint8_t)first->func,
                                  first->addr, 1, bits);
            if (rc != MB_OK) {
                LOG_WARN("device '%s': read %s '%s' failed: %s",
                         d->cfg.name, mb_func_str(first->func), first->tag,
                         mb_status_str(&d->mb, rc));
                return -1;
            }

            pthread_mutex_lock(&t->mtx);
            point_apply_bit(first, (bits[0] & 0x01) != 0, now);
            if (point_should_report(first, now, integ_ms))
                nb_report_point(app, first, now);
            pthread_mutex_unlock(&t->mtx);

            d->points_updated++;
            i++;
            continue;
        }

        /* ---- register access: coalesce consecutive addresses ---- */
        const uint16_t start = first->addr;
        uint16_t       end   = start + (uint16_t)point_reg_span(first);
        size_t         j     = i + 1;

        while (j < d->pcount) {
            const point_t *cand = &t->items[d->pidx[j]];
            uint16_t       nend;

            if (cand->func != first->func || cand->addr != end)
                break;
            nend = cand->addr + (uint16_t)point_reg_span(cand);
            if (nend - start > EICD_MB_MAX_READ_REGS)
                break;
            end = nend;
            j++;
        }

        const uint16_t qty = (uint16_t)(end - start);
        uint16_t       regs[EICD_MB_MAX_READ_REGS];
        int            rc;

        rc = mb_tcp_read_regs(&d->mb, (uint8_t)first->func, start, qty, regs);
        if (rc != MB_OK) {
            LOG_WARN("device '%s': read %s @%u+%u failed: %s",
                     d->cfg.name, mb_func_str(first->func),
                     (unsigned)start, (unsigned)qty,
                     mb_status_str(&d->mb, rc));
            return -1;
        }

        pthread_mutex_lock(&t->mtx);
        for (size_t k = i; k < j; k++) {
            point_t *p   = &t->items[d->pidx[k]];
            size_t   off = (size_t)(p->addr - start);

            point_apply_regs(p, regs + off, qty - off, now);
            if (point_should_report(p, now, integ_ms))
                nb_report_point(app, p, now);
            d->points_updated++;
        }
        pthread_mutex_unlock(&t->mtx);

        i = j;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* worker thread                                                       */
/* ------------------------------------------------------------------ */

static void *worker_main(void *arg)
{
    device_t  *d   = arg;
    app_ctx_t *app = d->app;
    char       tname[16];

    snprintf(tname, sizeof tname, "dev-%.11s", d->cfg.name);
    pthread_setname_np(pthread_self(), tname);

    LOG_INFO("device '%s': polling %s:%u (unit %u) every %u ms, %zu point(s)",
             d->cfg.name, d->cfg.host, d->cfg.port, d->cfg.unit_id,
             d->cfg.poll_interval_ms, d->pcount);

    while (!app_should_stop(app)) {
        const uint64_t cycle_start = util_mono_ms();

        if (!mb_tcp_connected(&d->mb)) {
            if (mb_tcp_connect(&d->mb) != MB_OK) {
                if (d->online) {
                    LOG_WARN("device '%s': connection to %s:%u lost",
                             d->cfg.name, d->cfg.host, d->cfg.port);
                    d->online = false;
                }
                mark_all_bad(d, util_epoch_ms());
                app_wait_ms(app, d->cfg.retry_interval_s * 1000u);
                continue;
            }
            LOG_INFO("device '%s': connected to %s:%u",
                     d->cfg.name, d->cfg.host, d->cfg.port);
        }

        if (poll_once(d) == 0) {
            if (!d->online)
                LOG_INFO("device '%s': back online", d->cfg.name);
            d->online      = true;
            d->consec_fail = 0;
            d->polls_ok++;
            d->last_ok_ms  = util_mono_ms();
        } else {
            d->polls_fail++;
            d->consec_fail++;
            if (d->online)
                LOG_WARN("device '%s': marking device offline", d->cfg.name);
            d->online = false;
            mb_tcp_close(&d->mb);           /* force a clean reconnect   */
            mark_all_bad(d, util_epoch_ms());
        }

        const uint64_t elapsed = util_mono_ms() - cycle_start;
        if (elapsed < d->cfg.poll_interval_ms)
            app_wait_ms(app, d->cfg.poll_interval_ms - (uint32_t)elapsed);
    }

    mb_tcp_close(&d->mb);
    LOG_INFO("device '%s': worker stopped (polls ok=%llu fail=%llu)",
             d->cfg.name,
             (unsigned long long)d->polls_ok,
             (unsigned long long)d->polls_fail);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int device_init(device_t *d, const device_cfg_t *cfg, struct app_ctx *app)
{
    size_t n = 0, k = 0;

    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    d->app = app;
    mb_tcp_init(&d->mb, cfg->host, cfg->port, cfg->unit_id, cfg->timeout_ms);

    /* collect the indices of all points assigned to this device */
    for (size_t i = 0; i < app->points.count; i++) {
        if (strcmp(app->points.items[i].device, cfg->name) == 0)
            n++;
    }
    if (n == 0) {
        LOG_WARN("device '%s': no points assigned, worker will not start",
                 cfg->name);
        return -1;
    }

    d->pidx = malloc(n * sizeof(size_t));
    if (d->pidx == NULL) {
        LOG_ERROR("device '%s': out of memory", cfg->name);
        return -1;
    }
    for (size_t i = 0; i < app->points.count; i++) {
        if (strcmp(app->points.items[i].device, cfg->name) == 0)
            d->pidx[k++] = i;
    }
    d->pcount = n;

    /* sorted so block polling sees consecutive addresses adjacently */
    qsort_r(d->pidx, n, sizeof(size_t), cmp_pidx, &app->points);
    return 0;
}

int device_start(device_t *d)
{
    if (pthread_create(&d->thread, NULL, worker_main, d) != 0) {
        LOG_ERROR("device '%s': cannot start worker thread", d->cfg.name);
        return -1;
    }
    d->started = true;
    return 0;
}

void device_stop(device_t *d)
{
    if (d->started) {
        pthread_join(d->thread, NULL);
        d->started = false;
    }
}

void device_destroy(device_t *d)
{
    free(d->pidx);
    d->pidx   = NULL;
    d->pcount = 0;
}
