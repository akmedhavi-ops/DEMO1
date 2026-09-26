/*
 * rdn_config.c - configuration defaults, validation, timing bound,
 *                name tables
 */
#include <string.h>
#include "rdn.h"

void rdn_config_defaults(rdn_config_t *c)
{
    (void)memset(c, 0, sizeof(*c));
    c->cluster_id          = 0x52444E00u;
    c->node_id             = 1u;
    c->peer_id             = 2u;
    c->preferred_active    = 0u;
    c->allow_cold_takeover = 0u;
    c->monitor_period_us   = 2000u;
    c->hb_period_us        = 10000u;
    c->hb_miss_limit       = 3u;
    c->startup_listen_us   = 200000u;
    c->sync_retry_us       = 50000u;
    c->link_latency_max_us = 1000u;
    c->sched_jitter_us     = 1000u;
    c->activate_budget_us  = 5000u;
    c->arbiter_budget_us   = 500u;
    c->repl_mode           = RDN_REPL_ASYNC;
    c->ack_timeout_us      = 20000u;
    c->block_size          = 64u;
    c->monitor_prio        = 50;
    c->rx_prio             = 48;
    c->stack_size          = 16384u;
}

static int is_pow2(uint32_t v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

int rdn_config_validate(const rdn_config_t *c)
{
    uint64_t t_to;

    if (c == NULL) {
        return RDN_E_PARAM;
    }
    if ((c->node_id == 0u) || (c->node_id == 0xFFu) ||
        (c->peer_id == 0u) || (c->peer_id == 0xFFu) ||
        (c->node_id == c->peer_id)) {
        return RDN_E_PARAM;
    }
    if ((c->preferred_active != 0u) && (c->preferred_active != c->node_id) &&
        (c->preferred_active != c->peer_id)) {
        return RDN_E_PARAM;
    }
    if ((c->monitor_period_us == 0u) || (c->hb_period_us == 0u) ||
        (c->monitor_period_us > c->hb_period_us) ||
        (c->hb_miss_limit < 2u) || (c->hb_miss_limit > 1000u) ||
        (c->sync_retry_us == 0u)) {
        return RDN_E_PARAM;
    }
    t_to = (uint64_t)c->hb_miss_limit * c->hb_period_us;
    /* must tolerate at least one lost heartbeat without false failover */
    if (t_to < ((uint64_t)2u * c->hb_period_us + c->sched_jitter_us +
                c->link_latency_max_us)) {
        return RDN_E_PARAM;
    }
    /* a live peer must be heard before a cold start is attempted */
    if ((uint64_t)c->startup_listen_us < t_to) {
        return RDN_E_PARAM;
    }
    if (t_to > 0xFFFFFFFFull) {
        return RDN_E_PARAM;
    }
    if (!is_pow2(c->block_size) || (c->block_size < 8u) ||
        (c->block_size > 1024u)) {
        return RDN_E_PARAM;
    }
    if ((c->repl_mode != RDN_REPL_ASYNC) && (c->repl_mode != RDN_REPL_SYNC)) {
        return RDN_E_PARAM;
    }
    if ((c->repl_mode == RDN_REPL_SYNC) && (c->ack_timeout_us == 0u)) {
        return RDN_E_PARAM;
    }
    if (c->stack_size < 4096u) {
        return RDN_E_PARAM;
    }
    return RDN_OK;
}

static uint32_t sat_add(uint32_t a, uint64_t b)
{
    uint64_t s = (uint64_t)a + b;
    return (s > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)s;
}

/*
 * Worst-case bounds; derivation in docs/TIMING.md.
 *
 *  q      clock resolution (one tick on VxWorks without timestamp driver)
 *  T_to = N * T_hb
 *
 *  detect     = L_max + T_to + T_mon + J + 2q
 *  takeover   = detect + T_arb + T_act
 *  switchover = L_max + J + q + T_arb + T_act
 *  min_false  = T_to - q  (shortest real silence that can trip a timeout)
 */
void rdn_timing_bound(const rdn_config_t *c, uint32_t q,
                      rdn_timing_bound_t *out)
{
    uint64_t t_to = (uint64_t)c->hb_miss_limit * c->hb_period_us;
    uint32_t v;

    v = sat_add(c->link_latency_max_us, t_to);
    v = sat_add(v, c->monitor_period_us);
    v = sat_add(v, c->sched_jitter_us);
    v = sat_add(v, 2ull * q);
    out->detect_us = v;

    v = sat_add(v, c->arbiter_budget_us);
    out->takeover_us = sat_add(v, c->activate_budget_us);

    v = sat_add(c->link_latency_max_us, c->sched_jitter_us);
    v = sat_add(v, q);
    v = sat_add(v, c->arbiter_budget_us);
    out->switchover_us = sat_add(v, c->activate_budget_us);

    out->min_false_us = (t_to > q) ? (uint32_t)(t_to - q) : 0u;
}

const char *rdn_state_name(rdn_state_t s)
{
    static const char *const names[RDN_ST_COUNT] = {
        "INIT", "DISCOVER", "STANDBY_SYNC", "STANDBY_HOT",
        "TAKEOVER", "ACTIVE", "SAFE"
    };
    return ((unsigned)s < (unsigned)RDN_ST_COUNT) ? names[s] : "?";
}

const char *rdn_status_name(int st)
{
    switch (st) {
    case RDN_OK:        return "OK";
    case RDN_E_PARAM:   return "E_PARAM";
    case RDN_E_STATE:   return "E_STATE";
    case RDN_E_NOMEM:   return "E_NOMEM";
    case RDN_E_TIMEOUT: return "E_TIMEOUT";
    case RDN_E_IO:      return "E_IO";
    case RDN_E_NOPEER:  return "E_NOPEER";
    case RDN_E_CRC:     return "E_CRC";
    case RDN_E_FULL:    return "E_FULL";
    case RDN_E_PROTO:   return "E_PROTO";
    case RDN_E_CONFIG:  return "E_CONFIG";
    default:            return "E_?";
    }
}
