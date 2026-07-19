/*
 * util.h - small helpers shared by all eicd modules.
 *
 * The network helpers (util_tcp_connect / util_send_all / util_recv_exact)
 * are the single place where socket timeout semantics live: every blocking
 * operation in the daemon carries an explicit deadline.
 */
#ifndef EICD_UTIL_H
#define EICD_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- clocks -------------------------------------------------------- */
uint64_t util_mono_ms(void);                /* CLOCK_MONOTONIC, for deadlines */
uint64_t util_epoch_ms(void);               /* CLOCK_REALTIME, for timestamps */
void     util_sleep_ms(uint32_t ms);
void     util_timespec_add_ms(struct timespec *ts, uint32_t ms);

/* ---- strings ------------------------------------------------------- */
size_t   util_strlcpy(char *dst, const char *src, size_t cap);
char    *util_trim(char *s);                /* in-place, returns start      */
bool     util_parse_long(const char *s, long *out);
bool     util_parse_double(const char *s, double *out);
bool     util_parse_bool(const char *s, bool *out);
bool     util_is_safe_tag(const char *s);   /* [A-Za-z0-9._-]+              */
bool     util_is_json_safe(const char *s);  /* no quotes, backslash, ctrl   */
const char *util_strerrno(int err, char *buf, size_t cap);

/* ---- binary -------------------------------------------------------- */
uint16_t util_be16(const uint8_t *p);
void     util_put_be16(uint8_t *p, uint16_t v);
uint16_t util_crc16_modbus(const uint8_t *data, size_t len); /* RTU framing */

/* ---- filesystem ---------------------------------------------------- */
int      util_mkdir_p(const char *path, int mode);

/* ---- networking ---------------------------------------------------- */
/*
 * util_tcp_connect: connect with an overall timeout (DNS resolution
 * excepted). Returns a connected, blocking, TCP_NODELAY fd or -1.
 */
int util_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/*
 * util_send_all / util_recv_exact: full-buffer I/O bounded by a deadline.
 * Return: 0 ok, -1 i/o error, -2 timeout, -3 peer closed (recv only).
 */
int util_send_all(int fd, const void *buf, size_t len, uint32_t timeout_ms);
int util_recv_exact(int fd, void *buf, size_t len, uint32_t timeout_ms);

#endif /* EICD_UTIL_H */
