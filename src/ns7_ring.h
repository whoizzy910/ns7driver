/*
 * ns7_ring.h - single-producer / single-consumer byte ring, lock-free.
 * Plain data and offsets only, so a ring can later live in shared memory
 * between the driver daemon and the CoreAudio plug-in.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS7_RING_H
#define NS7_RING_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t size;               /* bytes, power of two */
    uint32_t _pad;
    uint64_t head;               /* written by producer */
    uint64_t tail;               /* written by consumer */
    uint8_t  data[];             /* size bytes follow */
} ns7_ring;

static inline uint64_t ns7_ring_used(const ns7_ring *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline uint64_t ns7_ring_free(const ns7_ring *r)
{
    return r->size - ns7_ring_used(r);
}

/* Write up to len bytes; returns bytes written. */
static inline uint32_t ns7_ring_write(ns7_ring *r, const void *src, uint32_t len)
{
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t space = r->size - (uint32_t)(head - tail);
    if (len > space) len = space;
    uint32_t pos = (uint32_t)head & (r->size - 1);
    uint32_t first = r->size - pos < len ? r->size - pos : len;
    memcpy(r->data + pos, src, first);
    memcpy(r->data, (const uint8_t *)src + first, len - first);
    __atomic_store_n(&r->head, head + len, __ATOMIC_RELEASE);
    return len;
}

/* Read up to len bytes; returns bytes read. */
static inline uint32_t ns7_ring_read(ns7_ring *r, void *dst, uint32_t len)
{
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t avail = (uint32_t)(head - tail);
    if (len > avail) len = avail;
    uint32_t pos = (uint32_t)tail & (r->size - 1);
    uint32_t first = r->size - pos < len ? r->size - pos : len;
    memcpy(dst, r->data + pos, first);
    memcpy((uint8_t *)dst + first, r->data, len - first);
    __atomic_store_n(&r->tail, tail + len, __ATOMIC_RELEASE);
    return len;
}

static inline ns7_ring *ns7_ring_new(uint32_t size_pow2)
{
    ns7_ring *r = (ns7_ring *)calloc(1, sizeof(ns7_ring) + size_pow2);
    if (r) r->size = size_pow2;
    return r;
}

#endif
