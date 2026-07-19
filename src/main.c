/*
 * main.c - eicd entry point: argument parsing, startup sequencing,
 * signal handling, the main supervision loop and graceful shutdown.
 *
 * Startup order matters:
 *   1. parse CLI + config            (errors still reach stderr)
 *   2. load point table              (validates against the config)
 *   3. install signal handlers       (before any thread exists)
 *   4. init logger
 *   5. daemonize + PID file          (unless -f)
 *   6. start queue, MQTT, publisher, device workers
 *   7. supervise until SIGTERM/SIGINT
 */
#include "app.h"
#include "daemonize.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static app_ctx_t g_app;

/* ------------------------------------------------------------------ */
/* signals - handlers only set flags, work happens in the main loop    */
/* ------------------------------------------------------------------ */

static void handle_signal(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT:
        g_app.stop_flag = 1;
        break;
    case SIGHUP:
        g_app.reload_flag = 1;
        break;
    default:
        break;
    }
}

static void install_signal_handlers(void)
{
    struct sigaction sa, ign;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    /* no SA_RESTART: let blocking syscalls return EINTR promptly */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* writing to a closed broker/PLC socket must never kill the daemon */
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);
}

/* ------------------------------------------------------------------ */
/* periodic housekeeping                                               */
/* ------------------------------------------------------------------ */

static void log_gateway_stats(app_ctx_t *app)
{
    uint64_t enq, deq, dropped;
    size_t   good = 0, other = 0;

    rb_stats(&app->txq, &enq, &deq, &dropped);

    pthread_mutex_lock(&app->points.mtx);
    for (size_t i = 0; i < app->points.count; i++) {
        if (app->points.items[i].quality == QUALITY_GOOD)
            good++;
        else
            other++;
    }
    pthread_mutex_unlock(&app->points.mtx);

    LOG_INFO("gateway: points good=%zu bad/other=%zu, queue depth=%zu "
             "dropped=%llu", good, other, rb_depth(&app->txq),
             (unsigned long long)dropped);

    for (size_t i = 0; i < app->device_count; i++) {
        const device_t *d = &app->devices[i];
        LOG_INFO("device '%s': %s, polls ok=%llu fail=%llu, "
                 "transactions ok=%llu fail=%llu",
                 d->cfg.name, d->online ? "online" : "OFFLINE",
                 (unsigned long long)d->polls_ok,
                 (unsigned long long)d->polls_fail,
                 (unsigned long long)d->mb.tx_ok,
                 (unsigned long long)d->mb.tx_fail);
    }
}

/*
 * SIGHUP: reopen the log file (logrotate) and re-read the log level.
 * Anything structural (devices, points, broker) still needs a restart -
 * reloading those live is a deliberate non-goal, see README.
 */
static void handle_reload(app_ctx_t *app)
{
    eicd_config_t fresh;

    LOG_INFO("reload requested (SIGHUP): reopening log, refreshing log level");
    log_reopen();

    config_defaults(&fresh);
    if (config_load(&fresh, app->conf_path, false) == 0) {
        log_set_level(fresh.log_level);
        LOG_INFO("reload: log level set to %s",
                 log_level_to_str(fresh.log_level));
    } else {
        LOG_WARN("reload: '%s' has errors, keeping current settings",
                 app->conf_path);
    }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    fprintf(out,
        EICD_PROGNAME " " EICD_VERSION
        " - Edge Industrial Communication Daemon\n"
        "\n"
        "usage: " EICD_PROGNAME " [options]\n"
        "\n"
        "  -c FILE   configuration file (default /etc/eicd/eicd.conf)\n"
        "  -f        foreground mode (do not daemonize; log to stderr)\n"
        "  -t        test configuration and points file, then exit\n"
        "  -v        print version and exit\n"
        "  -h        this help\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *conf_path  = "/etc/eicd/eicd.conf";
    bool        foreground = false;
    bool        test_only  = false;
    int         pidfd      = -1;
    int         opt;

    while ((opt = getopt(argc, argv, "c:ftvh")) != -1) {
        switch (opt) {
        case 'c': conf_path  = optarg; break;
        case 'f': foreground = true;   break;
        case 't': test_only  = true;   break;
        case 'v':
            puts(EICD_PROGNAME " " EICD_VERSION);
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    memset(&g_app, 0, sizeof g_app);
    g_app.foreground = foreground;
    util_strlcpy(g_app.conf_path, conf_path, sizeof g_app.conf_path);

    /* ---- configuration ---- */
    config_defaults(&g_app.cfg);
    if (config_load(&g_app.cfg, conf_path, true) != 0) {
        fprintf(stderr, EICD_PROGNAME ": invalid configuration '%s'\n",
                conf_path);
        return 2;
    }
    g_app.cfg.foreground = foreground;

    /* ---- point table ---- */
    points_init(&g_app.points);
    if (points_load_csv(&g_app.points, g_app.cfg.points_file, &g_app.cfg) != 0 ||
        g_app.points.count == 0) {
        fprintf(stderr, EICD_PROGNAME ": no usable points in '%s'\n",
                g_app.cfg.points_file);
        return 2;
    }

    if (test_only) {
        printf("configuration OK: %zu device(s), %zu point(s)\n",
               g_app.cfg.device_count, g_app.points.count);
        for (size_t i = 0; i < g_app.cfg.device_count; i++) {
            const device_cfg_t *d = &g_app.cfg.devices[i];
            printf("  device '%s': %s:%u unit=%u poll=%ums%s\n",
                   d->name, d->host, d->port, d->unit_id,
                   d->poll_interval_ms, d->enabled ? "" : " (disabled)");
        }
        return 0;
    }

    /* ---- signals, logger, daemon ---- */
    install_signal_handlers();

    if (log_init(EICD_PROGNAME,
                 g_app.cfg.log_file[0] ? g_app.cfg.log_file : NULL,
                 g_app.cfg.log_level, g_app.cfg.log_syslog,
                 g_app.cfg.log_max_bytes, foreground) != 0) {
        fprintf(stderr, EICD_PROGNAME ": cannot open log file '%s'\n",
                g_app.cfg.log_file);
        return 1;
    }

    if (!foreground && daemonize_now() != 0) {
        LOG_ERROR("daemonize failed: %s", strerror(errno));
        return 1;
    }

    util_mkdir_p(g_app.cfg.work_dir, 0750);

    pidfd = pidfile_acquire(g_app.cfg.pid_file);
    if (pidfd < 0) {
        LOG_ERROR("cannot lock pid file '%s' (already running?)",
                  g_app.cfg.pid_file);
        return 1;
    }

    LOG_INFO(EICD_PROGNAME " " EICD_VERSION " starting, pid %d, config %s",
             (int)getpid(), conf_path);
    config_dump(&g_app.cfg);

    /* ---- store-and-forward queue ---- */
    if (rb_init(&g_app.txq, g_app.cfg.queue_capacity) != 0) {
        LOG_ERROR("cannot allocate queue (%zu slots)", g_app.cfg.queue_capacity);
        pidfile_release(g_app.cfg.pid_file, pidfd);
        return 1;
    }

    /* ---- MQTT client ---- */
    mqtt_cfg_t mc;
    memset(&mc, 0, sizeof mc);
    util_strlcpy(mc.host,      g_app.cfg.mqtt_host,      sizeof mc.host);
    mc.port = g_app.cfg.mqtt_port;
    util_strlcpy(mc.client_id, g_app.cfg.mqtt_client_id, sizeof mc.client_id);
    util_strlcpy(mc.username,  g_app.cfg.mqtt_username,  sizeof mc.username);
    util_strlcpy(mc.password,  g_app.cfg.mqtt_password,  sizeof mc.password);
    mc.keepalive_s = g_app.cfg.mqtt_keepalive_s;
    mqtt_init(&g_app.mqtt, &mc);

    /* ---- devices (initialise only; start after the publisher) ---- */
    for (size_t i = 0; i < g_app.cfg.device_count; i++) {
        device_t *d;

        if (!g_app.cfg.devices[i].enabled)
            continue;
        d = &g_app.devices[g_app.device_count];
        if (device_init(d, &g_app.cfg.devices[i], &g_app) != 0)
            continue;
        g_app.device_count++;
    }
    if (g_app.device_count == 0) {
        LOG_ERROR("no enabled device has any points; nothing to do");
        pidfile_release(g_app.cfg.pid_file, pidfd);
        return 1;
    }

    /* ---- threads: publisher first, then producers ---- */
    if (nb_start(&g_app.nb, &g_app) != 0) {
        pidfile_release(g_app.cfg.pid_file, pidfd);
        return 1;
    }
    for (size_t i = 0; i < g_app.device_count; i++)
        device_start(&g_app.devices[i]);

    LOG_INFO("startup complete: %zu device(s), %zu point(s), queue %zu slot(s)",
             g_app.device_count, g_app.points.count, g_app.cfg.queue_capacity);

    /* ---- supervision loop ---- */
    uint64_t last_stats = util_mono_ms();
    while (!g_app.stop_flag) {
        uint64_t now;

        util_sleep_ms(200);

        if (g_app.reload_flag) {
            g_app.reload_flag = 0;
            handle_reload(&g_app);
        }

        now = util_mono_ms();
        if (now - last_stats >= 60000) {
            last_stats = now;
            log_gateway_stats(&g_app);
        }
    }

    /* ---- graceful shutdown: stop producers, then drain the queue ---- */
    LOG_INFO("shutdown requested, stopping device workers...");
    for (size_t i = 0; i < g_app.device_count; i++)
        device_stop(&g_app.devices[i]);

    rb_close(&g_app.txq);               /* no producers left; wake consumer */
    nb_stop(&g_app.nb);                 /* drains up to drain_timeout_s    */

    for (size_t i = 0; i < g_app.device_count; i++)
        device_destroy(&g_app.devices[i]);
    rb_destroy(&g_app.txq);

    LOG_INFO(EICD_PROGNAME " stopped cleanly");
    pidfile_release(g_app.cfg.pid_file, pidfd);
    log_close();
    return 0;
}
