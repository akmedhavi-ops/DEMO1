/*
 * rdn_core.c - redundancy runtime
 *
 * Tasks per node:
 *   tRdnMon  - periodic monitor (T_mon): liveness, state machine, heartbeats.
 *              The ONLY context that steps the state machine after start.
 *   tRdnRxN  - one receiver per link: validates frames, updates the peer
 *              view, applies replication on the standby, wakes tRdnMon
 *              immediately for urgent events (relinquish, state changes).
 *   (caller) - rdn_commit() runs in the application task.
 *
 * Lock order (never acquire leftwards while holding rightwards):
 *   commit_lock -> lock -> reg_lock -> link[i].tx_lock
 */
#include <string.h>
#include "rdn.h"
#include "rdn_fsm.h"
#include "rdn_osal.h"
#include "rdn_proto.h"
#include "rdn_repl.h"

#define MAX_EVENTS_PER_CYCLE 12u
#define MAX_CHAINED_STEPS    4u   /* e.g. STANDBY_HOT->TAKEOVER->ACTIVE->SAFE */

typedef struct rdn_link_rt {
    rdn_link_t        link;
    struct rdn_node  *node;
    uint32_t          idx;
    rdn_mutex_t      *tx_lock;
    rdn_task_t       *rx_task;
    uint32_t          tx_seq;
    rdn_seq_rx_t      rx_seq;
    uint64_t          last_rx_us;
    uint8_t           heard;
    rdn_link_stats_t  st;
    uint8_t           txbuf[RDN_MAX_FRAME];
    uint8_t           rxbuf[RDN_MAX_FRAME];
} rdn_link_rt_t;

struct rdn_node {
    rdn_config_t     cfg;
    rdn_callbacks_t  cb;
    rdn_arbiter_t    arb;
    uint8_t          has_arb;

    rdn_mutex_t     *commit_lock;
    rdn_mutex_t     *lock;
    rdn_mutex_t     *reg_lock;
    rdn_sem_t       *wake;
    rdn_sem_t       *ack_sem;

    rdn_link_rt_t   *link[RDN_MAX_LINKS];
    uint32_t         nlinks;

    rdn_fsm_t        fsm;
    rdn_repl_t       repl;
    uint8_t         *repl_mem[RDN_MAX_REGIONS];

    uint32_t         incarnation;
    uint32_t         clock_res_us;
    uint32_t         t_to_us;
    uint64_t         start_us;

    /* peer view (from heartbeats) */
    rdn_fsm_peer_t   peer;
    uint64_t         peer_last_rx_us;
    uint32_t         peer_applied_txn;
    uint8_t          peer_hb_new;

    /* pending events, consumed by the monitor */
    uint8_t          ev_fault;
    uint8_t          ev_switchover;
    uint8_t          ev_maint;
    uint8_t          ev_relinquish;
    uint8_t          ev_sync_complete;
    uint8_t          ev_sync_lost;

    /* active-side replication */
    uint8_t          full_pending;
    uint32_t         acked_txn;

    uint64_t         discover_start_us;
    uint64_t         next_hb_us;
    uint64_t         last_sync_req_us;
    uint64_t         last_monitor_us;
    uint64_t         detect_at_us;

    uint32_t         running;   /* RDN_ATOMIC_* only */
    uint32_t         frozen;    /* RDN_ATOMIC_* only */
    uint8_t          outputs_enabled;
    rdn_task_t      *mon_task;
    rdn_stats_t      st;
};

/* ------------------------------------------------------------------------ */

static uint32_t elapsed_u32(uint64_t now, uint64_t then)
{
    uint64_t d = (now > then) ? (now - then) : 0u;
    return (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
}

static void fill_hdr(rdn_node_t *n, rdn_msg_hdr_t *h, uint8_t type)
{
    (void)memset(h, 0, sizeof(*h));
    h->type        = type;
    h->src         = n->cfg.node_id;
    h->dst         = n->cfg.peer_id;
    h->cluster     = n->cfg.cluster_id;
    h->incarnation = n->incarnation;
}

/* Encode and transmit one frame on one link. Sets h->seq. */
static int link_send(rdn_link_rt_t *l, rdn_msg_hdr_t *h, const uint8_t *pl)
{
    int len;
    int rc;

    rdn_mutex_lock(l->tx_lock);
    l->tx_seq++;
    if (l->tx_seq == 0u) {
        l->tx_seq = 1u;
    }
    h->seq = l->tx_seq;
    len = rdn_proto_encode(h, pl, l->txbuf, RDN_MAX_FRAME);
    if (len < 0) {
        rc = len;
    } else {
        rc = l->link.ops->send(l->link.ctx, l->txbuf, (uint32_t)len);
    }
    if (rc < 0) {
        l->st.tx_errors++;
    } else {
        l->st.tx_frames++;
    }
    rdn_mutex_unlock(l->tx_lock);
    return rc;
}

static int send_all(rdn_node_t *n, rdn_msg_hdr_t *h, const uint8_t *pl)
{
    uint32_t i;
    int ok = 0;

    for (i = 0u; i < n->nlinks; i++) {
        if (link_send(n->link[i], h, pl) >= 0) {
            ok = 1;
        }
    }
    return ok ? RDN_OK : RDN_E_IO;
}

/* Replication traffic uses ONE link so fragments stay in order: the first
 * link on which the peer is currently heard. Caller holds n->lock. */
static uint32_t pick_repl_link(const rdn_node_t *n)
{
    uint32_t i;
    for (i = 0u; i < n->nlinks; i++) {
        if (n->link[i]->st.up) {
            return i;
        }
    }
    return 0u;
}

static void send_heartbeat(rdn_node_t *n, uint64_t now)
{
    rdn_msg_hdr_t h;
    rdn_hb_t      hb;
    uint8_t       pl[RDN_HB_LEN];
    uint32_t      i;

    (void)memset(&hb, 0, sizeof(hb));
    fill_hdr(n, &h, (uint8_t)RDN_MSG_HB);
    rdn_mutex_lock(n->lock);
    h.epoch      = n->fsm.epoch;
    hb.state     = (uint8_t)n->fsm.state;
    hb.health    = (n->fsm.state == RDN_ST_SAFE) ? 1u : 0u;
    hb.applied_txn = (n->fsm.state == RDN_ST_ACTIVE) ? n->st.last_txn
                                                      : n->repl.applied_txn;
    hb.uptime_ms  = elapsed_u32(now, n->start_us) / 1000u;
    hb.layout_sig = n->repl.layout_sig;
    for (i = 0u; i < n->nlinks; i++) {
        if (n->link[i]->st.up) {
            hb.link_mask |= (uint8_t)(1u << i);
        }
    }
    rdn_mutex_unlock(n->lock);
    rdn_hb_encode(&hb, pl);
    h.len = RDN_HB_LEN;
    (void)send_all(n, &h, pl);
}

static void send_simple(rdn_node_t *n, uint8_t type, uint32_t txn, int all)
{
    rdn_msg_hdr_t h;
    uint32_t li;

    fill_hdr(n, &h, type);
    rdn_mutex_lock(n->lock);
    h.epoch = n->fsm.epoch;
    li = pick_repl_link(n);
    rdn_mutex_unlock(n->lock);
    h.txn = txn;
    if (all) {
        (void)send_all(n, &h, NULL);
    } else {
        (void)link_send(n->link[li], &h, NULL);
    }
}

/* ---- state machine dispatch (monitor context) --------------------------- */

static void dispatch(rdn_node_t *n, rdn_event_t first)
{
    rdn_event_t ev = first;
    uint32_t    k;

    for (k = 0u; k < MAX_CHAINED_STEPS; k++) {
        rdn_state_t from, to;
        uint32_t    acts, epoch, last_txn;
        uint8_t     cold;
        int         have_next = 0;
        rdn_event_t next = RDN_EV_COUNT;
        uint64_t    now = rdn_time_us();

        rdn_mutex_lock(n->commit_lock);   /* no commit in flight */
        rdn_mutex_lock(n->lock);
        from  = n->fsm.state;
        acts  = rdn_fsm_step(&n->fsm, ev, &n->peer);
        to    = n->fsm.state;
        epoch = n->fsm.epoch;
        cold  = n->fsm.cold;
        if ((acts & RDN_ACT_DEACTIVATE) != 0u) {
            n->outputs_enabled = 0u;      /* inhibit before anything else */
        }
        if ((to == RDN_ST_STANDBY_SYNC) && (from != RDN_ST_STANDBY_SYNC)) {
            rdn_mutex_lock(n->reg_lock);
            rdn_repl_reset_standby(&n->repl);
            rdn_mutex_unlock(n->reg_lock);
        }
        if ((acts & RDN_ACT_START_LISTEN) != 0u) {
            n->discover_start_us = now;
        }
        if ((to == RDN_ST_ACTIVE) && (from != RDN_ST_ACTIVE)) {
            /* continue the transaction numbering of the replica */
            n->repl.txn     = cold ? 0u : n->repl.applied_txn;
            n->acked_txn    = n->repl.txn;
            n->st.last_txn  = n->repl.txn;
            n->full_pending = 0u;
        }
        if ((acts & RDN_ACT_SPLIT_BRAIN) != 0u) {
            if (to != RDN_ST_ACTIVE) {
                n->st.split_brain_resolved++;
            }
        }
        if (from != to) {
            n->st.transitions++;
        }
        last_txn = n->repl.applied_txn;
        rdn_mutex_unlock(n->lock);

        if (from != to) {
            rdn_log(RDN_LOG_INFO, "rdn[%u]: %s --%s--> %s (epoch %u)\n",
                    (unsigned)n->cfg.node_id, rdn_state_name(from),
                    rdn_event_name(ev), rdn_state_name(to), (unsigned)epoch);
        }

        /* actions, in the fixed order defined in rdn_fsm.h */
        if (((acts & RDN_ACT_DEACTIVATE) != 0u) && (n->cb.on_deactivate != NULL)) {
            n->cb.on_deactivate(n->cb.user);
        }
        if (((acts & RDN_ACT_RELEASE) != 0u) && n->has_arb &&
            (n->arb.release != NULL)) {
            n->arb.release(n->arb.ctx, n->cfg.node_id);
        }
        if ((acts & RDN_ACT_SEND_RELINQUISH) != 0u) {
            /* same link as replication data: FIFO order guarantees the
             * standby has seen every transaction sent before this */
            send_simple(n, (uint8_t)RDN_MSG_RELINQUISH, n->repl.txn, 0);
        }
        if ((acts & RDN_ACT_SEND_SYNC_REQ) != 0u) {
            send_simple(n, (uint8_t)RDN_MSG_SYNC_REQ, 0u, 1);
            n->last_sync_req_us = now;
        }
        if ((acts & RDN_ACT_CLAIM) != 0u) {
            int granted = 1;
            if (n->has_arb && (n->arb.claim != NULL)) {
                granted = n->arb.claim(n->arb.ctx, n->cfg.node_id, epoch + 1u);
            }
            next = granted ? RDN_EV_CLAIM_GRANTED : RDN_EV_CLAIM_DENIED;
            have_next = 1;
            if (!granted) {
                rdn_log(RDN_LOG_WARN, "rdn[%u]: arbiter denied activation\n",
                        (unsigned)n->cfg.node_id);
            }
        }
        if ((acts & RDN_ACT_ACTIVATE) != 0u) {
            int rc = 0;
            if (n->cb.on_activate != NULL) {
                rc = n->cb.on_activate(n->cb.user, (int)cold, last_txn);
            }
            if (rc != 0) {
                rdn_log(RDN_LOG_ERR, "rdn[%u]: on_activate failed (%d)\n",
                        (unsigned)n->cfg.node_id, rc);
                next = RDN_EV_LOCAL_FAULT;
                have_next = 1;
            } else {
                uint64_t t = rdn_time_us();
                rdn_mutex_lock(n->lock);
                n->outputs_enabled = 1u;
                n->st.takeovers++;
                if (n->detect_at_us != 0u) {
                    n->st.last_takeover_us = elapsed_u32(t, n->detect_at_us);
                    if (n->st.last_takeover_us > n->st.max_takeover_us) {
                        n->st.max_takeover_us = n->st.last_takeover_us;
                    }
                    n->detect_at_us = 0u;
                }
                rdn_mutex_unlock(n->lock);
            }
        }
        if ((acts & RDN_ACT_NOTIFY_PEER_LOST) != 0u) {
            rdn_log(RDN_LOG_WARN, "rdn[%u]: peer lost, running without standby\n",
                    (unsigned)n->cfg.node_id);
        }
        if ((acts & RDN_ACT_REJECT) != 0u) {
            rdn_log(RDN_LOG_WARN, "rdn[%u]: %s rejected in %s\n",
                    (unsigned)n->cfg.node_id, rdn_event_name(ev),
                    rdn_state_name(to));
        }
        rdn_mutex_unlock(n->commit_lock);

        if (from != to) {
            if (RDN_ATOMIC_LOAD(&n->running)) {
                send_heartbeat(n, rdn_time_us()); /* tell peer immediately */
            }
            if (n->cb.on_state_change != NULL) {
                n->cb.on_state_change(n->cb.user, from, to, epoch);
            }
        }
        if (!have_next) {
            break;
        }
        ev = next;
    }
}

/* ---- monitor task -------------------------------------------------------- */

static void monitor_cycle(rdn_node_t *n, uint64_t now, int periodic)
{
    rdn_event_t evs[MAX_EVENTS_PER_CYCLE];
    uint32_t    nev = 0u;
    uint32_t    i;
    int         health_fault = 0;
    int         alive_now = 0;
    int         resync = 0;

    if (periodic && (n->cb.health_check != NULL)) {
        health_fault = (n->cb.health_check(n->cb.user) != 0) ? 1 : 0;
    }
    /* keep the arbiter lease; losing it means another node may be active */
    if (n->has_arb && (n->arb.renew != NULL) &&
        (rdn_state(n) == RDN_ST_ACTIVE)) {
        if (!n->arb.renew(n->arb.ctx, n->cfg.node_id)) {
            rdn_log(RDN_LOG_ERR, "rdn[%u]: arbiter lease lost\n",
                    (unsigned)n->cfg.node_id);
            rdn_mutex_lock(n->lock);
            n->outputs_enabled = 0u;
            rdn_mutex_unlock(n->lock);
            health_fault = 1;
        }
    }

    rdn_mutex_lock(n->lock);
    n->last_monitor_us = now;

    if (n->ev_maint)  { evs[nev++] = RDN_EV_MAINT_RESET; }
    if (n->ev_fault || health_fault) { evs[nev++] = RDN_EV_LOCAL_FAULT; }
    if (n->ev_relinquish) { evs[nev++] = RDN_EV_PEER_RELINQUISH; }

    for (i = 0u; i < n->nlinks; i++) {
        rdn_link_rt_t *l = n->link[i];
        int up = l->heard && (elapsed_u32(now, l->last_rx_us) <= n->t_to_us);
        if (l->st.up && !up) {
            l->rx_seq.valid = 0u;   /* allow peer re-incarnation */
            rdn_log(RDN_LOG_WARN, "rdn[%u]: link %u (%s) down\n",
                    (unsigned)n->cfg.node_id, (unsigned)i, l->link.ops->name);
        }
        l->st.up = (uint8_t)up;
        if (up) {
            alive_now = 1;
        }
    }
    if (n->peer.alive && !alive_now) {
        uint32_t silence = elapsed_u32(now, n->peer_last_rx_us);
        n->st.last_detect_us = silence;
        if (silence > n->st.max_detect_us) {
            n->st.max_detect_us = silence;
        }
        n->detect_at_us = now;
        evs[nev++] = RDN_EV_PEER_TIMEOUT;
    } else if (!alive_now && ((n->fsm.state == RDN_ST_STANDBY_HOT) ||
                              (n->fsm.state == RDN_ST_STANDBY_SYNC))) {
        /* peer still silent after a denied claim: retry every cycle */
        evs[nev++] = RDN_EV_PEER_TIMEOUT;
    } else {
        /* no liveness event */
    }
    n->peer.alive = (uint8_t)alive_now;
    if (n->peer_hb_new && alive_now) {
        evs[nev++] = RDN_EV_PEER_HB;
    }
    if (n->ev_sync_complete) { evs[nev++] = RDN_EV_SYNC_COMPLETE; }
    if (n->ev_sync_lost)     { evs[nev++] = RDN_EV_SYNC_LOST; }
    if (n->ev_switchover)    { evs[nev++] = RDN_EV_SWITCHOVER_REQ; }
    if ((n->fsm.state == RDN_ST_DISCOVER) && !alive_now &&
        (elapsed_u32(now, n->discover_start_us) >= n->cfg.startup_listen_us)) {
        evs[nev++] = RDN_EV_STARTUP_TIMEOUT;
    }
    n->ev_maint = n->ev_fault = n->ev_relinquish = 0u;
    n->ev_sync_complete = n->ev_sync_lost = n->ev_switchover = 0u;
    n->peer_hb_new = 0u;
    rdn_mutex_unlock(n->lock);

    for (i = 0u; i < nev; i++) {
        dispatch(n, evs[i]);
    }

    rdn_mutex_lock(n->lock);
    if ((n->fsm.state == RDN_ST_STANDBY_SYNC) && alive_now &&
        (elapsed_u32(now, n->last_sync_req_us) >= n->cfg.sync_retry_us)) {
        n->last_sync_req_us = now;
        resync = 1;
    }
    rdn_mutex_unlock(n->lock);

    if (resync) {
        send_simple(n, (uint8_t)RDN_MSG_SYNC_REQ, 0u, 1);
    }
    if (now >= n->next_hb_us) {
        send_heartbeat(n, now);
        n->next_hb_us += n->cfg.hb_period_us;
        if (n->next_hb_us <= now) {
            n->next_hb_us = now + n->cfg.hb_period_us;
        }
    }
}

static void monitor_task(void *arg)
{
    rdn_node_t *n = (rdn_node_t *)arg;
    uint64_t next = rdn_time_us();

    n->next_hb_us = next;
    while (RDN_ATOMIC_LOAD(&n->running)) {
        uint64_t now = rdn_time_us();
        int periodic = 0;

        if (now < next) {
            (void)rdn_sem_take(n->wake, elapsed_u32(next, now));
            now = rdn_time_us();
        }
        if (!RDN_ATOMIC_LOAD(&n->running)) {
            break;
        }
        if (now >= next) {
            periodic = 1;
            next += n->cfg.monitor_period_us;
            if (next <= now) {
                n->st.monitor_overruns++;
                next = now + n->cfg.monitor_period_us;
            }
        }
        if (RDN_ATOMIC_LOAD(&n->frozen)) {
            continue;
        }
        monitor_cycle(n, now, periodic);
    }
}

/* ---- receive tasks -------------------------------------------------------- */

static void handle_frame(rdn_node_t *n, rdn_link_rt_t *l, const rdn_msg_hdr_t *h,
                         const uint8_t *pl, uint64_t now)
{
    int      wake = 0;
    int      send_ack = 0;
    uint32_t ack_txn = 0u;
    uint32_t lost = 0u;

    rdn_mutex_lock(n->lock);
    if (!rdn_seq_accept(&l->rx_seq, h->incarnation, h->seq, &lost)) {
        l->st.rx_replayed++;
        rdn_mutex_unlock(n->lock);
        return;
    }
    l->st.rx_lost += lost;
    l->st.rx_frames++;
    l->last_rx_us = now;
    l->heard = 1u;
    n->peer_last_rx_us = now;

    switch ((rdn_msg_type_t)h->type) {
    case RDN_MSG_HB:
        if (h->len == RDN_HB_LEN) {
            rdn_hb_t hb;
            rdn_hb_decode(pl, &hb);
            if ((hb.state < (uint8_t)RDN_ST_COUNT) &&
                (((rdn_state_t)hb.state != n->peer.state) ||
                 (h->epoch != n->peer.epoch) || !n->peer.alive)) {
                wake = 1;   /* react to peer state changes without delay */
            }
            if (hb.state < (uint8_t)RDN_ST_COUNT) {
                n->peer.state = (rdn_state_t)hb.state;
            }
            n->peer.epoch       = h->epoch;
            n->peer_applied_txn = hb.applied_txn;
            n->peer_hb_new      = 1u;
        } else {
            l->st.rx_bad_hdr++;
        }
        break;
    case RDN_MSG_REPL_DATA:
    case RDN_MSG_REPL_COMMIT:
        if ((n->fsm.state == RDN_ST_STANDBY_SYNC) ||
            (n->fsm.state == RDN_ST_STANDBY_HOT)) {
            int rc;
            if (h->type == (uint8_t)RDN_MSG_REPL_DATA) {
                rc = rdn_repl_rx_data(&n->repl, h, pl);
            } else {
                rdn_mutex_lock(n->reg_lock);
                rc = rdn_repl_rx_commit(&n->repl, h, pl);
                rdn_mutex_unlock(n->reg_lock);
            }
            switch (rc) {
            case RDN_REPL_RX_SYNCED:
                n->st.sync_full++;
                n->st.txn_applied++;
                n->ev_sync_complete = 1u;
                wake = 1;
                send_ack = 1;
                break;
            case RDN_REPL_RX_APPLIED:
                n->st.txn_applied++;
                send_ack = 1;
                break;
            case RDN_REPL_RX_LOST:
                n->st.sync_lost++;
                n->ev_sync_lost = 1u;
                wake = 1;
                break;
            case RDN_REPL_RX_LAYOUT:
                rdn_log(RDN_LOG_ERR, "rdn[%u]: region layout differs from peer\n",
                        (unsigned)n->cfg.node_id);
                n->ev_fault = 1u;
                wake = 1;
                break;
            default:
                break;
            }
            ack_txn = n->repl.applied_txn;
        }
        break;
    case RDN_MSG_ACK:
        if (n->fsm.state == RDN_ST_ACTIVE) {
            if ((int32_t)(h->txn - n->acked_txn) > 0) {
                n->acked_txn = h->txn;
            }
            rdn_sem_give(n->ack_sem);
        }
        break;
    case RDN_MSG_SYNC_REQ:
        if (n->fsm.state == RDN_ST_ACTIVE) {
            n->full_pending = 1u;
        }
        break;
    case RDN_MSG_RELINQUISH:
        n->ev_relinquish = 1u;
        n->detect_at_us  = now;
        wake = 1;
        break;
    default:
        l->st.rx_bad_hdr++;
        break;
    }
    rdn_mutex_unlock(n->lock);

    if (send_ack) {
        send_simple(n, (uint8_t)RDN_MSG_ACK, ack_txn, 0);
    }
    if (wake) {
        rdn_sem_give(n->wake);
    }
}

static void rx_task(void *arg)
{
    rdn_link_rt_t *l = (rdn_link_rt_t *)arg;
    rdn_node_t    *n = l->node;

    while (RDN_ATOMIC_LOAD(&n->running)) {
        rdn_msg_hdr_t  h;
        const uint8_t *pl = NULL;
        int len = l->link.ops->recv(l->link.ctx, l->rxbuf, RDN_MAX_FRAME,
                                    n->cfg.hb_period_us);
        int rc;

        if (len < 0) {
            rdn_sleep_us(n->cfg.hb_period_us);   /* link error: no spin */
            continue;
        }
        if ((len == 0) || RDN_ATOMIC_LOAD(&n->frozen)) {
            continue;
        }
        rc = rdn_proto_decode(l->rxbuf, (uint32_t)len, n->cfg.cluster_id,
                              n->cfg.peer_id, n->cfg.node_id, &h, &pl);
        if (rc != RDN_OK) {
            rdn_mutex_lock(n->lock);
            if (rc == RDN_E_CRC) {
                l->st.rx_bad_crc++;
            } else {
                l->st.rx_bad_hdr++;
            }
            rdn_mutex_unlock(n->lock);
            continue;
        }
        handle_frame(n, l, &h, pl, rdn_time_us());
    }
}

/* ---- commit (application task) ------------------------------------------ */

typedef struct emit_ctx {
    rdn_node_t    *n;
    rdn_link_rt_t *l;
    uint32_t       epoch;
} emit_ctx_t;

static int emit_frame(void *ctx, const rdn_msg_hdr_t *hin, const uint8_t *pl)
{
    emit_ctx_t   *e = (emit_ctx_t *)ctx;
    rdn_msg_hdr_t h = *hin;

    h.src         = e->n->cfg.node_id;
    h.dst         = e->n->cfg.peer_id;
    h.cluster     = e->n->cfg.cluster_id;
    h.incarnation = e->n->incarnation;
    h.epoch       = e->epoch;
    return link_send(e->l, &h, pl);
}

/* Replicate one transaction. Called with commit_lock held; releases it
 * before (optionally) waiting for the standby's acknowledgement. */
static int commit_locked(rdn_node_t *n)
{
    emit_ctx_t ec;
    uint32_t   txn = 0u;
    int        full, standby, hot;
    int        rc;

    rdn_mutex_lock(n->lock);
    if ((!RDN_ATOMIC_LOAD(&n->running)) || RDN_ATOMIC_LOAD(&n->frozen) ||
        (n->fsm.state != RDN_ST_ACTIVE) ||
        (!n->outputs_enabled)) {
        rdn_mutex_unlock(n->lock);
        rdn_mutex_unlock(n->commit_lock);
        return RDN_E_STATE;
    }
    full    = n->full_pending;
    standby = n->peer.alive && ((n->peer.state == RDN_ST_STANDBY_SYNC) ||
                                (n->peer.state == RDN_ST_STANDBY_HOT));
    hot     = n->peer.alive && (n->peer.state == RDN_ST_STANDBY_HOT);
    ec.n     = n;
    ec.l     = n->link[pick_repl_link(n)];
    ec.epoch = n->fsm.epoch;
    if (standby) {
        n->full_pending = 0u;
    }
    rdn_mutex_unlock(n->lock);

    if (!standby) {
        rdn_mutex_unlock(n->commit_lock);
        return RDN_E_NOPEER;   /* committed locally only (degraded) */
    }
    rc = rdn_repl_build(&n->repl, full, RDN_MAX_PAYLOAD, emit_frame, &ec, &txn);
    rdn_mutex_lock(n->lock);
    n->st.txn_committed++;
    n->st.last_txn = txn;
    if (full) {
        n->st.sync_full++;
    }
    rdn_mutex_unlock(n->lock);
    rdn_mutex_unlock(n->commit_lock);
    if (rc < 0) {
        return rc;
    }

    if ((n->cfg.repl_mode == RDN_REPL_SYNC) && (hot || full)) {
        uint64_t deadline = rdn_time_us() + n->cfg.ack_timeout_us;
        for (;;) {
            uint64_t now;
            int done;
            rdn_mutex_lock(n->lock);
            done = ((int32_t)(n->acked_txn - txn) >= 0);
            rdn_mutex_unlock(n->lock);
            if (done) {
                return RDN_OK;
            }
            now = rdn_time_us();
            if (now >= deadline) {
                rdn_mutex_lock(n->lock);
                n->st.ack_timeouts++;
                rdn_mutex_unlock(n->lock);
                return RDN_E_TIMEOUT;
            }
            (void)rdn_sem_take(n->ack_sem, elapsed_u32(deadline, now));
        }
    }
    return hot ? RDN_OK : RDN_E_NOPEER;
}

int rdn_commit(rdn_node_t *n)
{
    if (n == NULL) {
        return RDN_E_PARAM;
    }
    rdn_mutex_lock(n->commit_lock);
    return commit_locked(n);
}

/* The cycle bracket holds commit_lock from begin to end: dispatch() takes
 * the same lock, so a role change (demotion, switchover, replica reset)
 * can never interleave with an application cycle that is writing the
 * replicated regions. Safety inhibition (fault, lease loss, freeze) does
 * NOT wait for it: it clears outputs_enabled directly. */
int rdn_cycle_begin(rdn_node_t *n)
{
    if (n == NULL) {
        return 0;
    }
    rdn_mutex_lock(n->commit_lock);
    if (!rdn_output_permitted(n)) {
        rdn_mutex_unlock(n->commit_lock);
        return 0;
    }
    return 1;
}

int rdn_cycle_end(rdn_node_t *n)
{
    if (n == NULL) {
        return RDN_E_PARAM;
    }
    return commit_locked(n);
}

/* ---- lifecycle ------------------------------------------------------------ */

int rdn_create(const rdn_config_t *cfg, const rdn_callbacks_t *cb,
               rdn_node_t **out)
{
    rdn_node_t *n;
    int rc;

    if ((out == NULL) || (cfg == NULL)) {
        return RDN_E_PARAM;
    }
    *out = NULL;
    rc = rdn_config_validate(cfg);
    if (rc != RDN_OK) {
        return rc;
    }
    n = (rdn_node_t *)rdn_osal_alloc(sizeof(*n));
    if (n == NULL) {
        return RDN_E_NOMEM;
    }
    n->cfg = *cfg;
    if (cb != NULL) {
        n->cb = *cb;
    }
    n->commit_lock = rdn_mutex_create();
    n->lock        = rdn_mutex_create();
    n->reg_lock    = rdn_mutex_create();
    n->wake        = rdn_sem_create();
    n->ack_sem     = rdn_sem_create();
    if ((n->commit_lock == NULL) || (n->lock == NULL) ||
        (n->reg_lock == NULL) || (n->wake == NULL) || (n->ack_sem == NULL)) {
        rdn_destroy(n);
        return RDN_E_NOMEM;
    }
    n->clock_res_us = rdn_time_resolution_us();
    n->t_to_us = cfg->hb_miss_limit * cfg->hb_period_us;
    /* timeout must survive clock quantisation plus one lost heartbeat */
    if ((n->t_to_us - n->clock_res_us) <
        (2u * cfg->hb_period_us + cfg->sched_jitter_us)) {
        rdn_log(RDN_LOG_ERR, "rdn: T_to too small for clock resolution %u us\n",
                (unsigned)n->clock_res_us);
        rdn_destroy(n);
        return RDN_E_PARAM;
    }
    rdn_fsm_init(&n->fsm, cfg->node_id, cfg->peer_id, cfg->preferred_active,
                 cfg->allow_cold_takeover);
    rdn_repl_init(&n->repl, cfg->block_size);
    n->peer.state = RDN_ST_INIT;
    *out = n;
    return RDN_OK;
}

int rdn_add_link(rdn_node_t *n, const rdn_link_t *link)
{
    rdn_link_rt_t *l;

    if ((n == NULL) || (link == NULL) || (link->ops == NULL) ||
        (link->ops->send == NULL) || (link->ops->recv == NULL)) {
        return RDN_E_PARAM;
    }
    if (RDN_ATOMIC_LOAD(&n->running)) {
        return RDN_E_STATE;
    }
    if (n->nlinks >= RDN_MAX_LINKS) {
        return RDN_E_FULL;
    }
    l = (rdn_link_rt_t *)rdn_osal_alloc(sizeof(*l));
    if (l == NULL) {
        return RDN_E_NOMEM;
    }
    l->tx_lock = rdn_mutex_create();
    if (l->tx_lock == NULL) {
        rdn_osal_free(l);
        return RDN_E_NOMEM;
    }
    l->link = *link;
    l->node = n;
    l->idx  = n->nlinks;
    n->link[n->nlinks++] = l;
    return RDN_OK;
}

int rdn_set_arbiter(rdn_node_t *n, const rdn_arbiter_t *arb)
{
    if ((n == NULL) || (arb == NULL) || (arb->claim == NULL)) {
        return RDN_E_PARAM;
    }
    if (RDN_ATOMIC_LOAD(&n->running)) {
        return RDN_E_STATE;
    }
    n->arb = *arb;
    n->has_arb = 1u;
    return RDN_OK;
}

int rdn_region_register(rdn_node_t *n, uint16_t id, void *buf, uint32_t size)
{
    uint8_t *mem;
    int rc;

    if ((n == NULL) || (buf == NULL) || (size == 0u)) {
        return RDN_E_PARAM;
    }
    if (RDN_ATOMIC_LOAD(&n->running)) {
        return RDN_E_STATE;
    }
    if (n->repl.nreg >= RDN_MAX_REGIONS) {
        return RDN_E_FULL;
    }
    mem = (uint8_t *)rdn_osal_alloc((size_t)size * 2u);   /* shadow + work */
    if (mem == NULL) {
        return RDN_E_NOMEM;
    }
    rc = rdn_repl_add(&n->repl, id, buf, size, mem, mem + size);
    if (rc != RDN_OK) {
        rdn_osal_free(mem);
        return rc;
    }
    n->repl_mem[n->repl.nreg - 1u] = mem;
    return RDN_OK;
}

int rdn_start(rdn_node_t *n)
{
    uint32_t i;
    char name[16];

    if (n == NULL) {
        return RDN_E_PARAM;
    }
    if (RDN_ATOMIC_LOAD(&n->running) || (n->fsm.state != RDN_ST_INIT)) {
        return RDN_E_STATE;
    }
    if (n->nlinks == 0u) {
        return RDN_E_PARAM;
    }
    n->start_us = rdn_time_us();
    n->incarnation = rdn_osal_entropy() ^ (uint32_t)n->start_us ^
                     ((uint32_t)n->cfg.node_id << 24);
    if (n->incarnation == 0u) {
        n->incarnation = 1u;
    }
    n->last_monitor_us = n->start_us;
    RDN_ATOMIC_STORE(&n->frozen, 0u);
    RDN_ATOMIC_STORE(&n->running, 1u);
    dispatch(n, RDN_EV_START);

    for (i = 0u; i < n->nlinks; i++) {
        name[0] = 't'; name[1] = 'R'; name[2] = 'd'; name[3] = 'n';
        name[4] = 'R'; name[5] = 'x'; name[6] = (char)('0' + (char)i);
        name[7] = (char)('0' + (char)(n->cfg.node_id % 10u));
        name[8] = '\0';
        n->link[i]->rx_task = rdn_task_spawn(name, n->cfg.rx_prio,
                                             n->cfg.stack_size, rx_task,
                                             n->link[i]);
        if (n->link[i]->rx_task == NULL) {
            (void)rdn_stop(n);
            return RDN_E_NOMEM;
        }
    }
    n->mon_task = rdn_task_spawn("tRdnMon", n->cfg.monitor_prio,
                                 n->cfg.stack_size, monitor_task, n);
    if (n->mon_task == NULL) {
        (void)rdn_stop(n);
        return RDN_E_NOMEM;
    }
    return RDN_OK;
}

int rdn_stop(rdn_node_t *n)
{
    uint32_t i;

    if (n == NULL) {
        return RDN_E_PARAM;
    }
    if (!RDN_ATOMIC_LOAD(&n->running)) {
        return RDN_E_STATE;
    }
    RDN_ATOMIC_STORE(&n->running, 0u);
    rdn_sem_give(n->wake);
    if (n->mon_task != NULL) {
        rdn_task_join(n->mon_task);
        n->mon_task = NULL;
    }
    if (!RDN_ATOMIC_LOAD(&n->frozen)) {
        dispatch(n, RDN_EV_STOP);   /* graceful: deactivate + relinquish */
    }
    for (i = 0u; i < n->nlinks; i++) {
        if (n->link[i]->rx_task != NULL) {
            rdn_task_join(n->link[i]->rx_task);
            n->link[i]->rx_task = NULL;
        }
    }
    rdn_sem_give(n->ack_sem);
    return RDN_OK;
}

void rdn_destroy(rdn_node_t *n)
{
    uint32_t i;

    if (n == NULL) {
        return;
    }
    if (RDN_ATOMIC_LOAD(&n->running)) {
        (void)rdn_stop(n);
    }
    for (i = 0u; i < n->nlinks; i++) {
        if (n->link[i]->link.ops->close != NULL) {
            n->link[i]->link.ops->close(n->link[i]->link.ctx);
        }
        rdn_mutex_destroy(n->link[i]->tx_lock);
        rdn_osal_free(n->link[i]);
    }
    for (i = 0u; i < RDN_MAX_REGIONS; i++) {
        if (n->repl_mem[i] != NULL) {
            rdn_osal_free(n->repl_mem[i]);
        }
    }
    if (n->commit_lock != NULL) { rdn_mutex_destroy(n->commit_lock); }
    if (n->lock != NULL)        { rdn_mutex_destroy(n->lock); }
    if (n->reg_lock != NULL)    { rdn_mutex_destroy(n->reg_lock); }
    if (n->wake != NULL)        { rdn_sem_destroy(n->wake); }
    if (n->ack_sem != NULL)     { rdn_sem_destroy(n->ack_sem); }
    rdn_osal_free(n);
}

/* ---- runtime queries / requests ----------------------------------------- */

rdn_state_t rdn_state(const rdn_node_t *n)
{
    rdn_state_t s;
    if (n == NULL) {
        return RDN_ST_SAFE;
    }
    rdn_mutex_lock(n->lock);
    s = n->fsm.state;
    rdn_mutex_unlock(n->lock);
    return s;
}

int rdn_output_permitted(const rdn_node_t *n)
{
    int ok;
    uint64_t now;

    if (n == NULL) {
        return 0;
    }
    now = rdn_time_us();
    rdn_mutex_lock(n->lock);
    /* const API, but the flags are read-modify-write atomics */
    ok = RDN_ATOMIC_LOAD((uint32_t *)(uintptr_t)&n->running) &&
         !RDN_ATOMIC_LOAD((uint32_t *)(uintptr_t)&n->frozen) &&
         n->outputs_enabled &&
         (n->fsm.state == RDN_ST_ACTIVE) &&
         /* monitor task liveness: starved monitor => no outputs */
         (elapsed_u32(now, n->last_monitor_us) <=
          (2u * n->cfg.monitor_period_us + n->cfg.sched_jitter_us +
           n->clock_res_us));
    rdn_mutex_unlock(n->lock);
    return ok;
}

static int post_flag(rdn_node_t *n, uint8_t *flag)
{
    if ((n == NULL) || !RDN_ATOMIC_LOAD(&n->running)) {
        return RDN_E_STATE;
    }
    rdn_mutex_lock(n->lock);
    *flag = 1u;
    rdn_mutex_unlock(n->lock);
    rdn_sem_give(n->wake);
    return RDN_OK;
}

int rdn_request_switchover(rdn_node_t *n)
{
    if ((n == NULL) || (rdn_state(n) != RDN_ST_ACTIVE)) {
        return RDN_E_STATE;
    }
    return post_flag(n, &n->ev_switchover);
}

int rdn_report_fault(rdn_node_t *n, uint32_t code)
{
    if (n == NULL) {
        return RDN_E_PARAM;
    }
    rdn_log(RDN_LOG_ERR, "rdn[%u]: local fault 0x%08x reported\n",
            (unsigned)n->cfg.node_id, (unsigned)code);
    if (RDN_ATOMIC_LOAD(&n->running)) {
        rdn_mutex_lock(n->lock);
        n->outputs_enabled = 0u;   /* inhibit now, don't wait for monitor */
        rdn_mutex_unlock(n->lock);
    }
    return post_flag(n, &n->ev_fault);
}

int rdn_maintenance_reset(rdn_node_t *n)
{
    if ((n == NULL) || (rdn_state(n) != RDN_ST_SAFE)) {
        return RDN_E_STATE;
    }
    return post_flag(n, &n->ev_maint);
}

void rdn_get_stats(rdn_node_t *n, rdn_stats_t *st)
{
    uint32_t i;

    if ((n == NULL) || (st == NULL)) {
        return;
    }
    rdn_mutex_lock(n->lock);
    *st = n->st;
    for (i = 0u; i < n->nlinks; i++) {
        rdn_mutex_lock(n->link[i]->tx_lock);   /* tx counters */
        st->link[i] = n->link[i]->st;
        rdn_mutex_unlock(n->link[i]->tx_lock);
    }
    st->epoch      = n->fsm.epoch;
    st->state      = n->fsm.state;
    st->peer_state = n->peer.state;
    st->peer_alive = n->peer.alive;
    if (n->fsm.state != RDN_ST_ACTIVE) {
        st->last_txn = n->repl.applied_txn;
    }
    rdn_mutex_unlock(n->lock);
}

void rdn_region_lock(rdn_node_t *n)
{
    rdn_mutex_lock(n->reg_lock);
}

void rdn_region_unlock(rdn_node_t *n)
{
    rdn_mutex_unlock(n->reg_lock);
}

void rdn_test_freeze(rdn_node_t *n)
{
    rdn_mutex_lock(n->lock);
    RDN_ATOMIC_STORE(&n->frozen, 1u);
    n->outputs_enabled = 0u;
    rdn_mutex_unlock(n->lock);
}
