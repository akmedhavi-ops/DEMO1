/*
 * rdn_fsm.c - redundancy state machine
 *
 * The transition table implemented here is documented, row for row, in
 * docs/STATE_MACHINE.md and exercised exhaustively by tests/test_fsm.c.
 * Any change here MUST be reflected in both.
 */
#include "rdn_fsm.h"

void rdn_fsm_init(rdn_fsm_t *f, uint8_t node_id, uint8_t peer_id,
                  uint8_t preferred, uint8_t allow_cold)
{
    f->state      = RDN_ST_INIT;
    f->resume     = RDN_ST_INIT;
    f->epoch      = 0u;
    f->node_id    = node_id;
    f->peer_id    = peer_id;
    f->preferred  = preferred;
    f->allow_cold = allow_cold ? 1u : 0u;
    f->cold       = 0u;
}

/* Deterministic tie-break, identical on both nodes given the same config:
 * the configured preferred node wins, otherwise the lower node id. */
int rdn_fsm_wins_tie(const rdn_fsm_t *f)
{
    if (f->preferred == f->node_id) {
        return 1;
    }
    if (f->preferred == f->peer_id) {
        return 0;
    }
    return (f->node_id < f->peer_id) ? 1 : 0;
}

static int peer_is_active(const rdn_fsm_peer_t *p)
{
    return (p->state == RDN_ST_ACTIVE) || (p->state == RDN_ST_TAKEOVER);
}

static uint32_t begin_takeover(rdn_fsm_t *f, int cold)
{
    f->resume = f->state;
    f->cold   = cold ? 1u : 0u;
    f->state  = RDN_ST_TAKEOVER;
    return RDN_ACT_CLAIM;
}

/* Standby without a consistent replica loses its active peer. */
static uint32_t lost_active_unsynced(rdn_fsm_t *f)
{
    if (f->allow_cold) {
        return begin_takeover(f, 1);
    }
    f->state = RDN_ST_SAFE;
    return 0u;
}

static void adopt_epoch(rdn_fsm_t *f, const rdn_fsm_peer_t *p)
{
    if (p->epoch > f->epoch) {
        f->epoch = p->epoch;
    }
}

static uint32_t step_discover(rdn_fsm_t *f, rdn_event_t ev,
                              const rdn_fsm_peer_t *p)
{
    switch (ev) {
    case RDN_EV_PEER_HB:
        if (peer_is_active(p)) {
            adopt_epoch(f, p);
            f->state = RDN_ST_STANDBY_SYNC;
            return RDN_ACT_SEND_SYNC_REQ;
        }
        if (p->state == RDN_ST_SAFE) {
            return begin_takeover(f, 1);
        }
        if ((p->state == RDN_ST_DISCOVER) && rdn_fsm_wins_tie(f)) {
            return begin_takeover(f, 1);
        }
        return 0u;  /* peer standby/discover-loser: wait for peer to act */
    case RDN_EV_STARTUP_TIMEOUT:
        return begin_takeover(f, 1);
    case RDN_EV_PEER_TIMEOUT:
        return RDN_ACT_START_LISTEN;
    default:
        return 0u;
    }
}

static uint32_t step_standby_sync(rdn_fsm_t *f, rdn_event_t ev,
                                  const rdn_fsm_peer_t *p)
{
    switch (ev) {
    case RDN_EV_SYNC_COMPLETE:
        f->state = RDN_ST_STANDBY_HOT;
        return 0u;
    case RDN_EV_SYNC_LOST:
        return RDN_ACT_SEND_SYNC_REQ;
    case RDN_EV_PEER_HB:
        if (peer_is_active(p)) {
            adopt_epoch(f, p);
            return 0u;
        }
        if (p->state == RDN_ST_SAFE) {
            return lost_active_unsynced(f);
        }
        /* peer DISCOVER or standby: nobody is active, renegotiate */
        f->state = RDN_ST_DISCOVER;
        return RDN_ACT_START_LISTEN;
    case RDN_EV_PEER_TIMEOUT:
    case RDN_EV_PEER_RELINQUISH:
        return lost_active_unsynced(f);
    default:
        return 0u;
    }
}

static uint32_t step_standby_hot(rdn_fsm_t *f, rdn_event_t ev,
                                 const rdn_fsm_peer_t *p)
{
    switch (ev) {
    case RDN_EV_PEER_TIMEOUT:
    case RDN_EV_PEER_RELINQUISH:
        return begin_takeover(f, 0);
    case RDN_EV_SYNC_LOST:
        f->state = RDN_ST_STANDBY_SYNC;
        return RDN_ACT_SEND_SYNC_REQ;
    case RDN_EV_PEER_HB:
        if (peer_is_active(p)) {
            adopt_epoch(f, p);
            return 0u;
        }
        if ((p->state == RDN_ST_STANDBY_HOT) && !rdn_fsm_wins_tie(f)) {
            return 0u;  /* both hot: the tie winner takes over */
        }
        return begin_takeover(f, 0);
    default:
        return 0u;
    }
}

static uint32_t step_takeover(rdn_fsm_t *f, rdn_event_t ev,
                              const rdn_fsm_peer_t *p)
{
    switch (ev) {
    case RDN_EV_CLAIM_GRANTED:
        f->epoch = ((p->epoch > f->epoch) ? p->epoch : f->epoch) + 1u;
        f->state = RDN_ST_ACTIVE;
        return RDN_ACT_ACTIVATE;
    case RDN_EV_CLAIM_DENIED:
        f->state = f->resume;
        return (f->state == RDN_ST_DISCOVER) ? RDN_ACT_START_LISTEN : 0u;
    default:
        return 0u;
    }
}

static uint32_t step_active(rdn_fsm_t *f, rdn_event_t ev,
                            const rdn_fsm_peer_t *p)
{
    switch (ev) {
    case RDN_EV_PEER_HB:
        if (p->state == RDN_ST_ACTIVE) {
            /* dual active: higher epoch wins, tie-break on equal epoch */
            if ((p->epoch > f->epoch) ||
                ((p->epoch == f->epoch) && !rdn_fsm_wins_tie(f))) {
                f->epoch = p->epoch;
                f->state = RDN_ST_STANDBY_SYNC;
                return RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE |
                       RDN_ACT_SEND_SYNC_REQ | RDN_ACT_SPLIT_BRAIN;
            }
            return RDN_ACT_SPLIT_BRAIN;
        }
        return 0u;
    case RDN_EV_PEER_TIMEOUT:
        return RDN_ACT_NOTIFY_PEER_LOST;
    case RDN_EV_SWITCHOVER_REQ:
        if (p->alive && (p->state == RDN_ST_STANDBY_HOT)) {
            f->state = RDN_ST_STANDBY_SYNC;
            return RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE |
                   RDN_ACT_SEND_RELINQUISH;
        }
        return RDN_ACT_REJECT;
    default:
        return 0u;
    }
}

uint32_t rdn_fsm_step(rdn_fsm_t *f, rdn_event_t ev, const rdn_fsm_peer_t *p)
{
    /* Events valid in every started state. */
    if (ev == RDN_EV_STOP) {
        uint32_t a = 0u;
        if (f->state == RDN_ST_ACTIVE) {
            a = RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE;
            if (p->alive && (p->state == RDN_ST_STANDBY_HOT)) {
                a |= RDN_ACT_SEND_RELINQUISH;
            }
        } else if (f->state == RDN_ST_TAKEOVER) {
            a = RDN_ACT_RELEASE;
        } else {
            /* nothing to undo */
        }
        f->state = RDN_ST_INIT;
        return a;
    }
    if ((ev == RDN_EV_LOCAL_FAULT) &&
        (f->state != RDN_ST_INIT) && (f->state != RDN_ST_SAFE)) {
        uint32_t a = 0u;
        if (f->state == RDN_ST_ACTIVE) {
            a = RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE;
            if (p->alive && (p->state == RDN_ST_STANDBY_HOT)) {
                a |= RDN_ACT_SEND_RELINQUISH;
            }
        } else if (f->state == RDN_ST_TAKEOVER) {
            a = RDN_ACT_RELEASE;
        } else {
            /* standby / discover: nothing to undo */
        }
        f->state = RDN_ST_SAFE;
        return a;
    }

    switch (f->state) {
    case RDN_ST_INIT:
        if (ev == RDN_EV_START) {
            f->state = RDN_ST_DISCOVER;
            return RDN_ACT_START_LISTEN;
        }
        return 0u;
    case RDN_ST_DISCOVER:
        return step_discover(f, ev, p);
    case RDN_ST_STANDBY_SYNC:
        return step_standby_sync(f, ev, p);
    case RDN_ST_STANDBY_HOT:
        return step_standby_hot(f, ev, p);
    case RDN_ST_TAKEOVER:
        return step_takeover(f, ev, p);
    case RDN_ST_ACTIVE:
        return step_active(f, ev, p);
    case RDN_ST_SAFE:
        if (ev == RDN_EV_MAINT_RESET) {
            f->state = RDN_ST_DISCOVER;
            return RDN_ACT_START_LISTEN;
        }
        return 0u;
    default:
        /* corrupted state variable: fail safe */
        f->state = RDN_ST_SAFE;
        return RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE;
    }
}

const char *rdn_event_name(rdn_event_t ev)
{
    static const char *const names[RDN_EV_COUNT] = {
        "START", "PEER_HB", "PEER_TIMEOUT", "STARTUP_TIMEOUT",
        "SYNC_COMPLETE", "SYNC_LOST", "LOCAL_FAULT", "PEER_RELINQUISH",
        "SWITCHOVER_REQ", "CLAIM_GRANTED", "CLAIM_DENIED", "MAINT_RESET",
        "STOP"
    };
    return ((unsigned)ev < (unsigned)RDN_EV_COUNT) ? names[ev] : "?";
}
