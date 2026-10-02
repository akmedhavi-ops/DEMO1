/*
 * test_fsm.c - state machine verification
 *
 *  1. transition table: every row of docs/STATE_MACHINE.md
 *  2. exhaustive structural invariants over state x event x peer view
 *  3. randomised two-node simulation (crashes, partitions, faults) with and
 *     without an arbiter: safety (never two ACTIVE with arbiter) and
 *     liveness (converges to ACTIVE + STANDBY_HOT once faults stop)
 */
#include <string.h>
#include "rdn_fsm.h"
#include "rdn_test.h"

#define S(x) RDN_ST_##x
#define E(x) RDN_EV_##x

typedef struct row {
    rdn_state_t from;
    rdn_event_t ev;
    rdn_state_t peer_state;
    uint8_t     peer_alive;
    uint32_t    peer_epoch;
    uint8_t     wins_tie;    /* 1: this node is node 1 (wins), 0: node 2 */
    uint8_t     allow_cold;
    rdn_state_t to;
    uint32_t    acts;
} row_t;

static const row_t table[] = {
 /* from             event                peer          alive ep tie cold  to                 actions */
 { S(INIT),          E(START),            S(INIT),        0, 0, 1, 0, S(DISCOVER),     RDN_ACT_START_LISTEN },
 { S(INIT),          E(PEER_HB),          S(ACTIVE),      1, 0, 1, 0, S(INIT),         0 },
 /* DISCOVER */
 { S(DISCOVER),      E(PEER_HB),          S(ACTIVE),      1, 3, 1, 0, S(STANDBY_SYNC), RDN_ACT_SEND_SYNC_REQ },
 { S(DISCOVER),      E(PEER_HB),          S(TAKEOVER),    1, 3, 0, 0, S(STANDBY_SYNC), RDN_ACT_SEND_SYNC_REQ },
 { S(DISCOVER),      E(PEER_HB),          S(DISCOVER),    1, 0, 1, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(DISCOVER),      E(PEER_HB),          S(DISCOVER),    1, 0, 0, 0, S(DISCOVER),     0 },
 { S(DISCOVER),      E(PEER_HB),          S(SAFE),        1, 0, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(DISCOVER),      E(PEER_HB),          S(STANDBY_HOT), 1, 0, 1, 0, S(DISCOVER),     0 },
 { S(DISCOVER),      E(PEER_HB),          S(STANDBY_SYNC),1, 0, 1, 0, S(DISCOVER),     0 },
 { S(DISCOVER),      E(STARTUP_TIMEOUT),  S(INIT),        0, 0, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(DISCOVER),      E(PEER_TIMEOUT),     S(DISCOVER),    0, 0, 0, 0, S(DISCOVER),     RDN_ACT_START_LISTEN },
 { S(DISCOVER),      E(LOCAL_FAULT),      S(INIT),        0, 0, 0, 0, S(SAFE),         0 },
 /* STANDBY_SYNC */
 { S(STANDBY_SYNC),  E(SYNC_COMPLETE),    S(ACTIVE),      1, 1, 0, 0, S(STANDBY_HOT),  0 },
 { S(STANDBY_SYNC),  E(SYNC_LOST),        S(ACTIVE),      1, 1, 0, 0, S(STANDBY_SYNC), RDN_ACT_SEND_SYNC_REQ },
 { S(STANDBY_SYNC),  E(PEER_HB),          S(ACTIVE),      1, 1, 0, 0, S(STANDBY_SYNC), 0 },
 { S(STANDBY_SYNC),  E(PEER_HB),          S(DISCOVER),    1, 0, 0, 0, S(DISCOVER),     RDN_ACT_START_LISTEN },
 { S(STANDBY_SYNC),  E(PEER_HB),          S(STANDBY_HOT), 1, 0, 0, 0, S(DISCOVER),     RDN_ACT_START_LISTEN },
 { S(STANDBY_SYNC),  E(PEER_HB),          S(SAFE),        1, 0, 0, 0, S(SAFE),         0 },
 { S(STANDBY_SYNC),  E(PEER_HB),          S(SAFE),        1, 0, 0, 1, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_SYNC),  E(PEER_TIMEOUT),     S(ACTIVE),      0, 1, 0, 0, S(SAFE),         0 },
 { S(STANDBY_SYNC),  E(PEER_TIMEOUT),     S(ACTIVE),      0, 1, 0, 1, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_SYNC),  E(PEER_RELINQUISH),  S(STANDBY_SYNC),1, 1, 0, 0, S(SAFE),         0 },
 { S(STANDBY_SYNC),  E(LOCAL_FAULT),      S(ACTIVE),      1, 1, 0, 0, S(SAFE),         0 },
 /* STANDBY_HOT */
 { S(STANDBY_HOT),   E(PEER_TIMEOUT),     S(ACTIVE),      0, 1, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(PEER_RELINQUISH),  S(STANDBY_SYNC),1, 1, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(SYNC_LOST),        S(ACTIVE),      1, 1, 0, 0, S(STANDBY_SYNC), RDN_ACT_SEND_SYNC_REQ },
 { S(STANDBY_HOT),   E(PEER_HB),          S(ACTIVE),      1, 1, 0, 0, S(STANDBY_HOT),  0 },
 { S(STANDBY_HOT),   E(PEER_HB),          S(DISCOVER),    1, 1, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(PEER_HB),          S(SAFE),        1, 1, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(PEER_HB),          S(STANDBY_SYNC),1, 1, 0, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(PEER_HB),          S(STANDBY_HOT), 1, 1, 1, 0, S(TAKEOVER),     RDN_ACT_CLAIM },
 { S(STANDBY_HOT),   E(PEER_HB),          S(STANDBY_HOT), 1, 1, 0, 0, S(STANDBY_HOT),  0 },
 { S(STANDBY_HOT),   E(LOCAL_FAULT),      S(ACTIVE),      1, 1, 0, 0, S(SAFE),         0 },
 /* ACTIVE (own epoch is 5 in these rows) */
 { S(ACTIVE),        E(PEER_HB),          S(STANDBY_HOT), 1, 5, 1, 0, S(ACTIVE),       0 },
 { S(ACTIVE),        E(PEER_HB),          S(ACTIVE),      1, 6, 1, 0, S(STANDBY_SYNC), RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE|RDN_ACT_SEND_SYNC_REQ|RDN_ACT_SPLIT_BRAIN },
 { S(ACTIVE),        E(PEER_HB),          S(ACTIVE),      1, 4, 0, 0, S(ACTIVE),       RDN_ACT_SPLIT_BRAIN },
 { S(ACTIVE),        E(PEER_HB),          S(ACTIVE),      1, 5, 1, 0, S(ACTIVE),       RDN_ACT_SPLIT_BRAIN },
 { S(ACTIVE),        E(PEER_HB),          S(ACTIVE),      1, 5, 0, 0, S(STANDBY_SYNC), RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE|RDN_ACT_SEND_SYNC_REQ|RDN_ACT_SPLIT_BRAIN },
 { S(ACTIVE),        E(PEER_TIMEOUT),     S(STANDBY_HOT), 0, 5, 0, 0, S(ACTIVE),       RDN_ACT_NOTIFY_PEER_LOST },
 { S(ACTIVE),        E(SWITCHOVER_REQ),   S(STANDBY_HOT), 1, 5, 0, 0, S(STANDBY_SYNC), RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE|RDN_ACT_SEND_RELINQUISH },
 { S(ACTIVE),        E(SWITCHOVER_REQ),   S(STANDBY_SYNC),1, 5, 0, 0, S(ACTIVE),       RDN_ACT_REJECT },
 { S(ACTIVE),        E(SWITCHOVER_REQ),   S(STANDBY_HOT), 0, 5, 0, 0, S(ACTIVE),       RDN_ACT_REJECT },
 { S(ACTIVE),        E(LOCAL_FAULT),      S(STANDBY_HOT), 1, 5, 0, 0, S(SAFE),         RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE|RDN_ACT_SEND_RELINQUISH },
 { S(ACTIVE),        E(LOCAL_FAULT),      S(STANDBY_SYNC),1, 5, 0, 0, S(SAFE),         RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE },
 { S(ACTIVE),        E(STOP),             S(STANDBY_HOT), 1, 5, 0, 0, S(INIT),         RDN_ACT_DEACTIVATE|RDN_ACT_RELEASE|RDN_ACT_SEND_RELINQUISH },
 { S(ACTIVE),        E(PEER_RELINQUISH),  S(STANDBY_HOT), 1, 5, 0, 0, S(ACTIVE),       0 },
 /* TAKEOVER (resume state set by the helper below) */
 { S(TAKEOVER),      E(CLAIM_GRANTED),    S(ACTIVE),      0, 7, 0, 0, S(ACTIVE),       RDN_ACT_ACTIVATE },
 { S(TAKEOVER),      E(CLAIM_DENIED),     S(ACTIVE),      0, 7, 0, 0, S(STANDBY_HOT),  0 },
 { S(TAKEOVER),      E(LOCAL_FAULT),      S(ACTIVE),      0, 7, 0, 0, S(SAFE),         RDN_ACT_RELEASE },
 { S(TAKEOVER),      E(PEER_HB),          S(ACTIVE),      1, 7, 0, 0, S(TAKEOVER),     0 },
 /* SAFE */
 { S(SAFE),          E(PEER_HB),          S(DISCOVER),    1, 0, 1, 1, S(SAFE),         0 },
 { S(SAFE),          E(LOCAL_FAULT),      S(ACTIVE),      1, 0, 1, 1, S(SAFE),         0 },
 { S(SAFE),          E(STARTUP_TIMEOUT),  S(INIT),        0, 0, 1, 1, S(SAFE),         0 },
 { S(SAFE),          E(MAINT_RESET),      S(ACTIVE),      1, 0, 1, 1, S(DISCOVER),     RDN_ACT_START_LISTEN },
 { S(SAFE),          E(STOP),             S(ACTIVE),      1, 0, 1, 1, S(INIT),         0 },
};

static void test_transition_table(void)
{
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const row_t *r = &table[i];
        rdn_fsm_t f;
        rdn_fsm_peer_t p;
        uint32_t acts;

        rdn_fsm_init(&f, r->wins_tie ? 1u : 2u, r->wins_tie ? 2u : 1u,
                     0u, r->allow_cold);
        f.state  = r->from;
        f.epoch  = (r->from == RDN_ST_ACTIVE) ? 5u : 1u;
        f.resume = RDN_ST_STANDBY_HOT;
        p.state  = r->peer_state;
        p.alive  = r->peer_alive;
        p.epoch  = r->peer_epoch;
        acts = rdn_fsm_step(&f, r->ev, &p);
        if ((f.state != r->to) || (acts != r->acts)) {
            fprintf(stderr, "  row %zu: %s + %s (peer %s): got %s/0x%x, want %s/0x%x\n",
                    i, rdn_state_name(r->from), rdn_event_name(r->ev),
                    rdn_state_name(r->peer_state), rdn_state_name(f.state),
                    acts, rdn_state_name(r->to), r->acts);
        }
        CHECK_EQ(f.state, r->to);
        CHECK_EQ(acts, r->acts);
    }
}

static void test_epoch_rules(void)
{
    rdn_fsm_t f;
    rdn_fsm_peer_t p = { RDN_ST_ACTIVE, 9u, 1u };

    rdn_fsm_init(&f, 1u, 2u, 0u, 0u);
    f.state = RDN_ST_STANDBY_HOT;
    f.epoch = 3u;
    (void)rdn_fsm_step(&f, RDN_EV_PEER_HB, &p);
    CHECK_EQ(f.epoch, 9u);                       /* standby adopts epoch */
    p.alive = 0u;
    (void)rdn_fsm_step(&f, RDN_EV_PEER_TIMEOUT, &p);
    CHECK_EQ(f.state, RDN_ST_TAKEOVER);
    (void)rdn_fsm_step(&f, RDN_EV_CLAIM_GRANTED, &p);
    CHECK_EQ(f.state, RDN_ST_ACTIVE);
    CHECK_EQ(f.epoch, 10u);                      /* new generation */
    CHECK_EQ(f.cold, 0u);

    /* cold start from DISCOVER marks cold */
    rdn_fsm_init(&f, 1u, 2u, 0u, 0u);
    (void)rdn_fsm_step(&f, RDN_EV_START, &p);
    (void)rdn_fsm_step(&f, RDN_EV_STARTUP_TIMEOUT, &p);
    CHECK_EQ(f.cold, 1u);
    (void)rdn_fsm_step(&f, RDN_EV_CLAIM_DENIED, &p);
    CHECK_EQ(f.state, RDN_ST_DISCOVER);
}

static void test_tie_break(void)
{
    rdn_fsm_t a, b;
    rdn_fsm_init(&a, 3u, 7u, 0u, 0u);
    rdn_fsm_init(&b, 7u, 3u, 0u, 0u);
    CHECK(rdn_fsm_wins_tie(&a) && !rdn_fsm_wins_tie(&b));
    rdn_fsm_init(&a, 3u, 7u, 7u, 0u);
    rdn_fsm_init(&b, 7u, 3u, 7u, 0u);
    CHECK(!rdn_fsm_wins_tie(&a) && rdn_fsm_wins_tie(&b));
}

/* Structural invariants over the whole input space. */
static void test_exhaustive_invariants(void)
{
    int s, e, ps, alive, tie, cold, pe;
    long combos = 0;

    for (s = 0; s < (int)RDN_ST_COUNT; s++)
    for (e = 0; e < (int)RDN_EV_COUNT; e++)
    for (ps = 0; ps < (int)RDN_ST_COUNT; ps++)
    for (alive = 0; alive < 2; alive++)
    for (tie = 0; tie < 2; tie++)
    for (cold = 0; cold < 2; cold++)
    for (pe = 0; pe < 3; pe++) {
        rdn_fsm_t f;
        rdn_fsm_peer_t p;
        uint32_t acts, ep0;
        rdn_state_t from = (rdn_state_t)s, to;

        rdn_fsm_init(&f, tie ? 1u : 2u, tie ? 2u : 1u, 0u, (uint8_t)cold);
        f.state  = from;
        f.epoch  = 4u;
        f.resume = RDN_ST_DISCOVER;
        p.state  = (rdn_state_t)ps;
        p.alive  = (uint8_t)alive;
        p.epoch  = (uint32_t)(3 + pe);           /* lower, equal, higher */
        ep0      = f.epoch;
        acts = rdn_fsm_step(&f, (rdn_event_t)e, &p);
        to = f.state;
        combos++;

        CHECK((unsigned)to < (unsigned)RDN_ST_COUNT);
        CHECK(f.epoch >= ep0);                               /* monotonic */
        /* ACTIVE is entered only from TAKEOVER, with ACTIVATE */
        if ((to == RDN_ST_ACTIVE) && (from != RDN_ST_ACTIVE)) {
            CHECK(from == RDN_ST_TAKEOVER);
            CHECK((acts & RDN_ACT_ACTIVATE) != 0u);
            CHECK(f.epoch > p.epoch && f.epoch > ep0);
        }
        CHECK(((acts & RDN_ACT_ACTIVATE) == 0u) || (to == RDN_ST_ACTIVE));
        /* leaving ACTIVE always inhibits outputs and releases arbiter */
        if ((from == RDN_ST_ACTIVE) && (to != RDN_ST_ACTIVE)) {
            CHECK((acts & (RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE)) ==
                  (RDN_ACT_DEACTIVATE | RDN_ACT_RELEASE));
        }
        CHECK(((acts & RDN_ACT_DEACTIVATE) == 0u) || (from == RDN_ST_ACTIVE));
        /* CLAIM iff entering TAKEOVER */
        CHECK(((acts & RDN_ACT_CLAIM) != 0u) ==
              ((to == RDN_ST_TAKEOVER) && (from != RDN_ST_TAKEOVER)));
        /* SAFE is left only by maintenance reset or stop */
        if ((from == RDN_ST_SAFE) && (to != RDN_ST_SAFE)) {
            CHECK((e == RDN_EV_MAINT_RESET) || (e == RDN_EV_STOP));
        }
        /* local fault always ends in SAFE (once started) */
        if ((e == RDN_EV_LOCAL_FAULT) && (from != RDN_ST_INIT)) {
            CHECK(to == RDN_ST_SAFE);
        }
        /* only a hot standby may take over warm */
        if ((to == RDN_ST_TAKEOVER) && (from != RDN_ST_TAKEOVER) && !f.cold) {
            CHECK(from == RDN_ST_STANDBY_HOT);
        }
        /* unsynchronised standby takes over only if allowed */
        if ((from == RDN_ST_STANDBY_SYNC) && (to == RDN_ST_TAKEOVER)) {
            CHECK(cold == 1);
        }
    }
    CHECK(combos == (long)RDN_ST_COUNT * RDN_EV_COUNT * RDN_ST_COUNT * 24);
}

/* ---- two-node randomised simulation ------------------------------------ */

typedef struct simnode {
    rdn_fsm_t f;
    int up;               /* powered and running */
    int since_hb;         /* rounds since last heartbeat from peer */
    int peer_alive;
    int listen;           /* rounds in DISCOVER */
    int safe_rounds;
} simnode_t;

typedef struct sim {
    simnode_t n[2];
    int use_arb;
    int owner;            /* 0 = free, else node id */
    int lease_left;
    int connected;
    uint32_t rng;
    int max_dual;         /* safety violations */
} sim_t;

#define T_TO     3
#define T_LISTEN 5
#define LEASE    2

static uint32_t rnd(sim_t *s, uint32_t m)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (s->rng >> 10) % m;
}

static void sim_event(sim_t *s, int i, rdn_event_t ev);

static void sim_actions(sim_t *s, int i, uint32_t acts)
{
    simnode_t *me = &s->n[i];
    uint8_t id = me->f.node_id;

    if ((acts & RDN_ACT_RELEASE) && s->use_arb && (s->owner == id)) {
        s->owner = 0;
    }
    if (acts & RDN_ACT_START_LISTEN) {
        me->listen = 0;
    }
    if (acts & RDN_ACT_CLAIM) {
        int granted = 1;
        if (s->use_arb) {
            granted = (s->owner == 0) || (s->owner == id) || (s->lease_left <= 0);
            if (granted) {
                s->owner = id;
                s->lease_left = LEASE;
            }
        }
        sim_event(s, i, granted ? RDN_EV_CLAIM_GRANTED : RDN_EV_CLAIM_DENIED);
    }
}

static void sim_event(sim_t *s, int i, rdn_event_t ev)
{
    simnode_t *me = &s->n[i], *pe = &s->n[1 - i];
    rdn_fsm_peer_t p;
    uint32_t acts;

    p.state = pe->f.state;
    p.epoch = pe->f.epoch;
    p.alive = (uint8_t)me->peer_alive;
    acts = rdn_fsm_step(&me->f, ev, &p);
    sim_actions(s, i, acts);
}

static void sim_round(sim_t *s, int faults)
{
    int i, actives = 0;

    /* fault injection */
    if (faults) {
        uint32_t r = rnd(s, 100);
        int who = (int)rnd(s, 2);
        if (r < 3 && s->n[who].up) {                    /* crash */
            s->n[who].up = 0;
        } else if (r < 8 && !s->n[who].up) {            /* reboot */
            rdn_fsm_init(&s->n[who].f, s->n[who].f.node_id, s->n[who].f.peer_id, 0u, 0u);
            s->n[who].up = 1;
            s->n[who].since_hb = T_TO + 1;
            s->n[who].peer_alive = 0;
            sim_event(s, who, RDN_EV_START);
        } else if (r < 11) {                           /* partition toggle */
            s->connected = !s->connected;
        } else if (r < 12 && s->n[who].up) {
            sim_event(s, who, RDN_EV_LOCAL_FAULT);
        } else if (r < 13 && s->n[who].up) {
            sim_event(s, who, RDN_EV_SWITCHOVER_REQ);
        }
    }
    /* arbiter lease: renewed by a live, active owner */
    if (s->use_arb && s->owner != 0) {
        int oi = (s->n[0].f.node_id == s->owner) ? 0 : 1;
        if (s->n[oi].up && s->n[oi].f.state == RDN_ST_ACTIVE) {
            s->lease_left = LEASE;
        } else {
            s->lease_left--;
        }
    }
    for (i = 0; i < 2; i++) {
        simnode_t *me = &s->n[i], *pe = &s->n[1 - i];
        if (!me->up) {
            continue;
        }
        if (s->connected && pe->up) {
            me->since_hb = 0;
            me->peer_alive = 1;
            sim_event(s, i, RDN_EV_PEER_HB);
            if ((me->f.state == RDN_ST_STANDBY_SYNC) &&
                (pe->f.state == RDN_ST_ACTIVE)) {
                sim_event(s, i, RDN_EV_SYNC_COMPLETE);
            }
        } else {
            me->since_hb++;
            if (me->since_hb > T_TO) {
                int was = me->peer_alive;
                me->peer_alive = 0;
                if (was || me->f.state == RDN_ST_STANDBY_HOT ||
                    me->f.state == RDN_ST_STANDBY_SYNC) {
                    sim_event(s, i, RDN_EV_PEER_TIMEOUT);
                }
            }
        }
        if (me->f.state == RDN_ST_DISCOVER) {
            if (++me->listen >= T_LISTEN && !me->peer_alive) {
                sim_event(s, i, RDN_EV_STARTUP_TIMEOUT);
            }
        }
        if (me->f.state == RDN_ST_SAFE) {
            if (++me->safe_rounds > 10) {
                me->safe_rounds = 0;
                sim_event(s, i, RDN_EV_MAINT_RESET);
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (s->n[i].up && s->n[i].f.state == RDN_ST_ACTIVE) {
            actives++;
        }
    }
    if (actives > 1 && s->use_arb) {
        s->max_dual++;
    }
}

static void sim_run(int use_arb, uint32_t seed, int *violations, int *converged)
{
    sim_t s;
    int r, i;

    memset(&s, 0, sizeof(s));
    s.use_arb = use_arb;
    s.rng = seed;
    s.connected = 1;
    for (i = 0; i < 2; i++) {
        rdn_fsm_init(&s.n[i].f, (uint8_t)(i + 1), (uint8_t)(2 - i), 0u, 0u);
        s.n[i].up = 1;
        s.n[i].since_hb = T_TO + 1;
        sim_event(&s, i, RDN_EV_START);
    }
    for (r = 0; r < 3000; r++) {
        sim_round(&s, 1);
    }
    /* repair everything, then let it settle */
    s.connected = 1;
    for (i = 0; i < 2; i++) {
        if (!s.n[i].up) {
            rdn_fsm_init(&s.n[i].f, s.n[i].f.node_id, s.n[i].f.peer_id, 0u, 0u);
            s.n[i].up = 1;
            s.n[i].peer_alive = 0;
            s.n[i].since_hb = T_TO + 1;
            sim_event(&s, i, RDN_EV_START);
        }
    }
    for (r = 0; r < 40; r++) {
        sim_round(&s, 0);
    }
    *violations += s.max_dual;
    {
        rdn_state_t a = s.n[0].f.state, b = s.n[1].f.state;
        int ok = ((a == RDN_ST_ACTIVE && b == RDN_ST_STANDBY_HOT) ||
                  (b == RDN_ST_ACTIVE && a == RDN_ST_STANDBY_HOT));
        if (!ok) {
            fprintf(stderr, "  seed %u arb %d settled in %s/%s\n", seed, use_arb,
                    rdn_state_name(a), rdn_state_name(b));
        }
        *converged += ok;
    }
}

static void test_simulation_with_arbiter(void)
{
    int v = 0, c = 0;
    uint32_t seed;
    for (seed = 1; seed <= 200; seed++) {
        sim_run(1, seed, &v, &c);
    }
    CHECK_EQ(v, 0);         /* never two ACTIVE nodes */
    CHECK_EQ(c, 200);       /* always converges */
}

static void test_simulation_without_arbiter(void)
{
    int v = 0, c = 0;
    uint32_t seed;
    for (seed = 1000; seed <= 1200; seed++) {
        sim_run(0, seed, &v, &c);
    }
    CHECK_EQ(c, 201);       /* split brain always resolved after healing */
}

int main(void)
{
    printf("test_fsm\n");
    RUN(test_transition_table);
    RUN(test_epoch_rules);
    RUN(test_tie_break);
    RUN(test_exhaustive_invariants);
    RUN(test_simulation_with_arbiter);
    RUN(test_simulation_without_arbiter);
    return test_report("test_fsm");
}
