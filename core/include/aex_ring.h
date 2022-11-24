/* 
 * File:   aex_ring.h
 * Author: niakrisn
 *
 * Created on November 23, 2022, 4:07 PM
 */

#ifndef AEX_RING_H
#define AEX_RING_H

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

typedef struct {
    volatile uint32_t       head __attribute__((aligned(64)));
    volatile uint32_t       tail __attribute__((aligned(64)));

    uint32_t                mask;
    uint32_t                pc;
    void                   *pv[];
} aex_ring_t;

aex_ring_t *aex_ring_create(uint32_t size);
int aex_ring_put(aex_ring_t * const r, void * const d);
void *aex_ring_get(aex_ring_t * const r);
void aex_ring_destroy(aex_ring_t * const r);

#endif /* AEX_RING_H */

