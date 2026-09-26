/*
 * rdn.h - vxredund: active/standby redundancy middleware (public API)
 *
 * Reference implementation of a documented, testable 1oo2 hot-standby
 * pattern for VxWorks 6.9 / 7 on VME CPU boards (e.g. MVME5500), with a
 * POSIX port used for host-side verification.
 *
 * Design rules followed throughout (see docs/SAFETY.md):
 *   - all memory is allocated in rdn_create()/rdn_add_link()/
 *     rdn_region_register(); nothing is allocated after rdn_start()
 *   - no recursion, every loop has a static bound
 *   - every blocking call has a finite timeout
 *   - the state machine (rdn_fsm.h) is a pure function, separately testable
 */
#ifndef RDN_H
#define RDN_H

#include <stddef.h>
#include <stdint.h>

#include "rdn_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDN_VERSION_MAJOR 1
#define RDN_VERSION_MINOR 0

#define RDN_MAX_REGIONS   16u   /* replicated memory regions per node     */
#define RDN_MAX_LINKS     2u    /* e.g. link 0 = GbE, link 1 = VME ring    */
#define RDN_MAX_FRAME     1472u /* fits one UDP datagram on 1500 MTU      */

/* ---- status codes ------------------------------------------------------ */
typedef enum {
    RDN_OK        = 0,
    RDN_E_PARAM   = -1,  /* invalid argument / configuration             */
    RDN_E_STATE   = -2,  /* call not permitted in current node state     */
    RDN_E_NOMEM   = -3,
    RDN_E_TIMEOUT = -4,
    RDN_E_IO      = -5,  /* link send/receive failure                    */
    RDN_E_NOPEER  = -6,  /* no hot standby available (degraded)          */
    RDN_E_CRC     = -7,  /* integrity check failed                       */
    RDN_E_FULL    = -8,  /* table / ring full                            */
    RDN_E_PROTO   = -9,  /* malformed or unexpected message              */
    RDN_E_CONFIG  = -10  /* peer layout / cluster mismatch               */
} rdn_status_t;

/* ---- node states (see docs/STATE_MACHINE.md) ---------------------------- */
typedef enum {
    RDN_ST_INIT = 0,      /* created, not started                         */
    RDN_ST_DISCOVER,      /* listening for peer, role not yet decided     */
    RDN_ST_STANDBY_SYNC,  /* standby, replica NOT consistent (no takeover)*/
    RDN_ST_STANDBY_HOT,   /* standby, replica consistent, eligible        */
    RDN_ST_TAKEOVER,      /* transient: claiming the arbiter              */
    RDN_ST_ACTIVE,        /* owns outputs, replicates state               */
    RDN_ST_SAFE,          /* fail-safe; outputs off; needs maint. reset   */
    RDN_ST_COUNT
} rdn_state_t;

/* ---- replication mode --------------------------------------------------- */
typedef enum {
    RDN_REPL_ASYNC = 0,   /* rdn_commit() returns after transmission      */
    RDN_REPL_SYNC  = 1    /* rdn_commit() waits for standby ACK (bounded) */
} rdn_repl_mode_t;

/* ---- optional external arbiter ------------------------------------------
 * Prevents dual-active when every communication link is lost but both
 * nodes are alive (network partition). Typical hardware realisations are
 * listed in docs/SAFETY.md (lease register, SYSFAIL line, relay token).
 * claim() must be bounded by cfg.arbiter_budget_us. A denied claim is
 * retried every monitor cycle while the peer stays silent. If no arbiter
 * is set, claims are always granted and split-brain is resolved on link
 * recovery. */
typedef struct rdn_arbiter {
    int  (*claim)(void *ctx, uint8_t node_id, uint32_t epoch); /* 1 = granted */
    /* once per monitor cycle while ACTIVE; returns 1 while the lease is
     * still held. 0 (lease lost / taken over) forces the node to SAFE. */
    int  (*renew)(void *ctx, uint8_t node_id);
    void (*release)(void *ctx, uint8_t node_id);
    void *ctx;
} rdn_arbiter_t;

/* ---- application callbacks ----------------------------------------------
 * All callbacks run in the monitor task. They must not call rdn_stop(),
 * rdn_destroy() or rdn_commit(). */
typedef struct rdn_callbacks {
    /* informational; called after every state transition */
    void (*on_state_change)(void *user, rdn_state_t from, rdn_state_t to,
                            uint32_t epoch);
    /* called on entry to ACTIVE, before outputs are permitted. cold != 0
     * means no replicated state is available. Must return 0 within
     * cfg.activate_budget_us; non-zero forces SAFE. */
    int  (*on_activate)(void *user, int cold, uint32_t last_txn);
    /* called when leaving ACTIVE; outputs are already inhibited */
    void (*on_deactivate)(void *user);
    /* optional application self-test, once per monitor cycle; 0 = ok */
    int  (*health_check)(void *user);
    void *user;
} rdn_callbacks_t;

/* ---- configuration ------------------------------------------------------ */
typedef struct rdn_config {
    uint32_t cluster_id;          /* both nodes must match                */
    uint8_t  node_id;             /* 1..254, unique within the pair       */
    uint8_t  peer_id;
    uint8_t  preferred_active;    /* node id preferred at cold start, 0 = lower id */
    uint8_t  allow_cold_takeover; /* STANDBY_SYNC may go ACTIVE cold      */

    /* timing (microseconds) - see docs/TIMING.md */
    uint32_t monitor_period_us;   /* T_mon  : monitor task period         */
    uint32_t hb_period_us;        /* T_hb   : heartbeat transmit period   */
    uint32_t hb_miss_limit;       /* N      : T_to = N * T_hb             */
    uint32_t startup_listen_us;   /* DISCOVER listen before cold start    */
    uint32_t sync_retry_us;       /* SYNC_REQ retransmit period           */
    uint32_t link_latency_max_us; /* L_max  : worst-case one-way latency  */
    uint32_t sched_jitter_us;     /* J      : worst-case release jitter   */
    uint32_t activate_budget_us;  /* T_act  : on_activate() WCET budget   */
    uint32_t arbiter_budget_us;   /* T_arb  : arbiter claim() WCET budget */

    /* replication */
    rdn_repl_mode_t repl_mode;
    uint32_t ack_timeout_us;      /* RDN_REPL_SYNC wait bound             */
    uint32_t block_size;          /* delta granularity in bytes (pow2)    */

    /* tasks (VxWorks priorities: 0 = highest; ignored on POSIX) */
    int      monitor_prio;
    int      rx_prio;
    uint32_t stack_size;
} rdn_config_t;

/* ---- statistics --------------------------------------------------------- */
typedef struct rdn_link_stats {
    uint32_t tx_frames, tx_errors;
    uint32_t rx_frames, rx_bad_crc, rx_bad_hdr, rx_replayed, rx_lost;
    uint8_t  up;                  /* heartbeat seen within T_to           */
} rdn_link_stats_t;

typedef struct rdn_stats {
    rdn_link_stats_t link[RDN_MAX_LINKS];
    uint32_t transitions;
    uint32_t takeovers;
    uint32_t split_brain_resolved;
    uint32_t txn_committed;       /* active side                          */
    uint32_t txn_applied;         /* standby side                         */
    uint32_t sync_full;           /* full snapshots applied / sent        */
    uint32_t sync_lost;           /* gaps / CRC mismatches detected       */
    uint32_t ack_timeouts;
    uint32_t monitor_overruns;
    uint32_t last_detect_us;      /* last peer-loss detection latency     */
    uint32_t max_detect_us;
    uint32_t last_takeover_us;    /* detection -> outputs permitted       */
    uint32_t max_takeover_us;
    uint32_t epoch;
    uint32_t last_txn;
    rdn_state_t state;
    rdn_state_t peer_state;
    uint8_t  peer_alive;
} rdn_stats_t;

typedef struct rdn_node rdn_node_t;

/* ---- lifecycle ---------------------------------------------------------- */
void        rdn_config_defaults(rdn_config_t *cfg);
int         rdn_config_validate(const rdn_config_t *cfg);
int         rdn_create(const rdn_config_t *cfg, const rdn_callbacks_t *cb,
                       rdn_node_t **out);
int         rdn_add_link(rdn_node_t *n, const rdn_link_t *link);
int         rdn_set_arbiter(rdn_node_t *n, const rdn_arbiter_t *arb);
/* Register a replicated region. Same ids/sizes/order on both nodes. The
 * buffer is owned by the application; on the standby it is overwritten
 * atomically (per transaction) by the replication engine. */
int         rdn_region_register(rdn_node_t *n, uint16_t id, void *buf,
                                uint32_t size);
int         rdn_start(rdn_node_t *n);
int         rdn_stop(rdn_node_t *n);          /* graceful; relinquishes   */
void        rdn_destroy(rdn_node_t *n);

/* ---- runtime ------------------------------------------------------------ */
rdn_state_t rdn_state(const rdn_node_t *n);
/* The ONLY predicate an application should use to gate safety outputs:
 * ACTIVE, on_activate() completed, and monitor task alive. */
int         rdn_output_permitted(const rdn_node_t *n);
/* Application cycle bracket (recommended pattern):
 *
 *     if (rdn_cycle_begin(n)) {            // active: regions are ours
 *         ... compute, write replicated regions ...
 *         if (rdn_output_permitted(n)) { ... drive safety outputs ... }
 *         rc = rdn_cycle_end(n);           // replicate + release
 *     }
 *
 * rdn_cycle_begin() returns 1 only if outputs are permitted; the caller
 * then owns the regions and role changes are deferred until
 * rdn_cycle_end(), which MUST follow (it commits like rdn_commit()). It
 * returns 0 otherwise and holds nothing. Safety inhibition (fault, lost
 * arbiter lease) is never deferred, hence the output check inside the
 * cycle. Do not call rdn_commit() inside a cycle. The cycle's WCET adds to
 * the switchover time of the node being demoted (docs/TIMING.md). */
int         rdn_cycle_begin(rdn_node_t *n);
int         rdn_cycle_end(rdn_node_t *n);
/* Active only: replicate all dirty regions as one atomic transaction.
 * Lower-level alternative to the cycle bracket: the caller must itself
 * ensure it stops writing the regions once it is no longer active. */
int         rdn_commit(rdn_node_t *n);
int         rdn_request_switchover(rdn_node_t *n);   /* planned, active only */
int         rdn_report_fault(rdn_node_t *n, uint32_t code); /* -> SAFE      */
int         rdn_maintenance_reset(rdn_node_t *n);    /* SAFE -> DISCOVER    */
void        rdn_get_stats(rdn_node_t *n, rdn_stats_t *st);

/* Lock/unlock the replica for a consistent read on the standby. */
void        rdn_region_lock(rdn_node_t *n);
void        rdn_region_unlock(rdn_node_t *n);

/* ---- timing analysis (docs/TIMING.md) ----------------------------------- */
typedef struct rdn_timing_bound {
    uint32_t detect_us;       /* failure -> peer loss declared            */
    uint32_t takeover_us;     /* failure -> outputs permitted on standby  */
    uint32_t switchover_us;   /* planned relinquish -> outputs permitted  */
    uint32_t min_false_us;    /* min heartbeat silence that trips timeout */
} rdn_timing_bound_t;
void        rdn_timing_bound(const rdn_config_t *cfg, uint32_t clock_res_us,
                             rdn_timing_bound_t *out);

const char *rdn_state_name(rdn_state_t s);
const char *rdn_status_name(int status);

/* ---- reference lease arbiter --------------------------------------------
 * In-memory lease, the software model of a hardware lease/token (e.g. a
 * watchdog-backed ownership register). Both nodes must share the same
 * instance, so on real hardware it is only meaningful when placed behind a
 * device both CPUs can reach atomically. Used by the host test-suite.
 * Choose lease_us <= T_to - T_hb - J so that a crashed owner's lease has
 * expired by the time the standby detects the failure. */
int  rdn_lease_arbiter_create(uint32_t lease_us, rdn_arbiter_t *out);
void rdn_lease_arbiter_destroy(rdn_arbiter_t *arb);
uint8_t rdn_lease_arbiter_owner(const rdn_arbiter_t *arb);

/* ---- test support ------------------------------------------------------- */
/* Simulates an abrupt board failure: the node stops transmitting, receiving
 * and running its state machine, without any graceful action. */
void        rdn_test_freeze(rdn_node_t *n);

#ifdef __cplusplus
}
#endif
#endif /* RDN_H */
