# Failover timing

All bounds are computed by `rdn_timing_bound()` ([`src/rdn_config.c`](../src/rdn_config.c))
from the configuration and the clock resolution. `rdn_config_validate()`
refuses configurations that cannot tolerate at least one lost heartbeat.

## Parameters

| Symbol | Config field | Meaning |
|---|---|---|
| `T_hb` | `hb_period_us` | Heartbeat transmit period (on every link) |
| `N` | `hb_miss_limit` | Heartbeats that may be missed before the peer is declared lost |
| `T_to` | `N · T_hb` | Silence timeout |
| `T_mon` | `monitor_period_us` | Monitor task period (≤ `T_hb`) |
| `L_max` | `link_latency_max_us` | Worst-case one-way latency from send to receiver task timestamp, including VME polling interval |
| `J` | `sched_jitter_us` | Worst-case release jitter / preemption of the monitor task |
| `q` | `rdn_time_resolution_us()` | Clock resolution: 1 tick (1 ms at `sysClkRateSet(1000)`), or ≈1 µs with `RDN_VX_USE_TIMEBASE` |
| `T_arb` | `arbiter_budget_us` | WCET of arbiter `claim()` |
| `T_act` | `activate_budget_us` | WCET of the application's `on_activate()` |
| `T_cyc` | – | WCET of one application cycle inside `rdn_cycle_begin/end` |

## Unplanned failover (active dies)

Let the active fail at time `t_f`.

1. **Last evidence of life.** In the worst case the active emits a heartbeat
   right at `t_f`. It is time-stamped by the standby's receiver at
   `t_r ≤ t_f + L_max`.
2. **Detection.** The monitor declares the peer lost at the first cycle
   where `now − t_r > T_to` on *every* link. Cycles are at most
   `T_mon + J` apart. Both time stamps are quantised to `q`, so the
   measured difference can lag the true one by up to `2q`.
   So `t_d ≤ t_r + T_to + T_mon + J + 2q`.
3. **Takeover.** The `PEER_TIMEOUT → TAKEOVER → CLAIM → ACTIVE → on_activate()`
   chain executes in the same monitor cycle, so outputs are permitted at
   `t_a ≤ t_d + T_arb + T_act`.

```
detect   = L_max + T_to + T_mon + J + 2q
takeover = detect + T_arb + T_act
```

Heartbeats are sent on *all* links and the peer is lost only when *all*
links are silent. A single link failure never causes a failover.

## Planned switchover / fault handover

The active inhibits its outputs, releases the arbiter and sends
`RELINQUISH` on the replication link. The standby's receiver wakes the
monitor immediately (it does not wait for the next period):

```
switchover = L_max + J + q + T_arb + T_act      (+ T_cyc on the node being demoted)
```

`T_cyc` applies because the demotion waits for an application cycle that is
in progress inside `rdn_cycle_begin/end`. Keep the cycle short, or use
`rdn_report_fault()`: fault inhibition is never deferred.

## No false failover

The shortest real silence that can trip the timeout is `T_to − q`
(`min_false_us`). The validator requires

```
T_to ≥ 2·T_hb + J + L_max          (one lost heartbeat tolerated)
T_to − q ≥ 2·T_hb + J              (checked in rdn_create with the real q)
startup_listen ≥ T_to              (a live peer is heard before cold start)
```

Increase `N` to tolerate more consecutive losses on the last surviving link.
Each increment adds `T_hb` to the failover bound.

## Arbiter interaction

With a lease-style arbiter, choose `lease ≤ T_to − T_hb − J`. A crashed
active's lease has then expired by the time the standby detects the failure,
so the claim adds no delay. If the claim is denied (the lease is still held,
meaning the active may be alive but unreachable), the standby retries every
monitor cycle. Its takeover bound becomes `max(takeover, lease expiry)`.

## Worked example: MVME5500, 1 kHz tick

| | Value |
|---|---|
| `q` | 1 000 µs (tick; use `RDN_VX_USE_TIMEBASE` to remove) |
| `T_hb`, `N`, `T_to` | 10 ms, 3, 30 ms |
| `T_mon` | 2 ms |
| `L_max` | 2 ms (VME ring polled every tick; GbE is typically well below) |
| `J` | 1 ms (monitor at priority 50, above application tasks) |
| `T_arb`, `T_act` | 0.5 ms, 5 ms |
| **detect** | 2 + 30 + 2 + 1 + 2 = **37 ms** |
| **takeover** | 37 + 0.5 + 5 = **42.5 ms** |
| **switchover** | 2 + 1 + 1 + 0.5 + 5 = **9.5 ms** (+ `T_cyc`) |

These numbers are *design* values. `L_max`, `J` and the two budgets must be
established by measurement on the target, under worst-case load. Use
`rdnShow` (`max_detect_us`, `max_takeover_us`) during a fault-injection
campaign; see [MVME5500.md](MVME5500.md).

## Host verification

`tests/test_system.c` freezes the active at a random phase relative to its
heartbeat. It measures the time from the freeze to `outputs permitted` on
the standby and asserts `≤ takeover`. The host configuration is
`T_hb = 10 ms`, `N = 3`, `T_mon = 2 ms`, `J = 6 ms` and `L_max = 1 ms`,
which gives a bound of 41.5 ms. Representative results on a 4-core Linux
host, repeated with every core saturated by busy loops:

| Transport | Failovers | Average | Worst | Bound |
|---|---|---|---|---|
| Loopback (fault-injecting) | 10 | ≈ 30 ms | ≈ 32 ms | 41.5 ms |
| VME ring (host memory) | 5 | ≈ 30 ms | ≈ 32 ms | 41.5 ms |
| UDP (127.0.0.1) | 5 | ≈ 30 ms | ≈ 32 ms | 41.5 ms |

Measured values sit just above `T_to`, as the derivation predicts. The
remainder of the bound is margin for jitter, which Linux mostly did not
consume.
