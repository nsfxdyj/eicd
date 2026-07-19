/*
 * modbus_tcp.c - Modbus TCP client. See modbus_tcp.h for scope.
 *
 * Wire layout of one ADU:
 *   MBAP: tid(2) pid(2=0) length(2, unit+pdu) unit(1) | PDU: fc data...
 */
#include "modbus_tcp.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MB_MAX_ADU 260                      /* 7 MBAP + 253 PDU         */

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void mb_tcp_init(mb_tcp_t *ctx, const char *host, uint16_t port,
                 uint8_t unit, uint32_t timeout_ms)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->fd        = -1;
    ctx->port      = port;
    ctx->unit_id   = unit;
    ctx->timeout_ms = (timeout_ms > 0) ? timeout_ms : 3000;
    ctx->next_tid  = 1;
    ctx->last_exc  = -1;
    util_strlcpy(ctx->host, host, sizeof ctx->host);
}

int mb_tcp_connect(mb_tcp_t *ctx)
{
    mb_tcp_close(ctx);
    ctx->fd = util_tcp_connect(ctx->host, ctx->port, ctx->timeout_ms);
    if (ctx->fd < 0)
        return MB_ERR_IO;
    ctx->next_tid = 1;
    return MB_OK;
}

void mb_tcp_close(mb_tcp_t *ctx)
{
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

bool mb_tcp_connected(const mb_tcp_t *ctx)
{
    return ctx->fd >= 0;
}

/* ------------------------------------------------------------------ */
/* transaction core                                                    */
/* ------------------------------------------------------------------ */

/*
 * Send one PDU, wait for the matching response. On success *resp_len
 * carries the PDU length and resp[0] the (non-exception) function code.
 */
static int mb_transact(mb_tcp_t *ctx, const uint8_t *pdu, size_t pdu_len,
                       uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    uint8_t  adu[MB_MAX_ADU];
    uint8_t  hdr[7];
    uint16_t tid, len;
    int      rc;

    if (ctx->fd < 0)
        return MB_ERR_CLOSED;
    if (pdu_len == 0 || 7 + pdu_len > sizeof adu)
        return MB_ERR_PROTO;

    tid = ctx->next_tid++;
    if (ctx->next_tid == 0)
        ctx->next_tid = 1;

    util_put_be16(adu + 0, tid);
    util_put_be16(adu + 2, 0);                          /* protocol id  */
    util_put_be16(adu + 4, (uint16_t)(pdu_len + 1));    /* unit + PDU   */
    adu[6] = ctx->unit_id;
    memcpy(adu + 7, pdu, pdu_len);

    rc = util_send_all(ctx->fd, adu, 7 + pdu_len, ctx->timeout_ms);
    if (rc != 0)
        goto io_fail;

    rc = util_recv_exact(ctx->fd, hdr, sizeof hdr, ctx->timeout_ms);
    if (rc != 0)
        goto io_fail;

    if (util_be16(hdr + 0) != tid || util_be16(hdr + 2) != 0)
        return MB_ERR_PROTO;                            /* frame desync */
    if (hdr[6] != ctx->unit_id)
        return MB_ERR_PROTO;

    len = util_be16(hdr + 4);
    if (len < 2 || (size_t)(len - 1) > resp_cap)
        return MB_ERR_PROTO;

    rc = util_recv_exact(ctx->fd, resp, (size_t)(len - 1), ctx->timeout_ms);
    if (rc != 0)
        goto io_fail;

    *resp_len = (size_t)(len - 1);

    if (resp[0] & 0x80) {
        ctx->last_exc = (*resp_len > 1) ? resp[1] : -1;
        ctx->exceptions++;
        return MB_ERR_EXCEPTION;
    }
    ctx->last_exc = -1;
    return MB_OK;

io_fail:
    ctx->tx_fail++;
    return (rc == -2) ? MB_ERR_TIMEOUT : MB_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* function codes                                                      */
/* ------------------------------------------------------------------ */

int mb_tcp_read_regs(mb_tcp_t *ctx, uint8_t func, uint16_t addr,
                     uint16_t qty, uint16_t *out)
{
    uint8_t pdu[5];
    uint8_t resp[MB_MAX_ADU];
    size_t  rlen = 0;
    int     rc;

    if (qty == 0 || qty > 125)
        return MB_ERR_PROTO;

    pdu[0] = func;
    util_put_be16(pdu + 1, addr);
    util_put_be16(pdu + 3, qty);

    rc = mb_transact(ctx, pdu, sizeof pdu, resp, sizeof resp, &rlen);
    if (rc != MB_OK)
        return rc;

    if (rlen < 2 || resp[0] != func)
        return MB_ERR_PROTO;
    if (resp[1] != (uint8_t)(qty * 2) || rlen != (size_t)(2 + resp[1]))
        return MB_ERR_PROTO;

    for (uint16_t i = 0; i < qty; i++)
        out[i] = util_be16(resp + 2 + (size_t)i * 2);

    ctx->tx_ok++;
    return MB_OK;
}

int mb_tcp_read_bits(mb_tcp_t *ctx, uint8_t func, uint16_t addr,
                     uint16_t qty, uint8_t *out)
{
    uint8_t pdu[5];
    uint8_t resp[MB_MAX_ADU];
    size_t  rlen = 0;
    uint8_t expect;
    int     rc;

    if (qty == 0 || qty > EICD_MB_MAX_READ_BITS)
        return MB_ERR_PROTO;
    expect = (uint8_t)((qty + 7) / 8);

    pdu[0] = func;
    util_put_be16(pdu + 1, addr);
    util_put_be16(pdu + 3, qty);

    rc = mb_transact(ctx, pdu, sizeof pdu, resp, sizeof resp, &rlen);
    if (rc != MB_OK)
        return rc;

    if (rlen < 2 || resp[0] != func)
        return MB_ERR_PROTO;
    if (resp[1] != expect || rlen != (size_t)(2 + expect))
        return MB_ERR_PROTO;

    memcpy(out, resp + 2, expect);

    ctx->tx_ok++;
    return MB_OK;
}

int mb_tcp_write_u16(mb_tcp_t *ctx, uint16_t addr, uint16_t val)
{
    uint8_t pdu[5];
    uint8_t resp[MB_MAX_ADU];
    size_t  rlen = 0;
    int     rc;

    pdu[0] = 0x06;
    util_put_be16(pdu + 1, addr);
    util_put_be16(pdu + 3, val);

    rc = mb_transact(ctx, pdu, sizeof pdu, resp, sizeof resp, &rlen);
    if (rc != MB_OK)
        return rc;

    /* FC6 echoes the request; anything else is a framing violation */
    if (rlen != 5 || resp[0] != 0x06 || util_be16(resp + 1) != addr ||
        util_be16(resp + 3) != val)
        return MB_ERR_PROTO;

    ctx->tx_ok++;
    return MB_OK;
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

const char *mb_status_str(const mb_tcp_t *ctx, int rc)
{
    switch (rc) {
    case MB_ERR_IO:       return "i/o error";
    case MB_ERR_TIMEOUT:  return "timeout";
    case MB_ERR_PROTO:    return "protocol error";
    case MB_ERR_CLOSED:   return "not connected";
    case MB_ERR_EXCEPTION:
        switch (ctx->last_exc) {
        case 1:  return "exception 01 (illegal function)";
        case 2:  return "exception 02 (illegal data address)";
        case 3:  return "exception 03 (illegal data value)";
        case 4:  return "exception 04 (server device failure)";
        case 5:  return "exception 05 (acknowledge)";
        case 6:  return "exception 06 (server device busy)";
        case 8:  return "exception 08 (memory parity error)";
        case 10: return "exception 0A (gateway path unavailable)";
        case 11: return "exception 0B (gateway target no response)";
        default: return "modbus exception (unknown code)";
        }
    default:
        return "unknown error";
    }
}
