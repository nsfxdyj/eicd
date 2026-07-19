/*
 * ringbuf.c - see ringbuf.h for the design contract.
 */
#include "ringbuf.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int rb_init(ringbuf_t *q, size_t capacity)
{
    memset(q, 0, sizeof *q);

    q->slots = calloc(capacity, sizeof(rb_msg_t));
    if (q->slots == NULL)
        return -1;

    q->capacity = capacity;
    q->head     = 0;
    q->count    = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv_not_empty, NULL);
    return 0;
}

void rb_destroy(ringbuf_t *q)
{
    if (q->slots != NULL) {
        pthread_mutex_destroy(&q->mtx);
        pthread_cond_destroy(&q->cv_not_empty);
        free(q->slots);
        q->slots = NULL;
    }
}

int rb_push(ringbuf_t *q, const char *topic, const void *payload, size_t len)
{
    if (len == 0 || len > RB_PAYLOAD_MAX)
        return -1;

    pthread_mutex_lock(&q->mtx);

    if (q->count == q->capacity) {
        /* full: overwrite the oldest slot */
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->dropped++;
    }

    rb_msg_t *m = &q->slots[(q->head + q->count) % q->capacity];
    m->seq   = q->seq_next++;
    m->ts_ms = util_epoch_ms();
    util_strlcpy(m->topic, topic, sizeof m->topic);
    memcpy(m->payload, payload, len);
    m->len = (uint16_t)len;

    q->count++;
    q->enq++;

    pthread_cond_signal(&q->cv_not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int rb_peek(ringbuf_t *q, rb_msg_t *out, uint32_t timeout_ms)
{
    int rc = 1;                         /* timeout by default */

    pthread_mutex_lock(&q->mtx);

    if (q->count == 0 && !q->closed) {
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        util_timespec_add_ms(&ts, timeout_ms);
        while (q->count == 0 && !q->closed) {
            if (pthread_cond_timedwait(&q->cv_not_empty, &q->mtx, &ts) ==
                ETIMEDOUT)
                break;
        }
    }

    if (q->count > 0) {
        *out = q->slots[q->head];       /* struct copy, ~1.4 KB          */
        rc = 0;
    } else if (q->closed) {
        rc = -1;                        /* closed and drained            */
    }

    pthread_mutex_unlock(&q->mtx);
    return rc;
}

void rb_commit(ringbuf_t *q, uint64_t seq)
{
    pthread_mutex_lock(&q->mtx);

    /*
     * Only retire the head if it is still the message the consumer
     * delivered. When a full queue dropped that head between peek and
     * commit, it is already gone and there is nothing to do - retiring
     * the new head instead would silently lose an undelivered message.
     */
    if (q->count > 0 && q->slots[q->head].seq == seq) {
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->deq++;
    }

    pthread_mutex_unlock(&q->mtx);
}

void rb_close(ringbuf_t *q)
{
    pthread_mutex_lock(&q->mtx);
    q->closed = true;
    pthread_cond_broadcast(&q->cv_not_empty);
    pthread_mutex_unlock(&q->mtx);
}

size_t rb_depth(ringbuf_t *q)
{
    size_t n;

    pthread_mutex_lock(&q->mtx);
    n = q->count;
    pthread_mutex_unlock(&q->mtx);
    return n;
}

void rb_stats(ringbuf_t *q, uint64_t *enq, uint64_t *deq, uint64_t *dropped)
{
    pthread_mutex_lock(&q->mtx);
    *enq     = q->enq;
    *deq     = q->deq;
    *dropped = q->dropped;
    pthread_mutex_unlock(&q->mtx);
}
