#include "ring.h"
#include <string.h>

void ring_init(ring_t *r, uint8_t *storage, uint32_t size)
{
    r->buf = storage;
    r->size = size;
    r->head = r->tail = 0;
    r->overruns = 0;
}

void ring_reset(ring_t *r)
{
    r->head = r->tail = 0;
}

uint32_t ring_used(const ring_t *r)
{
    uint32_t h = r->head, t = r->tail;
    return (h >= t) ? (h - t) : (r->size - t + h);
}

uint32_t ring_free(const ring_t *r)
{
    /* keep one byte free so that head==tail means empty */
    return r->size - 1 - ring_used(r);
}

uint32_t ring_write(ring_t *r, const uint8_t *src, uint32_t len)
{
    uint32_t space = ring_free(r);
    if (len > space) {
        r->overruns += len - space;
        len = space;
    }
    uint32_t h = r->head;
    uint32_t first = r->size - h;
    if (first > len) first = len;
    memcpy(r->buf + h, src, first);
    if (len > first) memcpy(r->buf, src + first, len - first);
    h += len;
    if (h >= r->size) h -= r->size;
    __asm volatile("dmb" ::: "memory");
    r->head = h;
    return len;
}

uint32_t ring_read(ring_t *r, uint8_t *dst, uint32_t len)
{
    uint32_t used = ring_used(r);
    if (len > used) len = used;
    uint32_t t = r->tail;
    uint32_t first = r->size - t;
    if (first > len) first = len;
    memcpy(dst, r->buf + t, first);
    if (len > first) memcpy(dst + first, r->buf, len - first);
    t += len;
    if (t >= r->size) t -= r->size;
    __asm volatile("dmb" ::: "memory");
    r->tail = t;
    return len;
}

uint32_t ring_peek(const ring_t *r, const uint8_t **ptr, uint32_t len)
{
    uint32_t used = ring_used(r);
    if (len > used) len = used;
    uint32_t t = r->tail;
    uint32_t first = r->size - t;
    if (len > first) len = first;
    *ptr = r->buf + t;
    return len;
}

void ring_consume(ring_t *r, uint32_t len)
{
    uint32_t t = r->tail + len;
    if (t >= r->size) t -= r->size;
    __asm volatile("dmb" ::: "memory");
    r->tail = t;
}
