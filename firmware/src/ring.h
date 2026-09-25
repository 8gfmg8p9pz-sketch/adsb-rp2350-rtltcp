/* Single-producer / single-consumer byte ring buffer (core0 -> core1) */
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t *buf;
    uint32_t size;               /* power of two not required */
    volatile uint32_t head;      /* write index (producer) */
    volatile uint32_t tail;      /* read index  (consumer) */
    volatile uint32_t overruns;  /* bytes dropped because the ring was full */
} ring_t;

void     ring_init(ring_t *r, uint8_t *storage, uint32_t size);
void     ring_reset(ring_t *r);
uint32_t ring_used(const ring_t *r);
uint32_t ring_free(const ring_t *r);
/* returns bytes actually written (drops the remainder, counted in overruns) */
uint32_t ring_write(ring_t *r, const uint8_t *src, uint32_t len);
/* copy out up to len bytes, returns count */
uint32_t ring_read(ring_t *r, uint8_t *dst, uint32_t len);
/* zero-copy: pointer to a contiguous readable region (<= len), then ring_consume() */
uint32_t ring_peek(const ring_t *r, const uint8_t **ptr, uint32_t len);
void     ring_consume(ring_t *r, uint32_t len);

#endif
