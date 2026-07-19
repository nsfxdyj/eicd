/*
 * modbus_tcp.h - Modbus TCP client (master).
 *
 * Implements the subset an edge gateway needs for polling:
 *   FC 0x01 read coils, 0x02 read discrete inputs,
 *   FC 0x03 read holding registers, 0x04 read input registers,
 *   FC 0x06 write single register.
 *
 * One mb_tcp_t context is used by exactly one device worker thread, so
 * no locking is required inside the client. All operations are bounded
 * by the configured timeout via util_send_all/util_recv_exact.
 */
#ifndef EICD_MODBUS_TCP_H
#define EICD_MODBUS_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "defs.h"

typedef enum {
    MB_OK            = 0,
    MB_ERR_IO        = -1,
    MB_ERR_TIMEOUT   = -2,          /* deadline exceeded                */
    MB_ERR_PROTO     = -3,          /* framing / validation failure     */
    MB_ERR_CLOSED    = -4,          /* called while disconnected        */
    MB_ERR_EXCEPTION = -5           /* slave exception, see last_exc    */
} mb_status_e;

typedef struct {
    int      fd;
    char     host[EICD_HOST_MAX];
    uint16_t port;
    uint8_t  unit_id;
    uint32_t timeout_ms;
    uint16_t next_tid;              /* MBAP transaction id              */
    int      last_exc;              /* exception code of last failure   */
    /* statistics */
    uint64_t tx_ok;
    uint64_t tx_fail;
    uint64_t exceptions;
} mb_tcp_t;

void        mb_tcp_init(mb_tcp_t *ctx, const char *host, uint16_t port,
                        uint8_t unit, uint32_t timeout_ms);
int         mb_tcp_connect(mb_tcp_t *ctx);      /* MB_OK or MB_ERR_IO   */
void        mb_tcp_close(mb_tcp_t *ctx);
bool        mb_tcp_connected(const mb_tcp_t *ctx);

int         mb_tcp_read_regs(mb_tcp_t *ctx, uint8_t func, uint16_t addr,
                             uint16_t qty, uint16_t *out);
int         mb_tcp_read_bits(mb_tcp_t *ctx, uint8_t func, uint16_t addr,
                             uint16_t qty, uint8_t *out);
int         mb_tcp_write_u16(mb_tcp_t *ctx, uint16_t addr, uint16_t val);

const char *mb_status_str(const mb_tcp_t *ctx, int rc);

#endif /* EICD_MODBUS_TCP_H */
