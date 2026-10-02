# vxredund: active/standby redundancy for VxWorks on VME

A reference middleware for **fail-operational 1oo2 hot-standby** pairs of
VxWorks 6.9 / 7 boards, such as MVME5500 CPUs in railway signalling or
telecom transport racks. It replicates application state from the active
node to the standby over the **VME backplane** (shared-memory ring), a
**dedicated Gigabit Ethernet link** (UDP), or both. It fails over within a
**computable worst-case time**.

In many legacy deployments this logic is bespoke, undocumented code
("tribal knowledge") that nobody dares touch. vxredund is meant to be the
opposite: a small, readable pattern with a specified state machine, a
derived timing bound, an explicit safety analysis, and tests that run on
a laptop in seconds.

> Not a certified product and no SIL claim. See [docs/SAFETY.md](docs/SAFETY.md)
> for what it provides towards a SIL argument and what the integrator must add.

## What you get

| | |
|---|---|
| **State machine** | Pure function, 7 states / 13 events, [transition table](docs/STATE_MACHINE.md) verified row by row and exhaustively for invariants, plus a randomised two-node simulation |
| **Failover bound** | `takeover ≤ L_max + N·T_hb + T_mon + J + 2q + T_arb + T_act` ([derivation](docs/TIMING.md)), computed at run time by `rdn_timing_bound()` and checked by the system tests |
| **Replication** | Block-delta transactions, applied atomically on the standby after verifying fragment count, transaction chain and a CRC-32C of the whole state. Automatic full resync on any gap. ASYNC or SYNC (commit waits for the standby's ACK) |
| **Links** | VME ring (write-only across the backplane, restart-safe), UDP over GbE, loopback with loss/corruption injection. Heartbeats on all links; failover only when *all* are silent |
| **Split-brain** | Epochs ensure the most recent takeover wins on recovery. An optional arbiter interface prevents dual-active outright (reference lease arbiter included) |
| **Safety hooks** | `rdn_output_permitted()` (role + activation + monitor liveness), immediate fault inhibition, `health_check()` callback, SAFE state with maintenance reset, application-cycle bracket |
| **Portability** | OS abstraction: VxWorks 6.9 / 7 kernel, POSIX (host verification). Endian-neutral wire format |

## Quick start (host)

```sh
cd vxredund
make test         # unit + simulation + system tests (a few seconds)
make sanitize     # the same under ASan+UBSan and ThreadSanitizer
```

Two-process demo over UDP:

```sh
./build/rdn_demo 1 &      # becomes ACTIVE (lower id wins the cold start)
./build/rdn_demo 2        # becomes STANDBY_HOT after a full sync
kill -9 %1                # node 2 takes over within the printed bound, WARM
```

## Using it

```c
#include "rdn.h"

static struct { uint32_t cycle; uint8_t routes[64]; } il;   /* replicated */

rdn_config_t cfg;
rdn_config_defaults(&cfg);
cfg.node_id = 1; cfg.peer_id = 2;
cfg.repl_mode = RDN_REPL_SYNC;             /* no acknowledged cycle is lost */

rdn_callbacks_t cb = { .on_activate = my_on_activate,   /* cold => safe init */
                       .health_check = my_selftest };

rdn_node_t *n;
rdn_link_t gbe, vme;
rdn_create(&cfg, &cb, &n);
rdn_link_udp_open(&udp_cfg, &gbe);  rdn_add_link(n, &gbe);  /* link 0 */
rdn_link_vme_open(&vme_cfg, &vme);  rdn_add_link(n, &vme);  /* link 1 */
rdn_region_register(n, 1, &il, sizeof il);   /* same ids/sizes on both */
rdn_start(n);

for (;;) {                                  /* application cycle */
    if (rdn_cycle_begin(n)) {               /* active: regions are ours */
        run_interlocking_logic(&il);
        if (rdn_output_permitted(n))
            drive_outputs(&il);
        rdn_cycle_end(n);                   /* replicate the transaction */
    }
    wait_for_next_cycle();
}
```

On the standby, `rdn_cycle_begin()` returns 0 and `il` is kept up to date
by the middleware, so a takeover resumes from the last verified cycle.

On VxWorks/MVME5500, `vxworks/rdn_vx_mvme5500.c` provides VME window
mapping and shell commands (`rdnDemoStart`, `rdnShow`, `rdnSwitch`,
`rdnFault`, `rdnReset`, `rdnStop`); see [docs/MVME5500.md](docs/MVME5500.md).

## Verification summary

| Suite | What | Checks |
|---|---|---|
| `test_fsm` | Transition table (55 rows); exhaustive invariants over 7×13×7×24 inputs; 400 randomised two-node histories with crashes/partitions/faults | ≈ 79 000 |
| `test_proto` | CRC-32C check value; all 1-bit and a sweep of 2-bit errors detected; identity, length and sequence-window rules | 45 |
| `test_repl` | Deltas, lost fragment / commit / whole transaction, payload corruption, layout mismatch, 2000-step randomised run with drops | ≈ 5 900 |
| `test_vme_ring` | Wrap-around, bounded full, garbage slots, consumer cold restart (incl. the full-ring corner case), 20 000-frame threaded ordering | ≈ 560 |
| `test_system` | Real tasks: cold start, failover × 20 over loopback/VME ring/UDP against the bound, SYNC continuity, planned switchover, link loss, split-brain with/without arbiter, local fault + maintenance, 2 % loss + 2 % corruption, standby crash | ≈ 385 |

Representative host result: worst measured takeover ≈ 32 ms against a
computed bound of 41.5 ms (T_hb 10 ms, N 3), stable with every core
saturated. The figures to rely on come from the target measurement
campaign in [docs/MVME5500.md](docs/MVME5500.md).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): layers, tasks, replication, locks
- [docs/STATE_MACHINE.md](docs/STATE_MACHINE.md): states, events, actions, the transition table
- [docs/TIMING.md](docs/TIMING.md): failover bound derivation and worked MVME5500 example
- [docs/SAFETY.md](docs/SAFETY.md): hazards, EN 50159 threat mapping, requirements traceability, integrator obligations
- [docs/MVME5500.md](docs/MVME5500.md): VME windows, caches, build, bring-up checklist

## Status and limits

- The host build and all tests pass (GCC, `-Wall -Wextra -Wconversion
  -Werror`, ASan/UBSan/TSan clean).
- The VxWorks OSAL and MVME5500 glue are compile-checked against the
  VxWorks 6.9 kernel API signatures, but **have not been run on hardware**
  in this reference. The VxWorks 7 layer files are templates.
- There is no cryptographic authentication: this assumes a closed
  transmission system (EN 50159 category 1).
- Two nodes only (1oo2 hot standby). 2oo3 voting is a different pattern.
