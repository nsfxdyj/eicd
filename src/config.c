/*
 * config.c - INI parser and configuration validation.
 *
 * config_load() may run before the logger exists (startup) or long after
 * (SIGHUP reload), so parse diagnostics go to stderr while semantic
 * findings are reported by the caller via the logger where possible.
 */
#include "config.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* defaults                                                            */
/* ------------------------------------------------------------------ */

static void device_cfg_defaults(device_cfg_t *d, const char *name)
{
    memset(d, 0, sizeof *d);
    util_strlcpy(d->name, name, sizeof d->name);
    d->port             = 502;
    d->unit_id          = 1;
    d->poll_interval_ms = 1000;
    d->timeout_ms       = 3000;
    d->retry_interval_s = 5;
    d->enabled          = true;
}

void config_defaults(eicd_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    util_strlcpy(cfg->pid_file, "/var/lib/eicd/eicd.pid", sizeof cfg->pid_file);
    util_strlcpy(cfg->work_dir, "/var/lib/eicd",          sizeof cfg->work_dir);

    cfg->log_level     = LOG_LVL_INFO;
    cfg->log_file[0]   = '\0';              /* stderr */
    cfg->log_max_bytes = 10L * 1024 * 1024;
    cfg->log_syslog    = false;

    util_strlcpy(cfg->points_file, "/etc/eicd/points.csv",
                 sizeof cfg->points_file);
    cfg->queue_capacity    = 1024;
    cfg->report_interval_s = 60;
    cfg->drain_timeout_s   = 10;

    cfg->mqtt_host[0] = '\0';               /* required, no default */
    cfg->mqtt_port    = 1883;
    util_strlcpy(cfg->mqtt_client_id, "eicd-gateway",
                 sizeof cfg->mqtt_client_id);
    cfg->mqtt_keepalive_s = 60;
    cfg->mqtt_qos         = 1;
    cfg->mqtt_retain      = false;
    util_strlcpy(cfg->topic_prefix, "eicd/gateway", sizeof cfg->topic_prefix);
    cfg->reconnect_min_s = 1;
    cfg->reconnect_max_s = 60;

    cfg->device_count = 0;
}

/* ------------------------------------------------------------------ */
/* minimal INI reader                                                  */
/* ------------------------------------------------------------------ */

typedef int (*ini_handler)(void *user, const char *section,
                           const char *key, const char *value,
                           unsigned lineno);

static int ini_parse(const char *path, ini_handler cb, void *user)
{
    FILE     *fp;
    char      raw[512];
    char      section[128] = "";
    unsigned  lineno       = 0;
    int       errors       = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, EICD_PROGNAME ": cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(raw, sizeof raw, fp) != NULL) {
        char *s;

        lineno++;
        if (strchr(raw, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, EICD_PROGNAME ": %s:%u: line too long\n",
                    path, lineno);
            errors++;
            continue;
        }

        s = util_trim(raw);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close == NULL) {
                fprintf(stderr, EICD_PROGNAME ": %s:%u: unterminated section\n",
                        path, lineno);
                errors++;
                continue;
            }
            *close = '\0';
            util_strlcpy(section, util_trim(s + 1), sizeof section);
            continue;
        }

        char *eq = strchr(s, '=');
        if (eq == NULL) {
            fprintf(stderr, EICD_PROGNAME ": %s:%u: expected key = value\n",
                    path, lineno);
            errors++;
            continue;
        }
        *eq = '\0';

        if (cb(user, section, util_trim(s), util_trim(eq + 1), lineno) != 0)
            errors++;
    }

    fclose(fp);
    return (errors == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* semantic handlers                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    eicd_config_t *cfg;
    const char    *path;
    bool           strict;
} parse_ctx_t;

static int cfg_err(const parse_ctx_t *pc, unsigned lineno,
                   const char *key, const char *msg)
{
    fprintf(stderr, EICD_PROGNAME ": %s:%u: %s: %s\n",
            pc->path, lineno, key, msg);
    return -1;
}

static device_cfg_t *find_or_add_device(eicd_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->device_count; i++) {
        if (strcmp(cfg->devices[i].name, name) == 0)
            return &cfg->devices[i];
    }
    if (cfg->device_count >= EICD_MAX_DEVICES)
        return NULL;
    device_cfg_defaults(&cfg->devices[cfg->device_count], name);
    return &cfg->devices[cfg->device_count++];
}

/* parse a bounded integer config value */
static int cfg_long(const parse_ctx_t *pc, unsigned lineno, const char *key,
                    const char *val, long min, long max, long *out)
{
    long v;

    if (!util_parse_long(val, &v) || v < min || v > max) {
        char msg[96];
        snprintf(msg, sizeof msg, "invalid value '%s' (range %ld..%ld)",
                 val, min, max);
        return cfg_err(pc, lineno, key, msg);
    }
    *out = v;
    return 0;
}

static int handle_key(void *user, const char *section, const char *key,
                      const char *val, unsigned lineno)
{
    parse_ctx_t  *pc  = user;
    eicd_config_t *c   = pc->cfg;
    long           num;
    bool           b;

    if (section[0] == '\0')
        return cfg_err(pc, lineno, key, "key outside any section");

    /* ---- [daemon] ---- */
    if (strcmp(section, "daemon") == 0) {
        if (strcmp(key, "pid_file") == 0)
            util_strlcpy(c->pid_file, val, sizeof c->pid_file);
        else if (strcmp(key, "work_dir") == 0)
            util_strlcpy(c->work_dir, val, sizeof c->work_dir);
        else
            goto unknown;
        return 0;
    }

    /* ---- [log] ---- */
    if (strcmp(section, "log") == 0) {
        if (strcmp(key, "level") == 0) {
            if (!log_level_valid(val))
                return cfg_err(pc, lineno, key, "expected debug|info|warn|error");
            c->log_level = log_level_from_str(val);
        } else if (strcmp(key, "file") == 0) {
            util_strlcpy(c->log_file, val, sizeof c->log_file);
        } else if (strcmp(key, "max_size_mb") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 1024, &num) != 0)
                return -1;
            c->log_max_bytes = num * 1024 * 1024;
        } else if (strcmp(key, "syslog") == 0) {
            if (!util_parse_bool(val, &b))
                return cfg_err(pc, lineno, key, "expected boolean");
            c->log_syslog = b;
        } else {
            goto unknown;
        }
        return 0;
    }

    /* ---- [gateway] ---- */
    if (strcmp(section, "gateway") == 0) {
        if (strcmp(key, "points_file") == 0) {
            util_strlcpy(c->points_file, val, sizeof c->points_file);
        } else if (strcmp(key, "queue_capacity") == 0) {
            if (cfg_long(pc, lineno, key, val, 16, 1000000, &num) != 0)
                return -1;
            c->queue_capacity = (size_t)num;
        } else if (strcmp(key, "report_interval_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 0, 86400, &num) != 0)
                return -1;
            c->report_interval_s = (uint32_t)num;
        } else if (strcmp(key, "drain_timeout_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 0, 300, &num) != 0)
                return -1;
            c->drain_timeout_s = (uint32_t)num;
        } else {
            goto unknown;
        }
        return 0;
    }

    /* ---- [mqtt] ---- */
    if (strcmp(section, "mqtt") == 0) {
        if (strcmp(key, "host") == 0) {
            util_strlcpy(c->mqtt_host, val, sizeof c->mqtt_host);
        } else if (strcmp(key, "port") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 65535, &num) != 0)
                return -1;
            c->mqtt_port = (uint16_t)num;
        } else if (strcmp(key, "client_id") == 0) {
            util_strlcpy(c->mqtt_client_id, val, sizeof c->mqtt_client_id);
        } else if (strcmp(key, "username") == 0) {
            util_strlcpy(c->mqtt_username, val, sizeof c->mqtt_username);
        } else if (strcmp(key, "password") == 0) {
            util_strlcpy(c->mqtt_password, val, sizeof c->mqtt_password);
        } else if (strcmp(key, "keepalive_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 5, 3600, &num) != 0)
                return -1;
            c->mqtt_keepalive_s = (uint16_t)num;
        } else if (strcmp(key, "qos") == 0) {
            if (cfg_long(pc, lineno, key, val, 0, 1, &num) != 0)
                return -1;
            c->mqtt_qos = (uint8_t)num;
        } else if (strcmp(key, "retain") == 0) {
            if (!util_parse_bool(val, &b))
                return cfg_err(pc, lineno, key, "expected boolean");
            c->mqtt_retain = b;
        } else if (strcmp(key, "topic_prefix") == 0) {
            util_strlcpy(c->topic_prefix, val, sizeof c->topic_prefix);
        } else if (strcmp(key, "reconnect_min_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 3600, &num) != 0)
                return -1;
            c->reconnect_min_s = (uint32_t)num;
        } else if (strcmp(key, "reconnect_max_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 86400, &num) != 0)
                return -1;
            c->reconnect_max_s = (uint32_t)num;
        } else {
            goto unknown;
        }
        return 0;
    }

    /* ---- [device.<name>] ---- */
    if (strncmp(section, "device.", 7) == 0) {
        const char   *name = section + 7;
        device_cfg_t *d;

        if (*name == '\0' || strlen(name) >= EICD_NAME_MAX)
            return cfg_err(pc, lineno, section, "invalid device name");
        d = find_or_add_device(c, name);
        if (d == NULL)
            return cfg_err(pc, lineno, section, "too many devices");

        if (strcmp(key, "host") == 0) {
            util_strlcpy(d->host, val, sizeof d->host);
        } else if (strcmp(key, "port") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 65535, &num) != 0)
                return -1;
            d->port = (uint16_t)num;
        } else if (strcmp(key, "unit_id") == 0) {
            if (cfg_long(pc, lineno, key, val, 0, 247, &num) != 0)
                return -1;
            d->unit_id = (uint8_t)num;
        } else if (strcmp(key, "poll_interval_ms") == 0) {
            if (cfg_long(pc, lineno, key, val, 50, 3600000, &num) != 0)
                return -1;
            d->poll_interval_ms = (uint32_t)num;
        } else if (strcmp(key, "timeout_ms") == 0) {
            if (cfg_long(pc, lineno, key, val, 100, 60000, &num) != 0)
                return -1;
            d->timeout_ms = (uint32_t)num;
        } else if (strcmp(key, "retry_interval_s") == 0) {
            if (cfg_long(pc, lineno, key, val, 1, 3600, &num) != 0)
                return -1;
            d->retry_interval_s = (uint32_t)num;
        } else if (strcmp(key, "enabled") == 0) {
            if (!util_parse_bool(val, &b))
                return cfg_err(pc, lineno, key, "expected boolean");
            d->enabled = b;
        } else {
            goto unknown;
        }
        return 0;
    }

    fprintf(stderr, EICD_PROGNAME ": %s:%u: warning: unknown section [%s]%s\n",
            pc->path, lineno, section, pc->strict ? " (fatal)" : "");
    return pc->strict ? -1 : 0;

unknown:
    fprintf(stderr, EICD_PROGNAME ": %s:%u: warning: unknown key '%s' in [%s]%s\n",
            pc->path, lineno, key, section, pc->strict ? " (fatal)" : "");
    return pc->strict ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* validation                                                          */
/* ------------------------------------------------------------------ */

static int config_validate(const eicd_config_t *c, const char *path)
{
    int errors = 0;

    if (c->points_file[0] == '\0') {
        fprintf(stderr, EICD_PROGNAME ": %s: [gateway] points_file is required\n", path);
        errors++;
    }
    if (c->mqtt_host[0] == '\0') {
        fprintf(stderr, EICD_PROGNAME ": %s: [mqtt] host is required\n", path);
        errors++;
    }
    if (c->mqtt_client_id[0] == '\0' || !util_is_json_safe(c->mqtt_client_id)) {
        fprintf(stderr, EICD_PROGNAME ": %s: [mqtt] client_id is empty or contains "
                "characters unsafe for JSON payloads\n", path);
        errors++;
    }
    if (c->topic_prefix[0] == '\0' || strchr(c->topic_prefix, '#') != NULL ||
        strchr(c->topic_prefix, '+') != NULL) {
        fprintf(stderr, EICD_PROGNAME ": %s: [mqtt] topic_prefix must be a valid, "
                "non-wildcard publish prefix\n", path);
        errors++;
    }
    if (c->reconnect_max_s < c->reconnect_min_s) {
        fprintf(stderr, EICD_PROGNAME ": %s: [mqtt] reconnect_max_s < reconnect_min_s\n",
                path);
        errors++;
    }
    if (c->device_count == 0) {
        fprintf(stderr, EICD_PROGNAME ": %s: at least one [device.<name>] section "
                "is required\n", path);
        errors++;
    }

    size_t enabled = 0;
    for (size_t i = 0; i < c->device_count; i++) {
        const device_cfg_t *d = &c->devices[i];

        if (d->host[0] == '\0') {
            fprintf(stderr, EICD_PROGNAME ": %s: [device.%s] host is required\n",
                    path, d->name);
            errors++;
        }
        if (d->enabled)
            enabled++;
    }
    if (c->device_count > 0 && enabled == 0) {
        fprintf(stderr, EICD_PROGNAME ": %s: all devices are disabled\n", path);
        errors++;
    }

    return (errors == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int config_load(eicd_config_t *cfg, const char *path, bool strict)
{
    parse_ctx_t pc = { .cfg = cfg, .path = path, .strict = strict };

    if (ini_parse(path, handle_key, &pc) != 0)
        return -1;

    /* cosmetic: topic prefix must not end with '/' */
    size_t plen = strlen(cfg->topic_prefix);
    while (plen > 0 && cfg->topic_prefix[plen - 1] == '/')
        cfg->topic_prefix[--plen] = '\0';

    return config_validate(cfg, path);
}

const device_cfg_t *config_find_device(const eicd_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->device_count; i++) {
        if (strcmp(cfg->devices[i].name, name) == 0)
            return &cfg->devices[i];
    }
    return NULL;
}

void config_dump(const eicd_config_t *c)
{
    LOG_INFO("config: work_dir=%s pid_file=%s", c->work_dir, c->pid_file);
    LOG_INFO("config: mqtt=%s:%u client_id=%s username=%s password=%s "
             "keepalive=%us qos=%u retain=%s prefix=%s",
             c->mqtt_host, c->mqtt_port, c->mqtt_client_id,
             c->mqtt_username[0] ? c->mqtt_username : "(none)",
             c->mqtt_password[0] ? "***" : "(none)",
             c->mqtt_keepalive_s, c->mqtt_qos,
             c->mqtt_retain ? "true" : "false", c->topic_prefix);
    LOG_INFO("config: points_file=%s queue_capacity=%zu report_interval=%us "
             "drain_timeout=%us log_level=%s",
             c->points_file, c->queue_capacity, c->report_interval_s,
             c->drain_timeout_s, log_level_to_str(c->log_level));

    for (size_t i = 0; i < c->device_count; i++) {
        const device_cfg_t *d = &c->devices[i];
        LOG_INFO("config: device[%zu] '%s' %s:%u unit=%u poll=%ums "
                 "timeout=%ums retry=%us %s",
                 i, d->name, d->host, d->port, d->unit_id,
                 d->poll_interval_ms, d->timeout_ms, d->retry_interval_s,
                 d->enabled ? "enabled" : "DISABLED");
    }
}
