/*
 * rdn_link_vme.c - VME backplane shared-memory ring link
 *
 * Each node owns one AREA in its local DRAM, exported on VME through an
 * A32 slave window. The peer maps that area through a master window.
 *
 *   AREA (owned by node X, in X's memory)
 *   +0   magic       written by X
 *   +4   slot_count  written by X
 *   +8   slot_size   written by X
 *   +12  rx_prod     written by PEER: frames the peer has produced for X
 *   +16  tx_cons     written by PEER: frames of X's the peer has consumed
 *   +20  cons_gen    written by PEER: changes whenever the peer (re)opens
 *   +64  slot[slot_count] = { u32 len; u32 seq; u8 data[slot_size] }
 *        seq = producer index of the frame; a mismatch with the consumer
 *        index (garbage after power-up, overrun) triggers resync.
 *
 * Every access that crosses the backplane is a WRITE (posted, cheap);
 * each CPU reads only its own memory. Ordering: data words, then barrier,
 * then the index word. The Universe II posted-write FIFO preserves order,
 * the barrier (PowerPC "sync") ensures the CPU emits them in that order.
 *
 * Restart resynchronisation (no handshake needed):
 *   consumer open: publish tx_cons, then a new cons_gen
 *   producer: cons_gen changed                -> prod = tx_cons
 *             prod - tx_cons > slot_count     -> prod = tx_cons
 *   consumer: rx_prod - cons > slot_count     -> cons = rx_prod (drop)
 *             slot seq != cons                -> cons = rx_prod (drop)
 * The generation word removes the one ambiguity index arithmetic cannot
 * resolve: a full ring behind a dead consumer versus a consumer that
 * restarted at the index that makes the ring look full.
 * Frames lost during resync are detected by the protocol layer. After a
 * consumer cold restart, up to slot_count frames sent before the restart
 * may still be delivered; the protocol layer filters them (seq/CRC).
 *
 * Both nodes must have the same endianness and the windows must not
 * byte-swap asymmetrically (see docs/MVME5500.md).
 */
#include <string.h>
#include "rdn.h"
#include "rdn_osal.h"

#define VME_MAGIC    0x52564D45u   /* "RVME" */
#define VME_HDR      64u
#define SLOT_HDR     8u

#define OFF_MAGIC    0u
#define OFF_COUNT    4u
#define OFF_SIZE     8u
#define OFF_RX_PROD  12u
#define OFF_TX_CONS  16u
#define OFF_CONS_GEN 20u

typedef struct vme_ctx {
    volatile uint8_t *local;
    volatile uint8_t *remote;
    uint32_t count;
    uint32_t size;
    uint32_t stride;
    uint32_t prod;       /* frames I produced into the peer's ring */
    uint32_t cons;       /* frames I consumed from my ring        */
    uint32_t seen_gen;   /* last consumer generation seen         */
    uint32_t poll_us;
    void   (*inval)(void *addr, uint32_t len);
    void   (*flush)(void *addr, uint32_t len);
} vme_ctx_t;

static volatile uint32_t *w32(volatile uint8_t *base, uint32_t off)
{
    return (volatile uint32_t *)(void *)(base + off);
}

/* word-wise copies: never let a library memcpy use cache-block
 * instructions (dcbz) on cache-inhibited VME space */
static void copy_to_vme(volatile uint8_t *dst, const uint8_t *src, uint32_t len)
{
    volatile uint32_t *d = (volatile uint32_t *)(void *)dst;
    uint32_t i;
    uint32_t words = (len + 3u) / 4u;

    for (i = 0u; i < words; i++) {
        uint32_t w = 0u;
        uint32_t rem = len - i * 4u;
        (void)memcpy(&w, &src[i * 4u], (rem >= 4u) ? 4u : rem);
        d[i] = w;
    }
}

static void copy_from_local(uint8_t *dst, volatile uint8_t *src, uint32_t len)
{
    volatile uint32_t *s = (volatile uint32_t *)(void *)src;
    uint32_t i;
    uint32_t words = (len + 3u) / 4u;

    for (i = 0u; i < words; i++) {
        uint32_t w = s[i];
        uint32_t rem = len - i * 4u;
        (void)memcpy(&dst[i * 4u], &w, (rem >= 4u) ? 4u : rem);
    }
}

uint32_t rdn_link_vme_area_size(uint32_t slot_count, uint32_t slot_size)
{
    uint32_t size4 = (slot_size + 3u) & ~3u;
    return VME_HDR + slot_count * (SLOT_HDR + size4);
}

static int vme_send(void *ctx, const uint8_t *buf, uint32_t len)
{
    vme_ctx_t *v = (vme_ctx_t *)ctx;
    volatile uint8_t *slot;
    uint32_t tx_cons;

    if ((len == 0u) || (len > v->size)) {
        return RDN_E_PARAM;
    }
    if (v->inval != NULL) {
        v->inval((void *)(uintptr_t)(v->local), VME_HDR);
    }
    if (*w32(v->local, OFF_CONS_GEN) != v->seen_gen) {
        v->seen_gen = *w32(v->local, OFF_CONS_GEN);
        RDN_MEM_BARRIER();                 /* gen before tx_cons */
        v->prod = *w32(v->local, OFF_TX_CONS);  /* consumer restarted */
    }
    tx_cons = *w32(v->local, OFF_TX_CONS);
    if ((v->prod - tx_cons) > v->count) {
        v->prod = tx_cons;                 /* peer restarted: resync */
    }
    if ((v->prod - tx_cons) == v->count) {
        return RDN_E_FULL;                 /* peer not consuming */
    }
    slot = v->remote + VME_HDR + (v->prod & (v->count - 1u)) * v->stride;
    copy_to_vme(slot + SLOT_HDR, buf, len);
    *w32(slot, 0u) = len;
    *w32(slot, 4u) = v->prod;
    if (v->flush != NULL) {
        v->flush((void *)(uintptr_t)slot, SLOT_HDR + len);
    }
    RDN_MEM_BARRIER();                     /* data before index */
    v->prod++;
    *w32(v->remote, OFF_RX_PROD) = v->prod;
    if (v->flush != NULL) {
        v->flush((void *)(uintptr_t)(v->remote + OFF_RX_PROD), 4u);
    }
    RDN_MEM_BARRIER();
    return RDN_OK;
}

static int vme_recv(void *ctx, uint8_t *buf, uint32_t cap, uint32_t timeout_us)
{
    vme_ctx_t *v = (vme_ctx_t *)ctx;
    uint64_t deadline = rdn_time_us() + timeout_us;

    for (;;) {
        uint32_t prod;
        uint64_t now;

        if (v->inval != NULL) {
            v->inval((void *)(uintptr_t)(v->local), VME_HDR);
        }
        prod = *w32(v->local, OFF_RX_PROD);
        if ((prod - v->cons) > v->count) {
            v->cons = prod;                /* producer restarted: resync */
            *w32(v->remote, OFF_TX_CONS) = v->cons;
            RDN_MEM_BARRIER();
        }
        if (prod != v->cons) {
            volatile uint8_t *slot;
            uint32_t len;

            RDN_MEM_BARRIER();             /* index before data */
            slot = v->local + VME_HDR + (v->cons & (v->count - 1u)) * v->stride;
            if (v->inval != NULL) {
                v->inval((void *)(uintptr_t)slot, v->stride);
            }
            len = *w32(slot, 0u);
            if (*w32(slot, 4u) != v->cons) {
                v->cons = prod;            /* stale / garbage: resync */
                *w32(v->remote, OFF_TX_CONS) = v->cons;
                RDN_MEM_BARRIER();
                continue;
            }
            if ((len > v->size) || (len > cap)) {
                len = 0u;                  /* corrupt slot: drop it */
            } else {
                copy_from_local(buf, slot + SLOT_HDR, len);
            }
            RDN_MEM_BARRIER();             /* finish reading before release */
            v->cons++;
            *w32(v->remote, OFF_TX_CONS) = v->cons;
            RDN_MEM_BARRIER();
            if (len > 0u) {
                return (int)len;
            }
            continue;
        }
        now = rdn_time_us();
        if (now >= deadline) {
            return 0;
        }
        rdn_sleep_us(((deadline - now) < v->poll_us) ?
                     (uint32_t)(deadline - now) : v->poll_us);
    }
}

static void vme_close(void *ctx)
{
    vme_ctx_t *v = (vme_ctx_t *)ctx;
    *w32(v->local, OFF_MAGIC) = 0u;
    rdn_osal_free(v);
}

static uint32_t gen_counter;

static const rdn_link_ops_t vme_ops = { "vme", vme_send, vme_recv, vme_close };

int rdn_link_vme_open(const rdn_vme_cfg_t *cfg, rdn_link_t *out)
{
    vme_ctx_t *v;
    volatile uint8_t *loc;

    if ((cfg == NULL) || (out == NULL) || (cfg->local_base == NULL) ||
        (cfg->remote_base == NULL) || (cfg->slot_count < 2u) ||
        ((cfg->slot_count & (cfg->slot_count - 1u)) != 0u) ||
        (cfg->slot_size < 64u) || (cfg->slot_size > RDN_MAX_FRAME) ||
        (cfg->area_size < rdn_link_vme_area_size(cfg->slot_count,
                                                 cfg->slot_size)) ||
        ((((uintptr_t)cfg->local_base) & 3u) != 0u) ||
        ((((uintptr_t)cfg->remote_base) & 3u) != 0u)) {
        return RDN_E_PARAM;
    }
    v = (vme_ctx_t *)rdn_osal_alloc(sizeof(*v));
    if (v == NULL) {
        return RDN_E_NOMEM;
    }
    v->local   = (volatile uint8_t *)cfg->local_base;
    v->remote  = (volatile uint8_t *)cfg->remote_base;
    v->count   = cfg->slot_count;
    v->size    = cfg->slot_size;
    v->stride  = SLOT_HDR + ((cfg->slot_size + 3u) & ~3u);
    v->poll_us = (cfg->poll_us == 0u) ? 1000u : cfg->poll_us;
    v->inval   = cfg->cache_invalidate;
    v->flush   = cfg->cache_flush;

    loc = v->local;
    if (v->inval != NULL) {
        v->inval((void *)(uintptr_t)loc, VME_HDR);
    }
    if ((*w32(loc, OFF_MAGIC) != VME_MAGIC) ||
        (*w32(loc, OFF_COUNT) != v->count) ||
        (*w32(loc, OFF_SIZE) != v->size)) {
        /* cold: memory content undefined */
        *w32(loc, OFF_MAGIC)   = 0u;
        *w32(loc, OFF_RX_PROD) = 0u;
        *w32(loc, OFF_TX_CONS) = 0u;
        *w32(loc, OFF_CONS_GEN) = 0u;
        *w32(loc, OFF_COUNT)   = v->count;
        *w32(loc, OFF_SIZE)    = v->size;
        RDN_MEM_BARRIER();
        *w32(loc, OFF_MAGIC)   = VME_MAGIC;
    }
    /* warm or cold: discard anything pending, continue producer numbering */
    v->cons     = *w32(loc, OFF_RX_PROD);
    v->seen_gen = *w32(loc, OFF_CONS_GEN);
    v->prod     = *w32(loc, OFF_TX_CONS);
    if (v->flush != NULL) {
        v->flush((void *)(uintptr_t)loc, VME_HDR);
    }
    /* announce the (re)opened consumer to the peer's producer */
    gen_counter++;
    *w32(v->remote, OFF_TX_CONS) = v->cons;
    RDN_MEM_BARRIER();                     /* tx_cons before gen */
    *w32(v->remote, OFF_CONS_GEN) = ((rdn_osal_entropy() << 8) ^ gen_counter) | 1u;
    RDN_MEM_BARRIER();
    out->ops = &vme_ops;
    out->ctx = v;
    return RDN_OK;
}
