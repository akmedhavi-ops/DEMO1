/*
 * rdn_repl.h - state replication engine (internal, no OS dependencies)
 *
 * Active side:  rdn_repl_build() compares each registered region against a
 *               shadow copy block-by-block and emits the changed blocks as
 *               REPL_DATA frames followed by one REPL_COMMIT carrying the
 *               fragment count, a CRC-32C of the complete post-commit state
 *               and the region layout signature.
 * Standby side: frames are applied to a work copy; the application-visible
 *               replica is only updated when a COMMIT verifies (count, CRC,
 *               layout, txn chain). Anything else is reported as SYNC_LOST
 *               and a full snapshot is requested.
 *
 * WCET of build/commit is linear in total registered state size.
 */
#ifndef RDN_REPL_H
#define RDN_REPL_H

#include <stdint.h>
#include "rdn_proto.h"

typedef struct rdn_region {
    uint16_t id;
    uint32_t size;
    uint8_t *app;      /* application buffer (live on active, replica on standby) */
    uint8_t *shadow;   /* active: last transmitted image                          */
    uint8_t *work;     /* standby: transaction staging image                      */
} rdn_region_t;

typedef struct rdn_repl {
    rdn_region_t reg[RDN_MAX_REGIONS];
    uint32_t     nreg;
    uint32_t     block;
    uint32_t     layout_sig;
    /* active */
    uint32_t     txn;          /* last committed transaction id */
    /* standby */
    uint32_t     applied_txn;
    uint8_t      synced;       /* replica consistent with active */
    uint8_t      rx_open;
    uint8_t      rx_full;
    uint8_t      rx_broken;    /* current txn unusable, wait for next full */
    uint32_t     rx_txn;
    uint32_t     rx_base;
    uint32_t     rx_frags;
} rdn_repl_t;

typedef int (*rdn_repl_emit_fn)(void *ctx, const rdn_msg_hdr_t *h,
                                const uint8_t *payload);

void     rdn_repl_init(rdn_repl_t *r, uint32_t block);
int      rdn_repl_add(rdn_repl_t *r, uint16_t id, void *buf, uint32_t size,
                      uint8_t *shadow, uint8_t *work);
uint32_t rdn_repl_state_crc(const rdn_repl_t *r, int use_work);

/* active: emit one transaction; returns txn id (>0) or negative status */
int      rdn_repl_build(rdn_repl_t *r, int full, uint32_t max_payload,
                        rdn_repl_emit_fn emit, void *ctx, uint32_t *out_txn);

/* standby results */
#define RDN_REPL_RX_OK        0   /* data accepted / nothing to do        */
#define RDN_REPL_RX_APPLIED   1   /* delta transaction applied            */
#define RDN_REPL_RX_SYNCED    2   /* full snapshot applied: now synced    */
#define RDN_REPL_RX_LOST      3   /* gap or verify failure: resync needed */
#define RDN_REPL_RX_LAYOUT    4   /* region layout mismatch (config error)*/

int      rdn_repl_rx_data(rdn_repl_t *r, const rdn_msg_hdr_t *h,
                          const uint8_t *payload);
/* Caller must hold the region lock: this is where the replica changes. */
int      rdn_repl_rx_commit(rdn_repl_t *r, const rdn_msg_hdr_t *h,
                            const uint8_t *payload);
void     rdn_repl_reset_standby(rdn_repl_t *r);

#endif /* RDN_REPL_H */
