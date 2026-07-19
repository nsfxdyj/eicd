/*
 * config.h - configuration model and INI parser interface.
 *
 * The file format is classic INI. Sections [daemon], [log], [gateway]
 * and [mqtt] appear at most once; every [device.<name>] section defines
 * one southbound device. Unknown keys produce a warning (for forward
 * compatibility); malformed values are fatal. See etc/eicd.conf.example
 * for the annotated reference.
 */
#ifndef EICD_CONFIG_H
#define EICD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "defs.h"
#include "log.h"

typedef struct {
    char     name[EICD_NAME_MAX];
    char     host[EICD_HOST_MAX];
    uint16_t port;                  /* default 502                    */
    uint8_t  unit_id;               /* Modbus unit / slave id         */
    uint32_t poll_interval_ms;
    uint32_t timeout_ms;            /* per-transaction timeout        */
    uint32_t retry_interval_s;      /* reconnect backoff (fixed)      */
    bool     enabled;
} device_cfg_t;

typedef struct {
    /* [daemon] */
    char      pid_file[EICD_PATH_MAX_LEN];
    char      work_dir[EICD_PATH_MAX_LEN];
    bool      foreground;           /* set from the -f CLI flag       */

    /* [log] */
    log_lvl_t log_level;
    char      log_file[EICD_PATH_MAX_LEN];  /* empty -> stderr only   */
    long      log_max_bytes;
    bool      log_syslog;

    /* [gateway] */
    char      points_file[EICD_PATH_MAX_LEN];
    size_t    queue_capacity;       /* store-and-forward ring slots   */
    uint32_t  report_interval_s;    /* integrity report period, 0=off */
    uint32_t  drain_timeout_s;      /* max shutdown drain time        */

    /* [mqtt] */
    char      mqtt_host[EICD_HOST_MAX];
    uint16_t  mqtt_port;
    char      mqtt_client_id[EICD_MQTT_STR_MAX];
    char      mqtt_username[EICD_MQTT_STR_MAX];
    char      mqtt_password[EICD_MQTT_STR_MAX];
    uint16_t  mqtt_keepalive_s;
    uint8_t   mqtt_qos;             /* 0 or 1                         */
    bool      mqtt_retain;
    char      topic_prefix[EICD_TOPIC_MAX];
    uint32_t  reconnect_min_s;      /* exponential backoff bounds     */
    uint32_t  reconnect_max_s;

    /* [device.*] */
    device_cfg_t devices[EICD_MAX_DEVICES];
    size_t       device_count;
} eicd_config_t;

void config_defaults(eicd_config_t *cfg);
int  config_load(eicd_config_t *cfg, const char *path, bool strict);
void config_dump(const eicd_config_t *cfg);
const device_cfg_t *config_find_device(const eicd_config_t *cfg, const char *name);

#endif /* EICD_CONFIG_H */
