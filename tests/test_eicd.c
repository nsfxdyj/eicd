/* Reliability tests for eicd's protocol, decoding, queue, and socket paths. */
#include "modbus_tcp.h"
#include "mqtt_client.h"
#include "points.h"
#include "ringbuf.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #expr);                              \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int recv_raw(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = recv(fd, p + done, len - done, 0);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_raw(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(fd, p + done, len - done, 0);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int test_crc_and_big_endian(void)
{
    static const uint8_t request[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    uint8_t encoded[2];

    CHECK(util_crc16_modbus(request, sizeof request) == 0xCDC5);
    CHECK(util_crc16_modbus((const uint8_t *)"123456789", 9) == 0x4B37);
    util_put_be16(encoded, 0xABCD);
    CHECK(encoded[0] == 0xAB && encoded[1] == 0xCD);
    CHECK(util_be16(encoded) == 0xABCD);
    return 0;
}

static int test_all_word_orders(void)
{
    static const struct {
        word_order_e order;
        uint16_t regs[2];
    } cases[] = {
        { WO_ABCD, { 0x1234, 0x5678 } },
        { WO_BADC, { 0x3412, 0x7856 } },
        { WO_CDAB, { 0x5678, 0x1234 } },
        { WO_DCBA, { 0x7856, 0x3412 } },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        point_t point;

        memset(&point, 0, sizeof point);
        point.dtype = DT_U32;
        point.worder = cases[i].order;
        point.scale = 1.0;
        CHECK(point_reg_span(&point) == 2);
        point_apply_regs(&point, cases[i].regs, 2, 1234);
        CHECK(point.quality == QUALITY_GOOD);
        CHECK(point.value == 305419896.0); /* 0x12345678 */
        CHECK(point.updated_ms == 1234);
        CHECK(point.raw[0] == cases[i].regs[0]);
        CHECK(point.raw[1] == cases[i].regs[1]);
    }
    return 0;
}

static int test_float_and_short_register_input(void)
{
    static const uint16_t one[] = { 0x3F80, 0x0000 };
    static const uint16_t nan_value[] = { 0x7FC0, 0x0000 };
    point_t point;

    memset(&point, 0, sizeof point);
    point.dtype = DT_F32;
    point.worder = WO_ABCD;
    point.scale = 2.0;
    point.offset = 0.5;
    point_apply_regs(&point, one, 2, 100);
    CHECK(point.quality == QUALITY_GOOD);
    CHECK(point.value == 2.5);

    point_apply_regs(&point, one, 1, 101);
    CHECK(point.quality == QUALITY_BAD);
    point_apply_regs(&point, nan_value, 2, 102);
    CHECK(point.quality == QUALITY_BAD);
    return 0;
}

static int test_ring_wrap_and_stale_commit(void)
{
    ringbuf_t queue;
    rb_msg_t message;
    uint64_t enq, deq, dropped;
    uint64_t first_seq;

    CHECK(rb_init(&queue, 3) == 0);
    for (uint8_t value = 0; value < 3; value++)
        CHECK(rb_push(&queue, "factory/point", &value, 1) == 0);

    CHECK(rb_peek(&queue, &message, 0) == 0);
    CHECK(message.payload[0] == 0);
    first_seq = message.seq;

    /* This push overwrites the peeked head before its publish commit. */
    CHECK(rb_push(&queue, "factory/point", &(uint8_t){ 3 }, 1) == 0);
    CHECK(rb_depth(&queue) == 3);
    rb_commit(&queue, first_seq);
    CHECK(rb_depth(&queue) == 3); /* the new head must not be retired */

    for (uint8_t expected = 1; expected < 4; expected++) {
        CHECK(rb_peek(&queue, &message, 0) == 0);
        CHECK(message.payload[0] == expected);
        rb_commit(&queue, message.seq);
    }
    CHECK(rb_depth(&queue) == 0);
    rb_stats(&queue, &enq, &deq, &dropped);
    CHECK(enq == 4 && deq == 3 && dropped == 1);
    CHECK(rb_peek(&queue, &message, 0) == 1);
    rb_close(&queue);
    CHECK(rb_peek(&queue, &message, 0) == -1);
    rb_destroy(&queue);
    return 0;
}

static int test_modbus_argument_validation(void)
{
    mb_tcp_t context;
    uint16_t output[1];

    mb_tcp_init(&context, "127.0.0.1", 502, 1, 50);
    CHECK(mb_tcp_read_regs(&context, 3, 0, 0, output) == MB_ERR_PROTO);
    CHECK(mb_tcp_read_regs(&context, 3, 0, 1, output) == MB_ERR_CLOSED);
    return 0;
}

typedef struct {
    int fd;
    int error;
} mb_server_t;

static size_t make_fc3_response(uint8_t *out, uint16_t tid, uint8_t unit,
                                uint16_t first, uint16_t second)
{
    util_put_be16(out + 0, tid);
    util_put_be16(out + 2, 0);
    util_put_be16(out + 4, 7); /* unit + six PDU bytes */
    out[6] = unit;
    out[7] = 0x03;
    out[8] = 4;
    util_put_be16(out + 9, first);
    util_put_be16(out + 11, second);
    return 13;
}

static int read_fc3_request(int fd, uint16_t tid, uint8_t unit)
{
    uint8_t request[12];

    if (recv_raw(fd, request, sizeof request) != 0)
        return -1;
    if (util_be16(request + 0) != tid || util_be16(request + 2) != 0 ||
        util_be16(request + 4) != 6 || request[6] != unit ||
        request[7] != 0x03 || util_be16(request + 8) != 0 ||
        util_be16(request + 10) != 2)
        return -1;
    return 0;
}

static void *partial_modbus_server(void *argument)
{
    mb_server_t *server = argument;
    static const uint8_t chunks[] = { 1, 2, 4, 6 };
    uint8_t response[13];
    size_t offset = 0;

    server->error = read_fc3_request(server->fd, 1, 7);
    if (server->error != 0)
        return NULL;
    (void)make_fc3_response(response, 1, 7, 0x1234, 0xABCD);
    for (size_t i = 0; i < sizeof chunks; i++) {
        if (send_raw(server->fd, response + offset, chunks[i]) != 0) {
            server->error = -1;
            return NULL;
        }
        offset += chunks[i];
        util_sleep_ms(2);
    }
    return NULL;
}

static int test_modbus_fragmented_response(void)
{
    int sockets[2];
    pthread_t thread;
    mb_server_t server = { .fd = -1, .error = 0 };
    mb_tcp_t context;
    uint16_t output[2] = { 0, 0 };
    int rc;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server.fd = sockets[1];
    CHECK(pthread_create(&thread, NULL, partial_modbus_server, &server) == 0);
    mb_tcp_init(&context, "socketpair", 0, 7, 200);
    context.fd = sockets[0];
    rc = mb_tcp_read_regs(&context, 3, 0, 2, output);
    CHECK(pthread_join(thread, NULL) == 0);
    close(server.fd);
    mb_tcp_close(&context);
    CHECK(rc == MB_OK);
    CHECK(server.error == 0);
    CHECK(output[0] == 0x1234 && output[1] == 0xABCD);
    CHECK(context.tx_ok == 1 && context.tx_fail == 0);
    return 0;
}

static void *sticky_modbus_server(void *argument)
{
    mb_server_t *server = argument;
    uint8_t responses[26];

    server->error = read_fc3_request(server->fd, 1, 7);
    if (server->error != 0)
        return NULL;
    (void)make_fc3_response(responses, 1, 7, 1, 2);
    (void)make_fc3_response(responses + 13, 2, 7, 3, 4);
    if (send_raw(server->fd, responses, sizeof responses) != 0) {
        server->error = -1;
        return NULL;
    }
    server->error = read_fc3_request(server->fd, 2, 7);
    return NULL;
}

static int test_modbus_sticky_responses(void)
{
    int sockets[2];
    pthread_t thread;
    mb_server_t server = { .fd = -1, .error = 0 };
    mb_tcp_t context;
    uint16_t output[2];
    int first_rc, second_rc;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server.fd = sockets[1];
    CHECK(pthread_create(&thread, NULL, sticky_modbus_server, &server) == 0);
    mb_tcp_init(&context, "socketpair", 0, 7, 200);
    context.fd = sockets[0];
    first_rc = mb_tcp_read_regs(&context, 3, 0, 2, output);
    CHECK(first_rc == MB_OK && output[0] == 1 && output[1] == 2);
    second_rc = mb_tcp_read_regs(&context, 3, 0, 2, output);
    CHECK(pthread_join(thread, NULL) == 0);
    close(server.fd);
    mb_tcp_close(&context);
    CHECK(second_rc == MB_OK);
    CHECK(server.error == 0);
    CHECK(output[0] == 3 && output[1] == 4);
    CHECK(context.tx_ok == 2);
    return 0;
}

static void *disconnecting_modbus_server(void *argument)
{
    mb_server_t *server = argument;

    server->error = read_fc3_request(server->fd, 1, 7);
    shutdown(server->fd, SHUT_RDWR);
    close(server->fd);
    server->fd = -1;
    return NULL;
}

static int test_modbus_disconnect(void)
{
    int sockets[2];
    pthread_t thread;
    mb_server_t server = { .fd = -1, .error = 0 };
    mb_tcp_t context;
    uint16_t output[2];
    int rc;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server.fd = sockets[1];
    CHECK(pthread_create(&thread, NULL, disconnecting_modbus_server, &server) == 0);
    mb_tcp_init(&context, "socketpair", 0, 7, 200);
    context.fd = sockets[0];
    rc = mb_tcp_read_regs(&context, 3, 0, 2, output);
    CHECK(pthread_join(thread, NULL) == 0);
    mb_tcp_close(&context);
    CHECK(rc == MB_ERR_IO);
    CHECK(server.error == 0);
    CHECK(context.tx_ok == 0 && context.tx_fail == 1);
    return 0;
}

typedef struct {
    int fd;
    int error;
    uint32_t remaining_length;
    int remaining_length_bytes;
} mqtt_server_t;

static void *acknowledging_mqtt_server(void *argument)
{
    static const uint8_t wrong_then_right_ack[] = {
        0x40, 0x02, 0x00, 0x63,
        0x40, 0x02, 0x00, 0x01,
    };
    mqtt_server_t *server = argument;
    uint8_t header, byte, body[256];
    uint32_t multiplier = 1;

    server->error = -1;
    if (recv_raw(server->fd, &header, 1) != 0 || header != 0x32)
        return NULL;
    server->remaining_length = 0;
    server->remaining_length_bytes = 0;
    for (int i = 0; i < 4; i++) {
        if (recv_raw(server->fd, &byte, 1) != 0)
            return NULL;
        server->remaining_length_bytes++;
        server->remaining_length += (uint32_t)(byte & 0x7F) * multiplier;
        multiplier *= 128;
        if ((byte & 0x80) == 0)
            break;
        if (i == 3)
            return NULL;
    }
    if (server->remaining_length != 137 ||
        recv_raw(server->fd, body, server->remaining_length) != 0)
        return NULL;
    if (util_be16(body) != 3 || memcmp(body + 2, "a/b", 3) != 0 ||
        util_be16(body + 5) != 1)
        return NULL;
    for (size_t i = 7; i < server->remaining_length; i++) {
        if (body[i] != 0x5A)
            return NULL;
    }

    /* A stray packet ID must be ignored; split the two sticky ACKs. */
    if (send_raw(server->fd, wrong_then_right_ack, 3) != 0)
        return NULL;
    util_sleep_ms(2);
    if (send_raw(server->fd, wrong_then_right_ack + 3,
                 sizeof wrong_then_right_ack - 3) != 0)
        return NULL;
    server->error = 0;
    return NULL;
}

static int test_mqtt_qos1_varint_and_puback(void)
{
    int sockets[2];
    pthread_t thread;
    mqtt_server_t server = { .fd = -1, .error = 0 };
    mqtt_client_t client;
    uint8_t payload[130];
    int rc;

    memset(payload, 0x5A, sizeof payload);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server.fd = sockets[1];
    CHECK(pthread_create(&thread, NULL, acknowledging_mqtt_server, &server) == 0);
    memset(&client, 0, sizeof client);
    client.fd = sockets[0];
    client.connected = true;
    client.next_pid = 1;
    rc = mqtt_publish(&client, "a/b", payload, sizeof payload, 1, false, 200);
    CHECK(pthread_join(thread, NULL) == 0);
    close(server.fd);
    close(client.fd);
    client.fd = -1;
    CHECK(rc == 0);
    CHECK(server.error == 0);
    CHECK(server.remaining_length == 137);
    CHECK(server.remaining_length_bytes == 2);
    CHECK(client.pubs_ok == 1 && client.pubs_fail == 0);
    CHECK(client.next_pid == 2);
    return 0;
}

typedef int (*test_function_t)(void);

typedef struct {
    const char *name;
    test_function_t function;
} test_case_t;

int main(void)
{
    static const test_case_t tests[] = {
        { "crc_and_big_endian", test_crc_and_big_endian },
        { "all_word_orders", test_all_word_orders },
        { "float_and_short_register_input", test_float_and_short_register_input },
        { "ring_wrap_and_stale_commit", test_ring_wrap_and_stale_commit },
        { "modbus_argument_validation", test_modbus_argument_validation },
        { "modbus_fragmented_response", test_modbus_fragmented_response },
        { "modbus_sticky_responses", test_modbus_sticky_responses },
        { "modbus_disconnect", test_modbus_disconnect },
        { "mqtt_qos1_varint_and_puback", test_mqtt_qos1_varint_and_puback },
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int rc = tests[i].function();
        printf("[%s] %s\n", rc == 0 ? "PASS" : "FAIL", tests[i].name);
        if (rc != 0)
            failures++;
    }
    printf("Ran %zu tests: %d passed, %d failed\n",
           sizeof tests / sizeof tests[0],
           (int)(sizeof tests / sizeof tests[0]) - failures, failures);
    return failures == 0 ? 0 : 1;
}
