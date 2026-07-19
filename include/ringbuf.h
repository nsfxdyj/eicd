/*
 * ringbuf.h - fixed-capacity, thread-safe store-and-forward queue.
 *
 * Decouples the southbound producers (device workers) from the single
 * northbound consumer (MQTT publisher). When the broker is unreachable
 * telemetry accumulates here; once the connection is restored the queue
 * drains in FIFO order.
 *
 * Push never blocks: when full, the oldest message is overwritten and
 * the drop counter incremented. The consumer uses peek + commit(seq):
 * the head is removed only after successful delivery, and commit()
 * matches the sequence number so a head dropped by a concurrent
 * full-queue push is never retired by mistake.
 */
#ifndef EICD_RINGBUF_H
#define EICD_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "defs.h"

#define RB_TOPIC_MAX    EICD_TOPIC_MAX
#define RB_PAYLOAD_MAX  1200        /* fits one JSON point frame       */

typedef struct {
    uint64_t seq;                   /* monotonically increasing         */
    uint64_t ts_ms;                 /* enqueue time (epoch)             */
    char     topic[RB_TOPIC_MAX];
    uint16_t len;
    uint8_t  payload[RB_PAYLOAD_MAX];
} rb_msg_t;

typedef struct {
    rb_msg_t       *slots;          /* heap array, fixed at init        */
    size_t          capacity;
    size_t          head;           /* oldest message                   */
    size_t          count;
    pthread_mutex_t mtx;
    pthread_cond_t  cv_not_empty;
    uint64_t        seq_next;
    uint64_t        enq;            /* total pushes                     */
    uint64_t        deq;            /* total committed                  */
    uint64_t        dropped;        /* total overwritten (queue full)   */
    bool            closed;         /* set on shutdown, wakes consumers */
} ringbuf_t;

int      rb_init(ringbuf_t *q, size_t capacity);
void     rb_destroy(ringbuf_t *q);
int      rb_push(ringbuf_t *q, const char *topic, const void *payload,
                 size_t len);
/*
 * rb_peek: copy the head message without removing it. Blocks up to
 * timeout_ms while empty. Returns 0 with a message, 1 on timeout,
 * -1 when the queue is closed AND drained.
 */
int      rb_peek(ringbuf_t *q, rb_msg_t *out, uint32_t timeout_ms);
void     rb_commit(ringbuf_t *q, uint64_t seq); /* retire head if same seq */
void     rb_close(ringbuf_t *q);
size_t   rb_depth(ringbuf_t *q);
void     rb_stats(ringbuf_t *q, uint64_t *enq, uint64_t *deq, uint64_t *dropped);

#endif /* EICD_RINGBUF_H */
