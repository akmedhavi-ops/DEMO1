/*
 * rdn_vx_mvme5500.c - VxWorks board glue and target-shell commands for a
 *                     redundant MVME5500 pair (VxWorks 6.9 / 7, kernel)
 *
 * Provides:
 *   rdnVmeAreaInit()   locate this node's ring area in memory exported by
 *                      the A32 slave window and map the peer's area
 *   rdnDemoStart()     start a redundant demo application (toy
 *                      interlocking) over GbE (UDP) and/or the VME ring
 *   rdnShow / rdnSwitch / rdnFault / rdnReset / rdnStop   operator commands
 *
 * Board assumptions (verify against your BSP's config.h; see docs/MVME5500.md):
 *   - Tundra Universe II A32 slave window exports local DRAM, including
 *     the USER_RESERVED_MEM region at sysMemTop(), to the backplane
 *   - an A32 master window reaches the peer board's slave window
 *   - sysClkRateSet(1000) (1 ms tick) or RDN_VX_USE_TIMEBASE
 *
 * NOTE: this file is target glue. It is compile-checked but has not been
 * run on MVME5500 hardware as part of this reference; follow the bring-up
 * checklist in docs/MVME5500.md.
 */
#include <vxWorks.h>
#include <cacheLib.h>
#include <logLib.h>
#include <sysLib.h>
#include <taskLib.h>
#include <vme.h>
#include <stdio.h>
#include <string.h>

#include "rdn.h"
#include "rdn_osal.h"

#define RDN_VME_AM        VME_AM_EXT_SUP_DATA   /* A32 supervisory data */
#define RDN_VME_SLOTS     32u
#define RDN_VME_SLOT_SIZE RDN_MAX_FRAME
#define RDN_DEMO_ROUTES   64

/* ---- VME area mapping ----------------------------------------------------- */

static void vx_cache_inval(void *addr, uint32_t len)
{
    (void)cacheInvalidate(DATA_CACHE, addr, (size_t)len);
}

static void vx_cache_flush(void *addr, uint32_t len)
{
    (void)cacheFlush(DATA_CACHE, addr, (size_t)len);
}

/*
 * localOffset : offset of this node's area inside USER_RESERVED_MEM
 * peerBusAdrs : A32 VME address of the peer's area (as printed by the
 *               peer's rdnVmeAreaInit)
 * Fills cfg; prints this node's own bus address for the peer's config.
 */
STATUS rdnVmeAreaInit(UINT32 localOffset, UINT32 peerBusAdrs, rdn_vme_cfg_t *cfg)
{
    char *local = (char *)sysMemTop() + localOffset;
    char *bus   = NULL;
    char *remote = NULL;
    uint32_t need = rdn_link_vme_area_size(RDN_VME_SLOTS, RDN_VME_SLOT_SIZE);

    if (cfg == NULL) {
        return ERROR;
    }
    if (sysLocalToBusAdrs(RDN_VME_AM, local, &bus) != OK) {
        printf("rdn: local area %p is not exported by an A32 slave window\n",
               (void *)local);
        return ERROR;
    }
    if (sysBusToLocalAdrs(RDN_VME_AM, (char *)(ULONG)peerBusAdrs, &remote) != OK) {
        printf("rdn: peer bus address 0x%08x not reachable by a master window\n",
               (unsigned)peerBusAdrs);
        return ERROR;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_base       = local;
    cfg->remote_base      = remote;
    cfg->area_size        = need;
    cfg->slot_count       = RDN_VME_SLOTS;
    cfg->slot_size        = RDN_VME_SLOT_SIZE;
    cfg->poll_us          = 1000u;      /* one tick at 1 kHz */
    cfg->cache_invalidate = vx_cache_inval;
    cfg->cache_flush      = vx_cache_flush;
    printf("rdn: my VME area: local %p, bus 0x%08lx, %u bytes\n",
           (void *)local, (unsigned long)(ULONG)bus, (unsigned)need);
    printf("rdn: peer area  : bus 0x%08x -> local %p\n",
           (unsigned)peerBusAdrs, (void *)remote);
    return OK;
}

/* ---- demo application ----------------------------------------------------- */

typedef struct {
    UINT32 cycle;
    UINT32 reserved;
    UINT8  route_locked[RDN_DEMO_ROUTES];
    UINT8  signal_aspect[RDN_DEMO_ROUTES];
} rdn_demo_state_t;

static rdn_demo_state_t demoState;
static rdn_node_t      *demoNode;
static volatile int     demoRun;
static TASK_ID          demoTid = TASK_ID_ERROR;

static void demo_on_state(void *u, rdn_state_t from, rdn_state_t to, uint32_t epoch)
{
    (void)u;
    logMsg("rdn: %s -> %s (epoch %d)\n", (_Vx_usr_arg_t)rdn_state_name(from),
           (_Vx_usr_arg_t)rdn_state_name(to), (_Vx_usr_arg_t)epoch, 0, 0, 0);
}

static int demo_on_activate(void *u, int cold, uint32_t last_txn)
{
    (void)u; (void)last_txn;
    if (cold) {
        memset(&demoState, 0, sizeof(demoState));   /* all signals at danger */
    }
    return 0;
}

static int demo_task(_Vx_usr_arg_t arg)
{
    (void)arg;
    while (demoRun) {
        if (rdn_cycle_begin(demoNode)) {
            UINT32 r = ++demoState.cycle % RDN_DEMO_ROUTES;
            demoState.route_locked[r] ^= 1u;
            demoState.signal_aspect[r] = demoState.route_locked[r] ? 2u : 0u;
            if (rdn_output_permitted(demoNode)) {
                /* write outputs to I/O boards here */
            }
            (void)rdn_cycle_end(demoNode);
        }
        rdn_sleep_us(10000u);
    }
    return 0;
}

/*
 * rdnDemoStart 1, "192.168.100.1", "192.168.100.2", 0x0, 0x08000000
 *
 * nodeId      1 or 2
 * localIp     address of this board's dedicated GbE port (NULL: no UDP)
 * peerIp      address of the peer's dedicated GbE port
 * vmeOffset   offset of this node's area in USER_RESERVED_MEM
 * peerVmeBus  A32 bus address of the peer's area (0: no VME link)
 */
STATUS rdnDemoStart(int nodeId, char *localIp, char *peerIp,
                    UINT32 vmeOffset, UINT32 peerVmeBus)
{
    rdn_config_t    cfg;
    rdn_callbacks_t cb;
    rdn_link_t      link;
    rdn_timing_bound_t b;
    int rc;

    if (demoNode != NULL || (nodeId != 1 && nodeId != 2)) {
        return ERROR;
    }
    if (sysClkRateGet() < 1000) {
        printf("rdn: warning: sysClkRate %d Hz; 1000 Hz recommended\n",
               sysClkRateGet());
    }
    rdn_log_level = RDN_LOG_INFO;
    rdn_config_defaults(&cfg);
    cfg.node_id = (uint8_t)nodeId;
    cfg.peer_id = (uint8_t)(3 - nodeId);
    cfg.repl_mode = RDN_REPL_SYNC;
    rdn_timing_bound(&cfg, rdn_time_resolution_us(), &b);
    printf("rdn: node %d, takeover bound %u us, switchover bound %u us\n",
           nodeId, (unsigned)b.takeover_us, (unsigned)b.switchover_us);

    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = demo_on_state;
    cb.on_activate     = demo_on_activate;

    if ((rc = rdn_create(&cfg, &cb, &demoNode)) != RDN_OK) {
        printf("rdn: create: %s\n", rdn_status_name(rc));
        return ERROR;
    }
    if (localIp != NULL && peerIp != NULL) {
        rdn_udp_cfg_t u;
        u.local_ip   = localIp;
        u.peer_ip    = peerIp;
        u.local_port = (uint16_t)(47000 + nodeId);
        u.peer_port  = (uint16_t)(47000 + 3 - nodeId);
        u.tos        = 0xB8;
        if (rdn_link_udp_open(&u, &link) != RDN_OK ||
            rdn_add_link(demoNode, &link) != RDN_OK) {
            printf("rdn: UDP link failed\n");
            goto fail;
        }
    }
    if (peerVmeBus != 0u) {
        rdn_vme_cfg_t v;
        if (rdnVmeAreaInit(vmeOffset, peerVmeBus, &v) != OK ||
            rdn_link_vme_open(&v, &link) != RDN_OK ||
            rdn_add_link(demoNode, &link) != RDN_OK) {
            printf("rdn: VME link failed\n");
            goto fail;
        }
    }
    if (rdn_region_register(demoNode, 1, &demoState, sizeof(demoState)) != RDN_OK ||
        rdn_start(demoNode) != RDN_OK) {
        goto fail;
    }
    demoRun = 1;
    demoTid = taskSpawn("tRdnApp", 60, VX_FP_TASK, 16384, (FUNCPTR)demo_task,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return (demoTid == TASK_ID_ERROR) ? ERROR : OK;
fail:
    rdn_destroy(demoNode);
    demoNode = NULL;
    return ERROR;
}

void rdnShow(void)
{
    rdn_stats_t st;
    unsigned i;

    if (demoNode == NULL) {
        printf("rdn: not started\n");
        return;
    }
    rdn_get_stats(demoNode, &st);
    printf("state %s epoch %u | peer %s alive %u | cycle %u txn %u\n",
           rdn_state_name(st.state), (unsigned)st.epoch,
           rdn_state_name(st.peer_state), st.peer_alive,
           (unsigned)demoState.cycle, (unsigned)st.last_txn);
    printf("takeovers %u split-brain %u sync_full %u sync_lost %u "
           "ack_timeouts %u overruns %u\n",
           (unsigned)st.takeovers, (unsigned)st.split_brain_resolved,
           (unsigned)st.sync_full, (unsigned)st.sync_lost,
           (unsigned)st.ack_timeouts, (unsigned)st.monitor_overruns);
    printf("detect last/max %u/%u us, takeover last/max %u/%u us\n",
           (unsigned)st.last_detect_us, (unsigned)st.max_detect_us,
           (unsigned)st.last_takeover_us, (unsigned)st.max_takeover_us);
    for (i = 0; i < RDN_MAX_LINKS; i++) {
        printf("link %u: up %u tx %u txerr %u rx %u crc %u hdr %u replay %u lost %u\n",
               i, st.link[i].up, (unsigned)st.link[i].tx_frames,
               (unsigned)st.link[i].tx_errors, (unsigned)st.link[i].rx_frames,
               (unsigned)st.link[i].rx_bad_crc, (unsigned)st.link[i].rx_bad_hdr,
               (unsigned)st.link[i].rx_replayed, (unsigned)st.link[i].rx_lost);
    }
}

STATUS rdnSwitch(void) { return rdn_request_switchover(demoNode) == RDN_OK ? OK : ERROR; }
STATUS rdnFault(void)  { return rdn_report_fault(demoNode, 0xF00Du) == RDN_OK ? OK : ERROR; }
STATUS rdnReset(void)  { return rdn_maintenance_reset(demoNode) == RDN_OK ? OK : ERROR; }

STATUS rdnStop(void)
{
    if (demoNode == NULL) {
        return ERROR;
    }
    demoRun = 0;
    taskDelay(sysClkRateGet() / 10);        /* let tRdnApp leave its cycle */
    (void)rdn_stop(demoNode);               /* graceful: relinquish */
    rdn_destroy(demoNode);
    demoNode = NULL;
    return OK;
}
