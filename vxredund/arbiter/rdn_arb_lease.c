/*
 * rdn_arb_lease.c - reference lease arbiter (software model)
 *
 * Semantics a hardware arbiter must provide (docs/SAFETY.md):
 *   claim   : granted iff the token is free, already ours, or the current
 *             owner's lease has expired; the grant is atomic
 *   renew   : extends our lease; fails if we are no longer the owner
 *   release : frees the token if we own it
 */
#include "rdn.h"
#include "rdn_osal.h"

typedef struct lease {
    rdn_mutex_t *m;
    uint8_t      owner;
    uint64_t     expires_us;
    uint32_t     lease_us;
} lease_t;

static int lease_claim(void *ctx, uint8_t node, uint32_t epoch)
{
    lease_t *l = (lease_t *)ctx;
    uint64_t now = rdn_time_us();
    int granted = 0;

    (void)epoch;
    rdn_mutex_lock(l->m);
    if ((l->owner == 0u) || (l->owner == node) || (now > l->expires_us)) {
        l->owner      = node;
        l->expires_us = now + l->lease_us;
        granted = 1;
    }
    rdn_mutex_unlock(l->m);
    return granted;
}

static int lease_renew(void *ctx, uint8_t node)
{
    lease_t *l = (lease_t *)ctx;
    uint64_t now = rdn_time_us();
    int held = 0;

    rdn_mutex_lock(l->m);
    if ((l->owner == node) && (now <= l->expires_us)) {
        l->expires_us = now + l->lease_us;
        held = 1;
    }
    rdn_mutex_unlock(l->m);
    return held;
}

static void lease_release(void *ctx, uint8_t node)
{
    lease_t *l = (lease_t *)ctx;

    rdn_mutex_lock(l->m);
    if (l->owner == node) {
        l->owner = 0u;
    }
    rdn_mutex_unlock(l->m);
}

int rdn_lease_arbiter_create(uint32_t lease_us, rdn_arbiter_t *out)
{
    lease_t *l;

    if ((out == NULL) || (lease_us == 0u)) {
        return RDN_E_PARAM;
    }
    l = (lease_t *)rdn_osal_alloc(sizeof(*l));
    if (l == NULL) {
        return RDN_E_NOMEM;
    }
    l->m = rdn_mutex_create();
    if (l->m == NULL) {
        rdn_osal_free(l);
        return RDN_E_NOMEM;
    }
    l->lease_us  = lease_us;
    out->claim   = lease_claim;
    out->renew   = lease_renew;
    out->release = lease_release;
    out->ctx     = l;
    return RDN_OK;
}

void rdn_lease_arbiter_destroy(rdn_arbiter_t *arb)
{
    lease_t *l = (lease_t *)arb->ctx;
    rdn_mutex_destroy(l->m);
    rdn_osal_free(l);
    arb->ctx = NULL;
}

uint8_t rdn_lease_arbiter_owner(const rdn_arbiter_t *arb)
{
    lease_t *l = (lease_t *)arb->ctx;
    uint8_t o;
    rdn_mutex_lock(l->m);
    o = l->owner;
    rdn_mutex_unlock(l->m);
    return o;
}
