# Architecture

## Layers

```
 +--------------------------------------------------------------+
 | Application  (interlocking logic, telecom control plane ...) |
 |   rdn_cycle_begin() / write regions / rdn_cycle_end()        |
 +--------------------------------------------------------------+
 | rdn_core   runtime: tasks, peer view, event generation,      |
 |            action execution, commit, statistics              |
 |   +----------------+  +------------------+  +-------------+  |
 |   | rdn_fsm        |  | rdn_repl         |  | rdn_proto   |  |
 |   | pure state     |  | delta/full       |  | framing,    |  |
 |   | machine        |  | transactions     |  | CRC-32C,    |  |
 |   |                |  | atomic apply     |  | seq window  |  |
 |   +----------------+  +------------------+  +-------------+  |
 +--------------------------------------------------------------+
 | rdn_link_*   UDP (GbE) | VME shared-memory ring | loopback   |
 +--------------------------------------------------------------+
 | rdn_osal_*   VxWorks 6.9/7          |  POSIX (host tests)    |
 +--------------------------------------------------------------+
```

`rdn_fsm`, `rdn_repl` and `rdn_proto` have no OS dependencies and are unit
tested in isolation. `rdn_core` is exercised by the system tests with real
tasks.

## Tasks and data flow

```
            application task                  tRdnMon (T_mon)             tRdnRx0 / tRdnRx1
            ----------------                  ---------------             -----------------
 cycle_begin ─┐                               health_check()              recv(timeout)
  write regs  │ commit_lock                   arbiter renew()             decode + CRC + ids
 cycle_end ───┤──> rdn_repl_build()           liveness per link           seq window
              │      DATA… COMMIT ──> link    derive events ─> fsm_step   HB  -> peer view
              └─ (SYNC) wait ACK <─────────── execute actions             DATA/COMMIT -> repl (standby)
                                              heartbeat every T_hb        ACK -> wake committer
                                                                          RELINQUISH/state change
                                                                             -> wake tRdnMon now
```

- Only `tRdnMon` steps the state machine after start, so the FSM is never
  re-entered.
- Receivers wake the monitor immediately for relinquish and peer state
  changes. That is why a planned switchover does not wait for `T_mon`.
- Heartbeats go out on all links. Replication, ACK and RELINQUISH go on one
  link (the first on which the peer is heard), which keeps them in FIFO
  order relative to each other.

## Replication

Each registered region has three images:

| Image | Active | Standby |
|---|---|---|
| `app` | Live application state | Replica (last verified transaction) |
| `shadow` | Last transmitted image (delta reference) | – |
| `work` | – | Staging copy for the open transaction |

The active compares `app` to `shadow` block by block (`block_size`),
coalesces dirty blocks into ≤ 1424-byte frames, and closes the transaction
with a COMMIT. The COMMIT carries the fragment count, a CRC-32C of the
complete state and the layout signature. The standby writes fragments into
`work` and copies `work → app` only when the COMMIT verifies, under
`reg_lock`. On any gap it restores `work` from `app` and requests a full
snapshot (`SYNC_REQ`).

WCET of a commit and of an apply is linear in the total registered state
size, and independent of history.

Memory: `2 × Σ region sizes` for shadow/work, plus about 3 KB per link
(frame buffers), plus the node structure. It is all allocated before
`rdn_start()`.

## Locks

| Lock | Protects | Held by |
|---|---|---|
| `commit_lock` | Replication transmit state; serialises application cycles against role changes | Application cycle, `rdn_commit`, `dispatch()` |
| `lock` | FSM, peer view, event flags, stats, standby replication receive state | Everyone, briefly |
| `reg_lock` | Replica buffers during apply | Receiver (COMMIT), `rdn_region_lock()` |
| `tx_lock` (per link) | Link transmit buffer, sequence counter, tx stats | `link_send()` |

Order: `commit_lock → lock → reg_lock → tx_lock`. Callbacks run without
`lock` held.

## Output permission

`rdn_output_permitted()` is true only when all of these hold:

- `state == ACTIVE` and `on_activate()` has completed;
- no fault has been reported (fault inhibition is immediate and does not
  wait for the monitor);
- the node is not frozen;
- the monitor task ran within `2·T_mon + J + q` (a starved monitor cannot
  keep permission);
- no arbiter renew has failed. A failed renew clears permission at once
  and forces SAFE.

## Files

| Path | Content |
|---|---|
| `include/rdn.h` | Public API |
| `include/rdn_fsm.h` | State machine interface (events, actions) |
| `include/rdn_link.h` | Link interface + UDP / VME / loopback constructors |
| `include/rdn_osal.h` | OS abstraction |
| `src/rdn_fsm.c` | State machine |
| `src/rdn_repl.c/.h` | Replication engine |
| `src/rdn_proto.c/.h`, `src/rdn_crc.c` | Wire protocol, CRC-32C |
| `src/rdn_core.c` | Runtime |
| `src/rdn_config.c` | Defaults, validation, timing bounds |
| `links/` | UDP, VME ring, loopback (fault injection) |
| `arbiter/rdn_arb_lease.c` | Reference lease arbiter (software model) |
| `osal/` | VxWorks and POSIX OSALs |
| `vxworks/` | MVME5500 glue + shell commands, 6.9 DKM Makefile, VxWorks 7 layer templates |
| `tests/` | Unit, simulation and system tests |
| `demo/` | Two-process host demo |
