/*
 * rdn_link_loop.c - in-process loopback link pair with fault injection.
 *
 * Used by the host test-suite and for simulation: supports silent cut
 * (cable pull), random frame loss and random single-bit corruption.
 */
#include <string.h>
#include "rdn.h"
#include "rdn_osal.h"

typedef struct loop_q {
    rdn_mutex_t *m;
    rdn_sem_t   *s;
    uint32_t     head;
    uint32_t     count;
    uint32_t    *len;
    uint8_t     *buf;          /* depth * RDN_MAX_FRAME */
    uint32_t     rng;          /* per direction, guarded by m */
} loop_q_t;

typedef struct loop_ep {
    struct rdn_loop_pair *p;
    uint32_t dir;              /* 0: a->b, 1: b->a (tx queue index) */
} loop_ep_t;

struct rdn_loop_pair {
    loop_q_t  q[2];
    loop_ep_t ep[2];
    uint32_t  depth;
    int       cut;             /* fault settings: guarded by q[0].m and q[1].m */
    uint32_t  drop_pm;
    uint32_t  corrupt_pm;
};

static uint32_t rng_next(loop_q_t *q)
{
    q->rng = q->rng * 1103515245u + 12345u;
    return (q->rng >> 8) % 1000u;
}

static int loop_send(void *ctx, const uint8_t *buf, uint32_t len)
{
    loop_ep_t *e = (loop_ep_t *)ctx;
    struct rdn_loop_pair *p = e->p;
    loop_q_t *q = &p->q[e->dir];
    uint32_t slot;

    if ((len == 0u) || (len > RDN_MAX_FRAME)) {
        return RDN_E_PARAM;
    }
    rdn_mutex_lock(q->m);
    if (p->cut || ((p->drop_pm != 0u) && (rng_next(q) < p->drop_pm))) {
        rdn_mutex_unlock(q->m);
        return RDN_OK;                      /* lost silently */
    }
    if (q->count >= p->depth) {
        rdn_mutex_unlock(q->m);
        return RDN_E_FULL;
    }
    slot = (q->head + q->count) % p->depth;
    (void)memcpy(&q->buf[slot * RDN_MAX_FRAME], buf, len);
    q->len[slot] = len;
    if ((p->corrupt_pm != 0u) && (rng_next(q) < p->corrupt_pm)) {
        uint32_t bit = (q->rng >> 3) % (len * 8u);
        q->buf[slot * RDN_MAX_FRAME + bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
    q->count++;
    rdn_mutex_unlock(q->m);
    rdn_sem_give(q->s);
    return RDN_OK;
}

static int loop_recv(void *ctx, uint8_t *buf, uint32_t cap, uint32_t timeout_us)
{
    loop_ep_t *e = (loop_ep_t *)ctx;
    loop_q_t *q = &e->p->q[e->dir ^ 1u];
    uint64_t deadline = rdn_time_us() + timeout_us;

    for (;;) {
        uint64_t now;
        rdn_mutex_lock(q->m);
        if (q->count > 0u) {
            uint32_t len = q->len[q->head];
            if (len > cap) {
                len = cap;
            }
            (void)memcpy(buf, &q->buf[q->head * RDN_MAX_FRAME], len);
            q->head = (q->head + 1u) % e->p->depth;
            q->count--;
            rdn_mutex_unlock(q->m);
            return (int)len;
        }
        rdn_mutex_unlock(q->m);
        now = rdn_time_us();
        if (now >= deadline) {
            return 0;
        }
        (void)rdn_sem_take(q->s, (uint32_t)(deadline - now));
    }
}

static void loop_close(void *ctx)
{
    (void)ctx;   /* pair is destroyed by rdn_loop_pair_destroy() */
}

static const rdn_link_ops_t loop_ops = {
    "loop", loop_send, loop_recv, loop_close
};

rdn_loop_pair_t *rdn_loop_pair_create(uint32_t depth)
{
    struct rdn_loop_pair *p;
    uint32_t i;

    if (depth == 0u) {
        return NULL;
    }
    p = (struct rdn_loop_pair *)rdn_osal_alloc(sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->depth = depth;
    for (i = 0u; i < 2u; i++) {
        p->q[i].rng = 0x1234567u + i;
        p->q[i].m   = rdn_mutex_create();
        p->q[i].s   = rdn_sem_create();
        p->q[i].len = (uint32_t *)rdn_osal_alloc(depth * sizeof(uint32_t));
        p->q[i].buf = (uint8_t *)rdn_osal_alloc((size_t)depth * RDN_MAX_FRAME);
        if ((p->q[i].m == NULL) || (p->q[i].s == NULL) ||
            (p->q[i].len == NULL) || (p->q[i].buf == NULL)) {
            rdn_loop_pair_destroy(p);
            return NULL;
        }
        p->ep[i].p   = p;
        p->ep[i].dir = i;
    }
    return p;
}

void rdn_loop_pair_links(rdn_loop_pair_t *p, rdn_link_t *a, rdn_link_t *b)
{
    a->ops = &loop_ops;
    a->ctx = &p->ep[0];
    b->ops = &loop_ops;
    b->ctx = &p->ep[1];
}

static void lock_both(rdn_loop_pair_t *p)
{
    rdn_mutex_lock(p->q[0].m);
    rdn_mutex_lock(p->q[1].m);
}

static void unlock_both(rdn_loop_pair_t *p)
{
    rdn_mutex_unlock(p->q[1].m);
    rdn_mutex_unlock(p->q[0].m);
}

void rdn_loop_set_cut(rdn_loop_pair_t *p, int cut)
{
    lock_both(p);
    p->cut = cut;
    unlock_both(p);
}

void rdn_loop_set_drop(rdn_loop_pair_t *p, uint32_t pm)
{
    lock_both(p);
    p->drop_pm = pm;
    unlock_both(p);
}

void rdn_loop_set_corrupt(rdn_loop_pair_t *p, uint32_t pm)
{
    lock_both(p);
    p->corrupt_pm = pm;
    unlock_both(p);
}

void rdn_loop_pair_destroy(rdn_loop_pair_t *p)
{
    uint32_t i;
    if (p == NULL) {
        return;
    }
    for (i = 0u; i < 2u; i++) {
        if (p->q[i].m != NULL)   { rdn_mutex_destroy(p->q[i].m); }
        if (p->q[i].s != NULL)   { rdn_sem_destroy(p->q[i].s); }
        if (p->q[i].len != NULL) { rdn_osal_free(p->q[i].len); }
        if (p->q[i].buf != NULL) { rdn_osal_free(p->q[i].buf); }
    }
    rdn_osal_free(p);
}
