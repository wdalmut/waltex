#include "ring.h"
#include "types.h"

void ring_init(struct ring *r)
{
    r->head = 0;
    r->tail = 0;
}

int ring_push(struct ring *r, uint8_t v)
{
    uint32_t next_step = (r->head + 1) % RING_SIZE;
    if (next_step == r->tail) {
        return 0;
    }
    
    r->buf[r->head] = v;
    r->head = next_step;

    return 1;
}

int ring_pop(struct ring *r)
{
    int v = -1;

    uint32_t next_step = (r->tail + 1) % RING_SIZE;

    if (r->head != r->tail) {
        v = r->buf[r->tail];
        r->tail = next_step;
    }

    return v;
}

int ring_empty(const struct ring *r)
{
    int empty = 1;

    if (r->head != r->tail) {
        empty = 0;
    } 

    return empty;
}