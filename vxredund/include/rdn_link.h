/*
 * rdn_link.h - transport abstraction for vxredund
 *
 * A link carries opaque frames (<= RDN_MAX_FRAME bytes). Integrity,
 * sequencing and authenticity checks are done above this layer (rdn_proto),
 * so links may be unreliable: loss, duplication and corruption are
 * tolerated and detected.
 */
#ifndef RDN_LINK_H
#define RDN_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdn_link_ops {
    const char *name;
    /* returns RDN_OK or a negative status; must not block indefinitely */
    int  (*send)(void *ctx, const uint8_t *buf, uint32_t len);
    /* returns frame length (>0), 0 on timeout, or a negative status */
    int  (*recv)(void *ctx, uint8_t *buf, uint32_t cap, uint32_t timeout_us);
    void (*close)(void *ctx);
} rdn_link_ops_t;

typedef struct rdn_link {
    const rdn_link_ops_t *ops;
    void                 *ctx;
} rdn_link_t;

/* ---- UDP over a dedicated (Gigabit) Ethernet link ----------------------- */
typedef struct rdn_udp_cfg {
    const char *local_ip;     /* address of the dedicated interface       */
    const char *peer_ip;
    uint16_t    local_port;
    uint16_t    peer_port;
    int         tos;          /* IP TOS/DSCP byte, 0 = leave default       */
} rdn_udp_cfg_t;

int rdn_link_udp_open(const rdn_udp_cfg_t *cfg, rdn_link_t *out);

/* ---- VME backplane shared-memory ring -----------------------------------
 * Each node exposes one area in its own memory as a VME slave window and
 * maps the peer's area through a master window. All cross-backplane
 * accesses are writes (posted); a node only ever reads its own memory.
 * Layout and ordering rules: docs/MVME5500.md. */
typedef struct rdn_vme_cfg {
    void     *local_base;     /* my area, CPU address of my slave window   */
    void     *remote_base;    /* peer area, CPU address via master window  */
    uint32_t  area_size;      /* bytes available in each area              */
    uint32_t  slot_count;     /* power of two                              */
    uint32_t  slot_size;      /* max frame bytes per slot (<= RDN_MAX_FRAME)*/
    uint32_t  poll_us;        /* receive poll interval (no doorbell irq)   */
    /* optional cache maintenance for non-snooped windows (may be NULL) */
    void    (*cache_invalidate)(void *addr, uint32_t len);
    void    (*cache_flush)(void *addr, uint32_t len);
} rdn_vme_cfg_t;

uint32_t rdn_link_vme_area_size(uint32_t slot_count, uint32_t slot_size);
int      rdn_link_vme_open(const rdn_vme_cfg_t *cfg, rdn_link_t *out);

/* ---- in-process loopback with fault injection (tests / simulation) ------ */
typedef struct rdn_loop_pair rdn_loop_pair_t;

rdn_loop_pair_t *rdn_loop_pair_create(uint32_t depth);
void rdn_loop_pair_links(rdn_loop_pair_t *p, rdn_link_t *a, rdn_link_t *b);
void rdn_loop_set_cut(rdn_loop_pair_t *p, int cut);        /* silent drop  */
void rdn_loop_set_drop(rdn_loop_pair_t *p, uint32_t permille);
void rdn_loop_set_corrupt(rdn_loop_pair_t *p, uint32_t permille);
void rdn_loop_pair_destroy(rdn_loop_pair_t *p);

#ifdef __cplusplus
}
#endif
#endif /* RDN_LINK_H */
