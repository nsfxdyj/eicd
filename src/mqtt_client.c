/*
 * mqtt_client.c - MQTT 3.1.1 publisher client. See mqtt_client.h for scope.
 *
 * Packet layout reminder (fixed header):
 *   byte 0: packet type << 4 | flags
 *   bytes : "remaining length" as a base-128 varint (1..4 bytes)
 */
#include "mqtt_client.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MQTT_PKT_BUF  2048            /* topic + payload + overhead      */
#define MQTT_INBUF_MAX 256            /* we only expect tiny ctrl pkts   */

/* ------------------------------------------------------------------ */
/* encoding helpers                                                    */
/* ------------------------------------------------------------------ */

static size_t encode_remaining_length(uint8_t *out, uint32_t len)
{
    size_t n = 0;

    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0)
            b |= 0x80;
        out[n++] = b;
    } while (len > 0 && n < 4);
    return n;
}

/* MQTT UTF-8 string: 2-byte length prefix + bytes */
static size_t put_mstr(uint8_t *out, const char *s)
{
    size_t n = strlen(s);

    out[0] = (uint8_t)(n >> 8);
    out[1] = (uint8_t)(n & 0xFF);
    memcpy(out + 2, s, n);
    return n + 2;
}

/* ------------------------------------------------------------------ */
/* packet I/O                                                          */
/* ------------------------------------------------------------------ */

static int mqtt_send_packet(mqtt_client_t *c, uint8_t hdr,
                            const uint8_t *body, size_t blen,
                            uint32_t timeout_ms)
{
    uint8_t head[8];
    size_t  hn = 0;

    head[hn++] = hdr;
    hn += encode_remaining_length(head + hn, (uint32_t)blen);

    if (util_send_all(c->fd, head, hn, timeout_ms) != 0)
        return -1;
    if (blen > 0 && util_send_all(c->fd, body, blen, timeout_ms) != 0)
        return -1;

    c->last_tx_ms = util_mono_ms();
    return 0;
}

/*
 * Read one control packet. Bodies larger than cap are a protocol error
 * for this client since we never subscribe or send large requests.
 */
static int mqtt_recv_packet(mqtt_client_t *c, uint8_t *hdr_out,
                            uint8_t *body, size_t cap, size_t *blen_out,
                            uint32_t timeout_ms)
{
    uint64_t deadline = util_mono_ms() + timeout_ms;
    uint8_t  b;
    uint32_t rl   = 0;
    uint32_t mult = 1;
    int      rc;

    rc = util_recv_exact(c->fd, &b, 1, timeout_ms);
    if (rc != 0)
        return rc;
    *hdr_out = b;

    for (int i = 0; i < 4; i++) {
        uint64_t now = util_mono_ms();
        if (now >= deadline)
            return -2;
        rc = util_recv_exact(c->fd, &b, 1, (uint32_t)(deadline - now));
        if (rc != 0)
            return rc;
        rl += (uint32_t)(b & 0x7F) * mult;
        if ((b & 0x80) == 0)
            break;
        mult *= 128;
        if (i == 3)
            return -1;              /* malformed remaining length */
    }

    if (rl > cap)
        return -1;
    if (rl > 0) {
        uint64_t now = util_mono_ms();
        if (now >= deadline)
            return -2;
        rc = util_recv_exact(c->fd, body, rl, (uint32_t)(deadline - now));
        if (rc != 0)
            return rc;
    }

    *blen_out   = rl;
    c->last_rx_ms = util_mono_ms();
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void mqtt_init(mqtt_client_t *c, const mqtt_cfg_t *cfg)
{
    memset(c, 0, sizeof *c);
    c->cfg      = *cfg;
    c->fd       = -1;
    c->next_pid = 1;
}

static void mqtt_close_fd(mqtt_client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->connected = false;
}

int mqtt_connect(mqtt_client_t *c, uint32_t timeout_ms)
{
    uint8_t body[512];
    size_t  bn = 0;
    uint8_t flags;
    bool    has_user, has_pass;
    uint8_t rhdr;
    uint8_t resp[MQTT_INBUF_MAX];
    size_t  rlen = 0;

    mqtt_close_fd(c);

    c->fd = util_tcp_connect(c->cfg.host, c->cfg.port, timeout_ms);
    if (c->fd < 0) {
        c->conn_fails++;
        return -1;
    }

    /* ---- variable header ---- */
    body[bn++] = 0;
    body[bn++] = 4;
    memcpy(body + bn, "MQTT", 4);
    bn += 4;
    body[bn++] = 4;                                 /* protocol level  */

    has_user = c->cfg.username[0] != '\0';
    has_pass = c->cfg.password[0] != '\0';
    flags = 0x02;                                   /* clean session   */
    if (has_user)
        flags |= 0x80;
    if (has_pass)
        flags |= 0x40;
    body[bn++] = flags;
    util_put_be16(body + bn, c->cfg.keepalive_s);
    bn += 2;

    /* ---- payload ---- */
    bn += put_mstr(body + bn, c->cfg.client_id);
    if (has_user)
        bn += put_mstr(body + bn, c->cfg.username);
    if (has_pass)
        bn += put_mstr(body + bn, c->cfg.password);

    if (mqtt_send_packet(c, 0x10, body, bn, timeout_ms) != 0)
        goto fail;

    if (mqtt_recv_packet(c, &rhdr, resp, sizeof resp, &rlen, timeout_ms) != 0)
        goto fail;
    if ((rhdr >> 4) != 2 || rlen != 2 || resp[1] != 0) {
        LOG_WARN("mqtt: broker refused connection (rc=%u)",
                 (rlen == 2) ? resp[1] : 0xFF);
        goto fail;
    }

    c->connected = true;
    c->connects++;
    return 0;

fail:
    c->conn_fails++;
    mqtt_close_fd(c);
    return -1;
}

void mqtt_disconnect(mqtt_client_t *c)
{
    if (c->fd >= 0 && c->connected)
        (void)mqtt_send_packet(c, 0xE0, NULL, 0, 1000);
    mqtt_close_fd(c);
}

/* ------------------------------------------------------------------ */
/* publish / keepalive                                                 */
/* ------------------------------------------------------------------ */

int mqtt_publish(mqtt_client_t *c, const char *topic,
                 const uint8_t *payload, size_t len,
                 uint8_t qos, bool retain, uint32_t timeout_ms)
{
    uint8_t  buf[MQTT_PKT_BUF];
    size_t   bn  = 0;
    uint16_t pid = 0;
    uint8_t  hdr;
    uint64_t deadline;

    if (!c->connected)
        return -1;

    bn += put_mstr(buf, topic);
    if (qos > 0) {
        pid = c->next_pid++;
        if (c->next_pid == 0)
            c->next_pid = 1;
        util_put_be16(buf + bn, pid);
        bn += 2;
    }
    if (bn + len > sizeof buf)
        return -1;                      /* guarded upstream by RB limits */
    memcpy(buf + bn, payload, len);
    bn += len;

    hdr = (uint8_t)(0x30 | (qos << 1) | (retain ? 0x01 : 0x00));
    if (mqtt_send_packet(c, hdr, buf, bn, timeout_ms) != 0) {
        c->pubs_fail++;
        return -1;
    }

    if (qos == 0) {
        c->pubs_ok++;
        return 0;
    }

    /* QoS 1: wait for the matching PUBACK, tolerate interleaved packets */
    deadline = util_mono_ms() + timeout_ms;
    while (util_mono_ms() < deadline) {
        uint8_t  rhdr;
        uint8_t  rbody[MQTT_INBUF_MAX];
        size_t   rlen = 0;
        uint32_t rem = (uint32_t)(deadline - util_mono_ms());
        int      rc;

        rc = mqtt_recv_packet(c, &rhdr, rbody, sizeof rbody, &rlen, rem);
        if (rc != 0) {
            c->pubs_fail++;
            return -1;
        }
        if ((rhdr >> 4) == 4 && rlen == 2 &&
            util_be16(rbody) == pid) {
            c->pubs_ok++;
            return 0;
        }
        /* PINGRESP / stray packets: not ours, keep waiting */
    }

    c->pubs_fail++;
    return -2;
}

int mqtt_ping(mqtt_client_t *c, uint32_t timeout_ms)
{
    uint64_t deadline;

    if (!c->connected)
        return -1;
    if (mqtt_send_packet(c, 0xC0, NULL, 0, timeout_ms) != 0)
        return -1;

    deadline = util_mono_ms() + timeout_ms;
    while (util_mono_ms() < deadline) {
        uint8_t  rhdr;
        uint8_t  rbody[MQTT_INBUF_MAX];
        size_t   rlen = 0;
        uint32_t rem = (uint32_t)(deadline - util_mono_ms());

        if (mqtt_recv_packet(c, &rhdr, rbody, sizeof rbody, &rlen, rem) != 0)
            return -1;
        if ((rhdr >> 4) == 13)          /* PINGRESP */
            return 0;
    }
    return -2;
}

bool mqtt_keepalive_due(const mqtt_client_t *c, uint64_t now_ms)
{
    uint64_t interval;

    if (!c->connected || c->cfg.keepalive_s == 0)
        return false;
    /* ping at 75% of the negotiated interval, per common practice */
    interval = (uint64_t)c->cfg.keepalive_s * 750u;
    return (now_ms - c->last_tx_ms) >= interval;
}
