/*
 * test_system.c - two complete nodes (real runtime tasks) in one process
 *
 * Measures failover latency against rdn_timing_bound() and checks state
 * continuity, link redundancy, split-brain handling, fault handling and
 * all three transports (loopback, VME ring on host memory, UDP localhost).
 *
 * Timing parameters are sized for a general-purpose host: sched_jitter_us
 * is generous. On the target the same tests run with target figures.
 */
#include <pthread.h>
#include <string.h>
#include "rdn.h"
#include "rdn_osal.h"
#include "rdn_test.h"

#define BIG 6000u

/* fields shared between the app thread and the test thread */
#define LD(x)     __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ST(x, v)  ((void)__sync_lock_test_and_set(&(x), (v)), __sync_synchronize())

typedef struct app {
    rdn_node_t *n;
    uint8_t     id;
    struct { uint32_t counter; uint32_t magic; } ctl;
    uint8_t     big[BIG];
    pthread_t   th;
    int      run;
    uint32_t last_ok;          /* last counter acked by standby */
    uint32_t last_try;
    uint64_t t_active;         /* outputs permitted */
    int      activations;
    int      cold;
    uint32_t activate_txn;
    int      deactivations;
} app_t;

static rdn_config_t base_cfg;
static rdn_timing_bound_t bound;

/* Latency assertions hold only when the platform meets the declared
 * scheduling jitter (cfg.sched_jitter_us). Sanitizer builds run 2-15x
 * slower, so `make sanitize` sets RDN_TEST_RELAXED_TIMING=1: timing
 * checks are reported but not enforced; functional checks still are. */
static int relaxed_timing;
#define CHECK_TIMING(c) do {                                                 \
        if (relaxed_timing) {                                                \
            if (!(c)) printf("\n    (timing not met, relaxed: %s)  ", #c);  \
        } else {                                                             \
            CHECK(c);                                                        \
        }                                                                    \
    } while (0)

static void on_state(void *u, rdn_state_t from, rdn_state_t to, uint32_t epoch)
{
    app_t *a = (app_t *)u;
    (void)from; (void)epoch;
    if (to == RDN_ST_ACTIVE) {
        ST(a->t_active, rdn_time_us());
        (void)__sync_fetch_and_add(&a->activations, 1);
    }
}

static int on_activate(void *u, int cold, uint32_t last_txn)
{
    app_t *a = (app_t *)u;
    ST(a->cold, cold);
    ST(a->activate_txn, last_txn);
    if (cold) {
        a->ctl.magic = 0xC0DEu;
    }
    return 0;
}

static void on_deactivate(void *u)
{
    (void)__sync_fetch_and_add(&((app_t *)u)->deactivations, 1);
}

/* application cycle: 2 ms, like an interlocking logic cycle */
static void *app_task(void *arg)
{
    app_t *a = (app_t *)arg;
    while (LD(a->run)) {
        if (rdn_cycle_begin(a->n)) {
            uint32_t c = __sync_add_and_fetch(&a->ctl.counter, 1u);
            int rc;
            a->big[c % BIG] = (uint8_t)c;
            a->big[(c * 7u) % BIG] ^= 0x5A;
            ST(a->last_try, c);
            rc = rdn_cycle_end(a->n);
            if (rc == RDN_OK) {
                ST(a->last_ok, c);
            }
        }
        rdn_sleep_us(2000);
    }
    return NULL;
}

static app_t *app_new(uint8_t id, const rdn_config_t *cfg_in, const rdn_link_t *links,
                      int nlinks, const rdn_arbiter_t *arb)
{
    app_t *a = (app_t *)calloc(1, sizeof(app_t));
    rdn_config_t cfg = *cfg_in;
    rdn_callbacks_t cb;
    int i;

    a->id = id;
    cfg.node_id = id;
    cfg.peer_id = (uint8_t)(3 - id);
    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = on_state;
    cb.on_activate = on_activate;
    cb.on_deactivate = on_deactivate;
    cb.user = a;
    CHECK_EQ(rdn_create(&cfg, &cb, &a->n), RDN_OK);
    for (i = 0; i < nlinks; i++) {
        CHECK_EQ(rdn_add_link(a->n, &links[i]), RDN_OK);
    }
    if (arb != NULL) {
        CHECK_EQ(rdn_set_arbiter(a->n, arb), RDN_OK);
    }
    CHECK_EQ(rdn_region_register(a->n, 1, &a->ctl, sizeof(a->ctl)), RDN_OK);
    CHECK_EQ(rdn_region_register(a->n, 2, a->big, BIG), RDN_OK);
    CHECK_EQ(rdn_start(a->n), RDN_OK);
    ST(a->run, 1);
    pthread_create(&a->th, NULL, app_task, a);
    return a;
}

static void app_free(app_t *a)
{
    if (a == NULL) return;
    ST(a->run, 0);
    pthread_join(a->th, NULL);
    rdn_destroy(a->n);
    free(a);
}

/* wait until predicate holds; returns elapsed us or -1 on timeout */
static long wait_for(int (*pred)(app_t *, app_t *), app_t *x, app_t *y, uint32_t max_ms)
{
    uint64_t t0 = rdn_time_us();
    while (!pred(x, y)) {
        if (rdn_time_us() - t0 > (uint64_t)max_ms * 1000u) return -1;
        rdn_sleep_us(500);
    }
    return (long)(rdn_time_us() - t0);
}

static int pair_ok(app_t *x, app_t *y)
{
    rdn_state_t a = rdn_state(x->n), b = rdn_state(y->n);
    return (a == RDN_ST_ACTIVE && b == RDN_ST_STANDBY_HOT && rdn_output_permitted(x->n)) ||
           (b == RDN_ST_ACTIVE && a == RDN_ST_STANDBY_HOT && rdn_output_permitted(y->n));
}

static int is_active(app_t *x, app_t *y)
{
    (void)y;
    return rdn_output_permitted(x->n);
}

static app_t *active_of(app_t *x, app_t *y)
{
    return (rdn_state(x->n) == RDN_ST_ACTIVE) ? x : y;
}

static uint32_t rnd_us(uint32_t max)
{
    static uint32_t r = 12345;
    r = r * 1103515245u + 12345u;
    return (r >> 8) % max;
}

/* ------------------------------------------------------------------------ */

static void test_config_and_bound(void)
{
    rdn_config_t c = base_cfg;
    CHECK_EQ(rdn_config_validate(&c), RDN_OK);
    c.hb_miss_limit = 1;
    CHECK_EQ(rdn_config_validate(&c), RDN_E_PARAM);
    c = base_cfg; c.node_id = c.peer_id;
    CHECK_EQ(rdn_config_validate(&c), RDN_E_PARAM);
    c = base_cfg; c.block_size = 100;
    CHECK_EQ(rdn_config_validate(&c), RDN_E_PARAM);
    c = base_cfg; c.startup_listen_us = 1000;
    CHECK_EQ(rdn_config_validate(&c), RDN_E_PARAM);
    c = base_cfg; c.monitor_period_us = c.hb_period_us * 2;
    CHECK_EQ(rdn_config_validate(&c), RDN_E_PARAM);
    printf("\n    bound: detect %u us, takeover %u us, switchover %u us, min_false %u us\n  %-44s",
           bound.detect_us, bound.takeover_us, bound.switchover_us, bound.min_false_us, "");
    CHECK(bound.takeover_us > bound.detect_us);
    CHECK(bound.min_false_us < bound.detect_us);
}

static void test_cold_start_and_replication(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b, *act, *sb;
    uint32_t c;

    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, NULL);
    b = app_new(2, &base_cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b); sb = (act == a) ? b : a;
    CHECK(act == a);                    /* lower node id wins cold start */
    CHECK_EQ(LD(act->cold), 1);
    rdn_sleep_us(100000);
    /* quiesce the active, then compare replicas */
    ST(act->run, 0); pthread_join(act->th, NULL);
    c = LD(act->ctl.counter);
    rdn_commit(act->n);
    rdn_sleep_us(20000);
    rdn_region_lock(sb->n);
    CHECK_EQ(sb->ctl.counter, c);
    CHECK_EQ(sb->ctl.magic, 0xC0DEu);
    CHECK(memcmp(sb->big, act->big, BIG) == 0);
    rdn_region_unlock(sb->n);
    CHECK(c > 20);
    ST(act->run, 1); pthread_create(&act->th, NULL, app_task, act);
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

/* Crash the active at a random phase, N times; each time repair the dead
 * node (fresh incarnation) and let it rejoin as standby. */
static void run_failover_series(const char *label, rdn_link_t *la, rdn_link_t *lb,
                                int nlinks, int iterations,
                                void (*reopen)(int idx, rdn_link_t *out))
{
    app_t *n[2];
    uint32_t worst = 0, total = 0;
    int it, lost_state = 0, over = 0;
    rdn_config_t cfg = base_cfg;

    cfg.repl_mode = RDN_REPL_SYNC;
    n[0] = app_new(1, &cfg, la, nlinks, NULL);
    n[1] = app_new(2, &cfg, lb, nlinks, NULL);
    CHECK(wait_for(pair_ok, n[0], n[1], 2000) >= 0);

    for (it = 0; it < iterations; it++) {
        int ai = (rdn_state(n[0]->n) == RDN_ST_ACTIVE) ? 0 : 1;
        app_t *act = n[ai], *sb = n[1 - ai];
        uint64_t t_fail;
        uint32_t lat, acked;
        long w;

        rdn_sleep_us(30000 + rnd_us(cfg.hb_period_us));
        if (!pair_ok(n[0], n[1]) || (rdn_state(act->n) != RDN_ST_ACTIVE)) {
            /* spurious role change (platform missed its jitter budget):
             * not a valid sample, re-establish and retry */
            CHECK_TIMING(0);
            CHECK(wait_for(pair_ok, n[0], n[1], 3000) >= 0);
            continue;
        }
        ST(sb->t_active, 0);
        t_fail = rdn_time_us();
        rdn_test_freeze(act->n);             /* board dies */
        acked = LD(act->last_ok);
        w = wait_for(is_active, sb, NULL, 1000);
        CHECK(w >= 0);
        if (w < 0) break;
        lat = (uint32_t)(LD(sb->t_active) - t_fail);
        if (lat > worst) worst = lat;
        total += lat;
        if (lat > bound.takeover_us) over++;
        /* continuity: nothing the old active got acknowledged is lost */
        if (LD(sb->activate_txn) == 0 || LD(sb->cold)) lost_state++;
        rdn_sleep_us(10000);
        if (LD(sb->ctl.counter) < acked) lost_state++;

        /* repair: replace the dead node */
        {
            uint8_t id = act->id;
            rdn_link_t l[2];
            int k;
            ST(act->run, 0); pthread_join(act->th, NULL);
            rdn_destroy(act->n);
            free(act);
            for (k = 0; k < nlinks; k++) {
                if (reopen != NULL) reopen(id - 1, &l[k]);
                else l[k] = (id == 1) ? la[k] : lb[k];
            }
            n[ai] = app_new(id, &cfg, l, nlinks, NULL);
        }
        CHECK(wait_for(pair_ok, n[0], n[1], 3000) >= 0);
    }
    printf("\n    %s: %d failovers, avg %u us, worst %u us, bound %u us  \n  %-44s",
           label, iterations, iterations ? total / (uint32_t)iterations : 0u,
           worst, bound.takeover_us, "");
    CHECK_TIMING(over == 0);
    CHECK_EQ(lost_state, 0);
    app_free(n[0]); app_free(n[1]);
}

static void test_failover_loop(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    rdn_loop_pair_links(lp, &la, &lb);
    run_failover_series("loop", &la, &lb, 1, 10, NULL);
    rdn_loop_pair_destroy(lp);
}

static uint32_t vme_a[16384], vme_b[16384];

static void vme_open_side(int idx, rdn_link_t *out)
{
    rdn_vme_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.slot_count = 32; c.slot_size = RDN_MAX_FRAME; c.poll_us = 200;
    c.area_size = sizeof(vme_a);
    c.local_base  = idx == 0 ? vme_a : vme_b;
    c.remote_base = idx == 0 ? vme_b : vme_a;
    CHECK_EQ(rdn_link_vme_open(&c, out), RDN_OK);
}

static void test_failover_vme_ring(void)
{
    rdn_link_t la, lb;
    CHECK(rdn_link_vme_area_size(32, RDN_MAX_FRAME) <= sizeof(vme_a));
    vme_open_side(0, &la);
    vme_open_side(1, &lb);
    run_failover_series("vme", &la, &lb, 1, 5, vme_open_side);
}

static void udp_open_side(int idx, rdn_link_t *out)
{
    rdn_udp_cfg_t c;
    c.local_ip = "127.0.0.1"; c.peer_ip = "127.0.0.1"; c.tos = 0;
    c.local_port = idx == 0 ? 47101 : 47102;
    c.peer_port  = idx == 0 ? 47102 : 47101;
    CHECK_EQ(rdn_link_udp_open(&c, out), RDN_OK);
}

static void test_failover_udp(void)
{
    rdn_link_t la, lb;
    udp_open_side(0, &la);
    udp_open_side(1, &lb);
    run_failover_series("udp", &la, &lb, 1, 5, udp_open_side);
}

static void test_planned_switchover(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b;
    rdn_config_t cfg = base_cfg;
    int i;

    cfg.repl_mode = RDN_REPL_SYNC;
    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &cfg, &la, 1, NULL);
    b = app_new(2, &cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    for (i = 0; i < 6; i++) {
        app_t *act = active_of(a, b), *sb = (act == a) ? b : a;
        uint64_t t0;
        uint32_t lat, acked;
        rdn_sleep_us(40000);
        ST(sb->t_active, 0);
        t0 = rdn_time_us();
        CHECK_EQ(rdn_request_switchover(act->n), RDN_OK);
        CHECK(wait_for(is_active, sb, NULL, 1000) >= 0);
        lat = (uint32_t)(LD(sb->t_active) - t0);
        acked = LD(act->last_ok);
        CHECK_TIMING(lat <= bound.switchover_us + base_cfg.monitor_period_us);
        CHECK(LD(sb->ctl.counter) >= acked);          /* no acked state lost */
        CHECK_EQ(LD(sb->cold), 0);
        CHECK(!rdn_output_permitted(act->n));
        CHECK(wait_for(pair_ok, a, b, 2000) >= 0); /* old active re-syncs */
    }
    CHECK(LD(a->deactivations) >= 3 && LD(b->deactivations) >= 3);
    /* switchover without hot standby is rejected */
    {
        app_t *act = active_of(a, b), *sb = (act == a) ? b : a;
        rdn_test_freeze(sb->n);
        rdn_sleep_us(base_cfg.hb_period_us * (base_cfg.hb_miss_limit + 2));
        CHECK_EQ(rdn_request_switchover(act->n), RDN_OK);
        rdn_sleep_us(20000);
        CHECK(rdn_output_permitted(act->n));
    }
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

static void test_link_redundancy(void)
{
    rdn_loop_pair_t *p0 = rdn_loop_pair_create(64), *p1 = rdn_loop_pair_create(64);
    rdn_link_t la[2], lb[2];
    app_t *a, *b, *act;
    rdn_stats_t st;
    uint32_t applied0;

    rdn_loop_pair_links(p0, &la[0], &lb[0]);
    rdn_loop_pair_links(p1, &la[1], &lb[1]);
    a = app_new(1, &base_cfg, la, 2, NULL);
    b = app_new(2, &base_cfg, lb, 2, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b);
    rdn_loop_set_cut(p0, 1);                  /* e.g. Ethernet cable pulled */
    rdn_sleep_us(300000);
    CHECK_TIMING(pair_ok(a, b));              /* no failover */
    CHECK_TIMING(active_of(a, b) == act);
    rdn_get_stats(b->n, &st);
    CHECK_EQ(st.link[0].up, 0);
    CHECK_EQ(st.link[1].up, 1);
    applied0 = st.txn_applied;
    rdn_sleep_us(100000);
    rdn_get_stats(b->n, &st);
    CHECK(st.txn_applied > applied0);         /* replication moved to link 1 */
    CHECK_TIMING(st.takeovers == 0);
    rdn_loop_set_cut(p0, 0);
    rdn_loop_set_cut(p1, 1);                  /* now the other one */
    rdn_sleep_us(300000);
    CHECK_TIMING(pair_ok(a, b));
    CHECK_TIMING(active_of(a, b) == act);
    rdn_loop_set_cut(p1, 0);
    rdn_sleep_us(100000);
    rdn_get_stats(b->n, &st);
    CHECK(st.link[0].up && st.link[1].up);
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(p0); rdn_loop_pair_destroy(p1);
}

static int both_active(app_t *x, app_t *y)
{
    return rdn_state(x->n) == RDN_ST_ACTIVE && rdn_state(y->n) == RDN_ST_ACTIVE;
}

static void test_split_brain_no_arbiter(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b, *old, *nw;
    long t;
    rdn_stats_t st;

    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, NULL);
    b = app_new(2, &base_cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    old = active_of(a, b); nw = (old == a) ? b : a;
    rdn_loop_set_cut(lp, 1);                     /* total partition */
    CHECK(wait_for(both_active, a, b, 1000) >= 0);
    rdn_loop_set_cut(lp, 0);                     /* heal */
    t = wait_for(pair_ok, a, b, 2000);
    CHECK(t >= 0);
    CHECK_TIMING(t <= (long)(bound.detect_us + 200000));
    CHECK(active_of(a, b) == nw);                /* higher epoch survives */
    rdn_get_stats(old->n, &st);
    CHECK_EQ(st.split_brain_resolved, 1);
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

static void test_split_brain_with_arbiter(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    rdn_arbiter_t arb;
    app_t *a, *b, *act, *sb;
    int i, dual = 0;
    uint64_t t0;
    uint32_t lease = base_cfg.hb_miss_limit * base_cfg.hb_period_us
                     - base_cfg.hb_period_us - base_cfg.sched_jitter_us;

    CHECK_EQ(rdn_lease_arbiter_create(lease, &arb), RDN_OK);
    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, &arb);
    b = app_new(2, &base_cfg, &lb, 1, &arb);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b); sb = (act == a) ? b : a;
    CHECK_EQ(rdn_lease_arbiter_owner(&arb), act->id);
    rdn_loop_set_cut(lp, 1);
    for (i = 0; i < 300; i++) {                  /* 300 ms partition */
        if (rdn_output_permitted(a->n) && rdn_output_permitted(b->n)) dual++;
        rdn_sleep_us(1000);
    }
    CHECK_EQ(dual, 0);
    CHECK(rdn_output_permitted(act->n));
    CHECK_EQ(rdn_state(sb->n), RDN_ST_STANDBY_HOT);   /* claim denied */
    /* now the active really dies: lease expires, standby takes over */
    t0 = rdn_time_us();
    rdn_test_freeze(act->n);
    CHECK(wait_for(is_active, sb, NULL, 1000) >= 0);
    CHECK_TIMING((uint32_t)(LD(sb->t_active) - t0) <= bound.takeover_us + lease);
    CHECK_EQ(rdn_lease_arbiter_owner(&arb), sb->id);
    app_free(a); app_free(b);
    rdn_lease_arbiter_destroy(&arb);
    rdn_loop_pair_destroy(lp);
}

static void test_local_fault_and_maintenance(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b, *act, *sb;
    uint64_t t0;

    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, NULL);
    b = app_new(2, &base_cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b); sb = (act == a) ? b : a;
    rdn_sleep_us(30000);
    t0 = rdn_time_us();
    CHECK_EQ(rdn_report_fault(act->n, 0xBAD), RDN_OK);
    CHECK(!rdn_output_permitted(act->n));          /* inhibited immediately */
    CHECK(wait_for(is_active, sb, NULL, 1000) >= 0);
    CHECK_TIMING((uint32_t)(LD(sb->t_active) - t0) <= bound.switchover_us + base_cfg.monitor_period_us);
    CHECK_EQ(LD(sb->cold), 0);
    rdn_sleep_us(20000);
    CHECK_EQ(rdn_state(act->n), RDN_ST_SAFE);
    rdn_sleep_us(100000);
    CHECK_EQ(rdn_state(act->n), RDN_ST_SAFE);      /* stays SAFE */
    CHECK_EQ(rdn_maintenance_reset(act->n), RDN_OK);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);     /* rejoins as standby */
    CHECK(rdn_output_permitted(sb->n));
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

static void test_lossy_corrupting_link(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b, *act;
    rdn_stats_t sa, sbs;
    int i, bad = 0;

    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, NULL);
    b = app_new(2, &base_cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b);
    rdn_loop_set_drop(lp, 20);                  /* 2 % loss      */
    rdn_loop_set_corrupt(lp, 20);               /* 2 % bit flips */
    for (i = 0; i < 1000; i++) {
        rdn_state_t x = rdn_state(a->n), y = rdn_state(b->n);
        if (x == RDN_ST_SAFE || y == RDN_ST_SAFE) bad++;
        if (rdn_output_permitted(a->n) && rdn_output_permitted(b->n)) bad++;
        rdn_sleep_us(1000);
    }
    CHECK_EQ(bad, 0);
    CHECK_TIMING(active_of(a, b) == act);       /* no false failover */
    rdn_get_stats(a->n, &sa);
    rdn_get_stats(b->n, &sbs);
    CHECK(sa.link[0].rx_bad_crc + sbs.link[0].rx_bad_crc > 0);
    CHECK_TIMING(sa.takeovers + sbs.takeovers == 1);
    rdn_loop_set_drop(lp, 0);
    rdn_loop_set_corrupt(lp, 0);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);  /* resynchronises */
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

static void test_standby_crash_keeps_active(void)
{
    rdn_loop_pair_t *lp = rdn_loop_pair_create(64);
    rdn_link_t la, lb;
    app_t *a, *b, *act, *sb;
    rdn_stats_t st;

    rdn_loop_pair_links(lp, &la, &lb);
    a = app_new(1, &base_cfg, &la, 1, NULL);
    b = app_new(2, &base_cfg, &lb, 1, NULL);
    CHECK(wait_for(pair_ok, a, b, 2000) >= 0);
    act = active_of(a, b); sb = (act == a) ? b : a;
    rdn_test_freeze(sb->n);
    rdn_sleep_us(200000);
    CHECK(rdn_output_permitted(act->n));
    rdn_get_stats(act->n, &st);
    CHECK_EQ(st.peer_alive, 0);
    CHECK_EQ(rdn_commit(act->n), RDN_E_NOPEER);   /* degraded, still runs */
    app_free(a); app_free(b);
    rdn_loop_pair_destroy(lp);
}

int main(void)
{
    rdn_log_level = RDN_LOG_ERR;
    relaxed_timing = getenv("RDN_TEST_RELAXED_TIMING") != NULL;
    rdn_config_defaults(&base_cfg);
    base_cfg.monitor_period_us   = 2000;
    base_cfg.hb_period_us        = 10000;
    base_cfg.hb_miss_limit       = 3;
    base_cfg.startup_listen_us   = 100000;
    base_cfg.sync_retry_us       = 20000;
    base_cfg.link_latency_max_us = 1000;
    base_cfg.sched_jitter_us     = 6000;   /* general-purpose host */
    base_cfg.activate_budget_us  = 2000;
    base_cfg.arbiter_budget_us   = 500;
    base_cfg.ack_timeout_us      = 20000;
    rdn_timing_bound(&base_cfg, rdn_time_resolution_us(), &bound);

    printf("test_system\n");
    RUN(test_config_and_bound);
    RUN(test_cold_start_and_replication);
    RUN(test_failover_loop);
    RUN(test_failover_vme_ring);
    RUN(test_failover_udp);
    RUN(test_planned_switchover);
    RUN(test_link_redundancy);
    RUN(test_split_brain_no_arbiter);
    RUN(test_split_brain_with_arbiter);
    RUN(test_local_fault_and_maintenance);
    RUN(test_lossy_corrupting_link);
    RUN(test_standby_crash_keeps_active);
    return test_report("test_system");
}
