/*
 * rdn_demo_host.c - two-process demonstration over UDP on one host
 *
 *   terminal 1:  ./build/rdn_demo 1
 *   terminal 2:  ./build/rdn_demo 2
 *
 * Node 1 listens on 127.0.0.1:47001, node 2 on :47002. The "application"
 * is a toy interlocking: a cycle counter plus a table of route states that
 * the active node mutates every 10 ms and replicates.
 *
 *   kill -9 <active pid>   -> crash: standby takes over within the bound
 *   Ctrl-C on the active   -> graceful stop: relinquish, fast switchover
 *   kill -USR1 <active pid>-> planned switchover
 *   kill -USR2 <pid>       -> report local fault (-> SAFE)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdn.h"
#include "rdn_osal.h"

#define ROUTES 64

typedef struct interlocking {
    uint32_t cycle;
    uint32_t reserved;
    uint8_t  route_locked[ROUTES];
    uint8_t  signal_aspect[ROUTES];
} interlocking_t;

static interlocking_t il;
static rdn_node_t *node;
static volatile sig_atomic_t stop_req, switch_req, fault_req;

static void on_sig(int s)
{
    if (s == SIGINT || s == SIGTERM) stop_req = 1;
    if (s == SIGUSR1) switch_req = 1;
    if (s == SIGUSR2) fault_req = 1;
}

static void on_state(void *u, rdn_state_t from, rdn_state_t to, uint32_t epoch)
{
    (void)u;
    printf(">>> %s -> %s (epoch %u)\n", rdn_state_name(from), rdn_state_name(to),
           (unsigned)epoch);
    fflush(stdout);
}

static int on_activate(void *u, int cold, uint32_t last_txn)
{
    (void)u;
    if (cold) {
        memset(&il, 0, sizeof(il));    /* all signals at danger */
    }
    printf(">>> activated %s, replica txn %u, cycle %u\n",
           cold ? "COLD" : "WARM", (unsigned)last_txn, (unsigned)il.cycle);
    return 0;
}

static void on_deactivate(void *u)
{
    (void)u;
    printf(">>> outputs inhibited\n");
}

int main(int argc, char **argv)
{
    rdn_config_t cfg;
    rdn_callbacks_t cb;
    rdn_udp_cfg_t uc;
    rdn_link_t link;
    rdn_timing_bound_t b;
    int id, rc;
    uint64_t next_print = 0;

    if (argc < 2 || (id = atoi(argv[1])) < 1 || id > 2) {
        fprintf(stderr, "usage: %s <1|2>\n", argv[0]);
        return 2;
    }
    rdn_log_level = RDN_LOG_INFO;
    rdn_config_defaults(&cfg);
    cfg.node_id = (uint8_t)id;
    cfg.peer_id = (uint8_t)(3 - id);
    cfg.sched_jitter_us = 5000;
    cfg.repl_mode = RDN_REPL_SYNC;
    rdn_timing_bound(&cfg, rdn_time_resolution_us(), &b);
    printf("node %d: takeover bound %u us, switchover bound %u us\n",
           id, (unsigned)b.takeover_us, (unsigned)b.switchover_us);

    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = on_state;
    cb.on_activate = on_activate;
    cb.on_deactivate = on_deactivate;

    uc.local_ip = "127.0.0.1";
    uc.peer_ip = "127.0.0.1";
    uc.local_port = (uint16_t)(47000 + id);
    uc.peer_port = (uint16_t)(47000 + 3 - id);
    uc.tos = 0xB8;   /* DSCP EF */

    if ((rc = rdn_create(&cfg, &cb, &node)) != RDN_OK ||
        (rc = rdn_link_udp_open(&uc, &link)) != RDN_OK ||
        (rc = rdn_add_link(node, &link)) != RDN_OK ||
        (rc = rdn_region_register(node, 1, &il, sizeof(il))) != RDN_OK ||
        (rc = rdn_start(node)) != RDN_OK) {
        fprintf(stderr, "init failed: %s\n", rdn_status_name(rc));
        return 1;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_sig);
    signal(SIGUSR2, on_sig);

    while (!stop_req) {
        uint64_t now = rdn_time_us();
        if (switch_req) { switch_req = 0; rdn_request_switchover(node); }
        if (fault_req)  { fault_req = 0;  rdn_report_fault(node, 0x42); }
        if (rdn_cycle_begin(node)) {
            il.cycle++;
            il.route_locked[il.cycle % ROUTES] ^= 1u;
            il.signal_aspect[il.cycle % ROUTES] =
                il.route_locked[il.cycle % ROUTES] ? 2u : 0u;
            if (rdn_output_permitted(node)) {
                /* drive outputs here (signal lamps, point machines ...) */
            }
            (void)rdn_cycle_end(node);
        }
        if (now >= next_print) {
            rdn_stats_t st;
            rdn_get_stats(node, &st);
            printf("[%d] %-12s peer %-12s alive %u cycle %-8u txn %-8u "
                   "takeovers %u max_detect %u us\n",
                   id, rdn_state_name(st.state), rdn_state_name(st.peer_state),
                   st.peer_alive, (unsigned)il.cycle, (unsigned)st.last_txn,
                   (unsigned)st.takeovers, (unsigned)st.max_detect_us);
            fflush(stdout);
            next_print = now + 1000000u;
        }
        rdn_sleep_us(10000);
    }
    printf("stopping (graceful)\n");
    rdn_stop(node);
    rdn_destroy(node);
    return 0;
}
