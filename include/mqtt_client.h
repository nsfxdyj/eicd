/*
 * mqtt_client.h - MQTT 3.1.1 publisher client.
 *
 * Implements the subset a telemetry publisher needs:
 *   CONNECT/CONNACK (clean session, optional username/password),
 *   PUBLISH with QoS 0 (fire-and-forget) or QoS 1 (PUBACK confirmed),
 *   PINGREQ/PINGRESP keepalive, graceful DISCONNECT.
 *
 * Out of scope: subscriptions, QoS 2, TLS (terminate at the broker;
 * see the README roadmap).
 *
 * One context is owned by one thread (the publisher), so no internal
 * locking is required.
 */
#ifndef EICD_MQTT_CLIENT_H
#define EICD_MQTT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "defs.h"

typedef struct {
    char     host[EICD_HOST_MAX];
    uint16_t port;
    char     client_id[EICD_MQTT_STR_MAX];
    char     username[EICD_MQTT_STR_MAX];
    char     password[EICD_MQTT_STR_MAX];
    uint16_t keepalive_s;
} mqtt_cfg_t;

typedef struct {
    mqtt_cfg_t cfg;
    int        fd;
    bool       connected;
    uint16_t   next_pid;            /* packet identifier for QoS > 0    */
    uint64_t   last_tx_ms;
    uint64_t   last_rx_ms;
    /* statistics */
    uint64_t   pubs_ok;
    uint64_t   pubs_fail;
    uint64_t   connects;
    uint64_t   conn_fails;
} mqtt_client_t;

void mqtt_init(mqtt_client_t *c, const mqtt_cfg_t *cfg);
int  mqtt_connect(mqtt_client_t *c, uint32_t timeout_ms);   /* 0 ok     */
int  mqtt_publish(mqtt_client_t *c, const char *topic,
                  const uint8_t *payload, size_t len,
                  uint8_t qos, bool retain, uint32_t timeout_ms);
int  mqtt_ping(mqtt_client_t *c, uint32_t timeout_ms);
void mqtt_disconnect(mqtt_client_t *c);
bool mqtt_keepalive_due(const mqtt_client_t *c, uint64_t now_ms);

#endif /* EICD_MQTT_CLIENT_H */
