#include <aex_ring.h>

aex_ring_t *
aex_ring_create(uint32_t size) {
    aex_ring_t *r = NULL;

    if (size == 0 || size > 1024 || (size & (size - 1))) {
        fprintf(stderr, "ERROR: failed to create ring: invalid argument\n");
        return NULL;
    }

    r = malloc(sizeof(r) + size * sizeof(void *));
    if (r == NULL) {
        fprintf(stderr, "ERROR: failed to create ring: not enough memory\n");
        return NULL;
    }

    r->mask = size - 1;
    r->pc = size;
    r->head = r->tail = 0;

    return r;
}

int
aex_ring_put(aex_ring_t * const r, void * const d) {
    if (r->head - r->tail == r->mask) return -ENOSPC;

    r->pv[r->head++ & r->mask] = d;

    return 0;
}

void *
aex_ring_get(aex_ring_t * const r) {
    if (r->head == r->tail) return NULL;

    return r->pv[r->tail++ & r->mask];
}

void
aex_ring_destroy(aex_ring_t * const r) {
    free(r);
}

