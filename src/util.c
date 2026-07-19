/*
 * util.c - see util.h. Small helpers, no global state.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* clocks                                                              */
/* ------------------------------------------------------------------ */

uint64_t util_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t util_epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void util_sleep_ms(uint32_t ms)
{
    struct timespec req = {
        .tv_sec  = (time_t)(ms / 1000u),
        .tv_nsec = (long)(ms % 1000u) * 1000000L,
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        /* resume after signal delivery */
    }
}

void util_timespec_add_ms(struct timespec *ts, uint32_t ms)
{
    ts->tv_sec  += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

size_t util_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t slen = strlen(src);

    if (cap > 0) {
        size_t n = (slen >= cap) ? cap - 1 : slen;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return slen;
}

char *util_trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

bool util_parse_long(const char *s, long *out)
{
    char *end = NULL;
    long  v;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s)
        return false;
    while (end != NULL && isspace((unsigned char)*end))
        end++;
    if (end != NULL && *end != '\0')
        return false;
    *out = v;
    return true;
}

bool util_parse_double(const char *s, double *out)
{
    char  *end = NULL;
    double v;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || !isfinite(v))
        return false;
    while (end != NULL && isspace((unsigned char)*end))
        end++;
    if (end != NULL && *end != '\0')
        return false;
    *out = v;
    return true;
}

bool util_parse_bool(const char *s, bool *out)
{
    if (s == NULL)
        return false;
    if (strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
        strcasecmp(s, "on") == 0   || strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
        strcasecmp(s, "off") == 0   || strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool util_is_safe_tag(const char *s)
{
    if (s == NULL || *s == '\0')
        return false;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && c != '-' && c != '_' && c != '.')
            return false;
    }
    return true;
}

bool util_is_json_safe(const char *s)
{
    if (s == NULL)
        return false;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\' || c < 0x20)
            return false;
    }
    return true;
}

const char *util_strerrno(int err, char *buf, size_t cap)
{
    /* GNU variant returns a char*, possibly static storage */
    return strerror_r(err, buf, cap);
}

/* ------------------------------------------------------------------ */
/* binary                                                              */
/* ------------------------------------------------------------------ */

uint16_t util_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void util_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

uint16_t util_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    while (len-- > 0) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x0001) ? (uint16_t)((crc >> 1) ^ 0xA001)
                                 : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* filesystem                                                          */
/* ------------------------------------------------------------------ */

int util_mkdir_p(const char *path, int mode)
{
    char tmp[512];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* networking                                                          */
/* ------------------------------------------------------------------ */

int util_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    char             portstr[8];
    int              fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        int flags, rc;

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        /* non-blocking connect bounded by poll() */
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };

            rc = poll(&pfd, 1, (int)timeout_ms);
            if (rc > 0) {
                int       soerr = 0;
                socklen_t slen  = sizeof soerr;

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 ||
                    soerr != 0)
                    rc = -1;
                else
                    rc = 0;
            } else {
                rc = -1;            /* timeout or poll error */
            }
        }

        if (rc == 0) {
            int one = 1;

            /* restore blocking mode; all later I/O uses poll deadlines */
            if (fcntl(fd, F_SETFL, flags) < 0) {
                close(fd);
                fd = -1;
                continue;
            }
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int util_send_all(int fd, const void *buf, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p        = buf;
    size_t         sent     = 0;
    uint64_t       deadline = util_mono_ms() + timeout_ms;

    while (sent < len) {
        uint64_t now = util_mono_ms();
        struct pollfd pfd;
        int rc;

        if (now >= deadline)
            return -2;
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc == 0)
            return -2;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;

        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int util_recv_exact(int fd, void *buf, size_t len, uint32_t timeout_ms)
{
    uint8_t *p        = buf;
    size_t   got      = 0;
    uint64_t deadline = util_mono_ms() + timeout_ms;

    while (got < len) {
        uint64_t now = util_mono_ms();
        struct pollfd pfd;
        int rc;

        if (now >= deadline)
            return -2;
        pfd.fd     = fd;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc == 0)
            return -2;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLNVAL))
            return -1;

        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return -3;              /* orderly shutdown by peer */
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}
