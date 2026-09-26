/*
 * rdn_fsm.h - redundancy state machine (pure, no OS dependencies)
 *
 * rdn_fsm_step() is a deterministic function of (state, event, peer info):
 * it performs no I/O and returns a bitmask of actions for the runtime to
 * execute in the fixed order listed below. This lets the complete
 * transition table be verified on a host (tests/test_fsm.c) and reviewed
 * against docs/STATE_MACHINE.md.
 */
#ifndef RDN_FSM_H
#define RDN_FSM_H

#include <stdint.h>
#include "rdn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RDN_EV_START = 0,       /* rdn_start()                                 */
    RDN_EV_PEER_HB,         /* valid heartbeat(s) received this cycle      */
    RDN_EV_PEER_TIMEOUT,    /* no valid heartbeat on ANY link for T_to     */
    RDN_EV_STARTUP_TIMEOUT, /* DISCOVER listen time elapsed, no peer       */
    RDN_EV_SYNC_COMPLETE,   /* full snapshot applied and verified          */
    RDN_EV_SYNC_LOST,       /* replication gap / CRC mismatch              */
    RDN_EV_LOCAL_FAULT,     /* self-test failure / rdn_report_fault()      */
    RDN_EV_PEER_RELINQUISH, /* active peer handed over deliberately        */
    RDN_EV_SWITCHOVER_REQ,  /* operator requested planned switchover       */
    RDN_EV_CLAIM_GRANTED,   /* arbiter granted activation                  */
    RDN_EV_CLAIM_DENIED,    /* arbiter refused activation                  */
    RDN_EV_MAINT_RESET,     /* maintenance reset from SAFE                 */
    RDN_EV_STOP,            /* rdn_stop()                                  */
    RDN_EV_COUNT
} rdn_event_t;

/* Actions, executed by the runtime in ascending bit order. */
#define RDN_ACT_DEACTIVATE      0x0001u /* inhibit outputs, on_deactivate()  */
#define RDN_ACT_RELEASE         0x0002u /* arbiter release()                 */
#define RDN_ACT_SEND_RELINQUISH 0x0004u
#define RDN_ACT_SEND_SYNC_REQ   0x0008u
#define RDN_ACT_START_LISTEN    0x0010u /* (re)start DISCOVER listen timer   */
#define RDN_ACT_CLAIM           0x0020u /* arbiter claim() -> GRANTED/DENIED */
#define RDN_ACT_ACTIVATE        0x0040u /* on_activate(), then permit outputs*/
#define RDN_ACT_NOTIFY_PEER_LOST 0x0080u
#define RDN_ACT_REJECT          0x0100u /* request refused in this state     */
#define RDN_ACT_SPLIT_BRAIN     0x0200u /* dual-active detected and resolved */

typedef struct rdn_fsm_peer {
    rdn_state_t state;      /* last state reported by peer                 */
    uint32_t    epoch;      /* last epoch reported by peer                 */
    uint8_t     alive;      /* heartbeat within T_to on some link          */
} rdn_fsm_peer_t;

typedef struct rdn_fsm {
    rdn_state_t state;
    rdn_state_t resume;     /* state to return to if a claim is denied     */
    uint32_t    epoch;      /* activation generation, monotonic            */
    uint8_t     node_id;
    uint8_t     peer_id;
    uint8_t     preferred;  /* preferred active node id, 0 = lower id      */
    uint8_t     allow_cold;
    uint8_t     cold;       /* current/last takeover had no valid replica  */
} rdn_fsm_t;

void     rdn_fsm_init(rdn_fsm_t *f, uint8_t node_id, uint8_t peer_id,
                      uint8_t preferred, uint8_t allow_cold);
uint32_t rdn_fsm_step(rdn_fsm_t *f, rdn_event_t ev, const rdn_fsm_peer_t *peer);
int      rdn_fsm_wins_tie(const rdn_fsm_t *f);
const char *rdn_event_name(rdn_event_t ev);

#ifdef __cplusplus
}
#endif
#endif /* RDN_FSM_H */
