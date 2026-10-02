/*
 * rdn_repl.c - state replication engine
 *
 * Invariant on the standby: outside an open transaction, work[] == app[]
 * for every region. A transaction only reaches app[] after the COMMIT
 * frame has verified fragment count, layout signature, txn chain and the
 * CRC of the complete resulting state - the replica is never observed in
 * a partially updated state.
 */
#include <string.h>
#include "rdn_repl.h"

void rdn_repl_init(rdn_repl_t *r, uint32_t block)
{
    (void)memset(r, 0, sizeof(*r));
    r->block      = block;
    r->layout_sig = rdn_crc32c(0u, NULL, 0u);
}

static void update_layout_sig(rdn_repl_t *r)
{
    uint8_t  b[6];
    uint32_t crc = 0u;
    uint32_t i;

    for (i = 0u; i < r->nreg; i++) {
        b[0] = (uint8_t)(r->reg[i].id >> 8);
        b[1] = (uint8_t)r->reg[i].id;
        rdn_put_u32(&b[2], r->reg[i].size);
        crc = rdn_crc32c(crc, b, (uint32_t)sizeof(b));
    }
    r->layout_sig = crc;
}

int rdn_repl_add(rdn_repl_t *r, uint16_t id, void *buf, uint32_t size,
                 uint8_t *shadow, uint8_t *work)
{
    uint32_t i;

    if ((buf == NULL) || (size == 0u) || (r->nreg >= RDN_MAX_REGIONS)) {
        return (r->nreg >= RDN_MAX_REGIONS) ? RDN_E_FULL : RDN_E_PARAM;
    }
    for (i = 0u; i < r->nreg; i++) {
        if (r->reg[i].id == id) {
            return RDN_E_PARAM;
        }
    }
    r->reg[r->nreg].id     = id;
    r->reg[r->nreg].size   = size;
    r->reg[r->nreg].app    = (uint8_t *)buf;
    r->reg[r->nreg].shadow = shadow;
    r->reg[r->nreg].work   = work;
    (void)memcpy(shadow, buf, size);
    (void)memcpy(work, buf, size);
    r->nreg++;
    update_layout_sig(r);
    return RDN_OK;
}

uint32_t rdn_repl_state_crc(const rdn_repl_t *r, int use_work)
{
    uint32_t crc = 0u;
    uint32_t i;

    for (i = 0u; i < r->nreg; i++) {
        crc = rdn_crc32c(crc, use_work ? r->reg[i].work : r->reg[i].app,
                         r->reg[i].size);
    }
    return crc;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static int block_dirty(const rdn_region_t *g, uint32_t off, uint32_t len,
                       int full)
{
    return full || (memcmp(&g->app[off], &g->shadow[off], len) != 0);
}

int rdn_repl_build(rdn_repl_t *r, int full, uint32_t max_payload,
                   rdn_repl_emit_fn emit, void *ctx, uint32_t *out_txn)
{
    rdn_msg_hdr_t h;
    uint8_t  cpl[RDN_COMMIT_LEN];
    uint32_t base  = full ? 0u : r->txn;
    uint32_t txn   = r->txn + 1u;
    uint32_t frags = 0u;
    uint32_t i;
    int      err   = RDN_OK;
    int      rc;

    if (txn == 0u) {
        txn = 1u;   /* 0 means "nothing applied" */
    }
    if ((max_payload == 0u) || (r->block > max_payload)) {
        return RDN_E_PARAM;
    }
    (void)memset(&h, 0, sizeof(h));
    h.txn   = txn;
    h.aux   = base;
    h.flags = full ? RDN_FLAG_FULL : 0u;

    for (i = 0u; i < r->nreg; i++) {
        const rdn_region_t *g = &r->reg[i];
        uint32_t off = 0u;

        while (off < g->size) {
            uint32_t blk = min_u32(r->block, g->size - off);
            uint32_t start;
            uint32_t run = 0u;

            if (!block_dirty(g, off, blk, full)) {
                off += blk;
                continue;
            }
            /* coalesce consecutive dirty blocks into one frame */
            start = off;
            for (;;) {
                run += blk;
                off += blk;
                if (off >= g->size) {
                    break;
                }
                blk = min_u32(r->block, g->size - off);
                if ((run + blk) > max_payload) {
                    break;
                }
                if (!block_dirty(g, off, blk, full)) {
                    break;
                }
            }
            if (frags >= 0xFFFFu) {
                return RDN_E_FULL;
            }
            (void)memcpy(&g->shadow[start], &g->app[start], run);
            h.type   = (uint8_t)RDN_MSG_REPL_DATA;
            h.region = g->id;
            h.offset = start;
            h.frag   = (uint16_t)frags;
            h.len    = (uint16_t)run;
            rc = emit(ctx, &h, &g->app[start]);
            if ((rc < 0) && (err == RDN_OK)) {
                err = rc;   /* keep going: shadow must track app */
            }
            frags++;
        }
    }

    rdn_put_u32(&cpl[0], rdn_repl_state_crc(r, 0));
    rdn_put_u32(&cpl[4], r->layout_sig);
    h.type   = (uint8_t)RDN_MSG_REPL_COMMIT;
    h.region = 0u;
    h.offset = 0u;
    h.frag   = (uint16_t)frags;
    h.len    = RDN_COMMIT_LEN;
    rc = emit(ctx, &h, cpl);
    if ((rc < 0) && (err == RDN_OK)) {
        err = rc;
    }
    r->txn = txn;
    if (out_txn != NULL) {
        *out_txn = txn;
    }
    return err;
}

/* ---- standby ------------------------------------------------------------ */

static void restore_work(rdn_repl_t *r)
{
    uint32_t i;
    for (i = 0u; i < r->nreg; i++) {
        (void)memcpy(r->reg[i].work, r->reg[i].app, r->reg[i].size);
    }
}

/* The current transaction stays open but broken, so its remaining frames
 * are absorbed silently: each loss is reported exactly once. */
static int lose_sync(rdn_repl_t *r)
{
    restore_work(r);
    r->synced    = 0u;
    r->rx_broken = 1u;
    return RDN_REPL_RX_LOST;
}

void rdn_repl_reset_standby(rdn_repl_t *r)
{
    restore_work(r);
    r->synced      = 0u;
    r->rx_open     = 0u;
    r->rx_broken   = 0u;
    r->applied_txn = 0u;
}

static int open_txn(rdn_repl_t *r, const rdn_msg_hdr_t *h)
{
    if (r->rx_open && !r->rx_broken) {
        /* previous txn never committed (commit frame lost) */
        restore_work(r);
        if (r->synced) {
            r->rx_txn = h->txn;
            return lose_sync(r);
        }
    }
    r->rx_open   = 1u;
    r->rx_txn    = h->txn;
    r->rx_base   = h->aux;
    r->rx_full   = ((h->flags & RDN_FLAG_FULL) != 0u) ? 1u : 0u;
    r->rx_frags  = 0u;
    r->rx_broken = 0u;
    if (!r->rx_full) {
        if (!r->synced) {
            r->rx_broken = 1u;          /* waiting for a full snapshot */
            return RDN_REPL_RX_OK;
        }
        if (r->rx_base != r->applied_txn) {
            return lose_sync(r);        /* missed a transaction */
        }
    }
    return RDN_REPL_RX_OK;
}

static rdn_region_t *find_region(rdn_repl_t *r, uint16_t id)
{
    uint32_t i;
    for (i = 0u; i < r->nreg; i++) {
        if (r->reg[i].id == id) {
            return &r->reg[i];
        }
    }
    return NULL;
}

int rdn_repl_rx_data(rdn_repl_t *r, const rdn_msg_hdr_t *h,
                     const uint8_t *payload)
{
    rdn_region_t *g;
    int rc;

    if ((!r->rx_open) || (h->txn != r->rx_txn)) {
        rc = open_txn(r, h);
        if (rc != RDN_REPL_RX_OK) {
            return rc;
        }
    }
    if (r->rx_broken) {
        return RDN_REPL_RX_OK;
    }
    if ((uint32_t)h->frag != r->rx_frags) {
        return lose_sync(r);            /* fragment lost on the link */
    }
    g = find_region(r, h->region);
    if ((g == NULL) || (h->offset > g->size) ||
        ((uint32_t)h->len > (g->size - h->offset))) {
        (void)lose_sync(r);
        return RDN_REPL_RX_LAYOUT;
    }
    (void)memcpy(&g->work[h->offset], payload, h->len);
    r->rx_frags++;
    return RDN_REPL_RX_OK;
}

int rdn_repl_rx_commit(rdn_repl_t *r, const rdn_msg_hdr_t *h,
                       const uint8_t *payload)
{
    uint32_t i;
    int rc;

    if ((!r->rx_open) || (h->txn != r->rx_txn)) {
        rc = open_txn(r, h);            /* commit with no data frames */
        if (rc != RDN_REPL_RX_OK) {
            return rc;
        }
    }
    if (r->rx_broken) {
        r->rx_open = 0u;
        return RDN_REPL_RX_OK;
    }
    if ((h->len != RDN_COMMIT_LEN) || ((uint32_t)h->frag != r->rx_frags)) {
        return lose_sync(r);
    }
    if (rdn_get_u32(&payload[4]) != r->layout_sig) {
        (void)lose_sync(r);
        return RDN_REPL_RX_LAYOUT;
    }
    if (rdn_repl_state_crc(r, 1) != rdn_get_u32(&payload[0])) {
        return lose_sync(r);
    }
    for (i = 0u; i < r->nreg; i++) {
        (void)memcpy(r->reg[i].app, r->reg[i].work, r->reg[i].size);
    }
    r->applied_txn = h->txn;
    r->rx_open     = 0u;
    if (r->rx_full) {
        r->synced = 1u;
        return RDN_REPL_RX_SYNCED;
    }
    return RDN_REPL_RX_APPLIED;
}
