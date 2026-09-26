# Safety notes

> **Status.** vxredund is a *reference* implementation of a redundancy
> pattern. It is **not** a certified product and carries no SIL claim.
> It was built to make a SIL argument *easier to construct*: behaviour is
> specified, bounded and tested, and the integrator's obligations are
> listed explicitly. A SIL claim needs the full EN 50126 / EN 50128 /
> EN 50129 (or IEC 61508) lifecycle on the integrated system.

## 1. Hazards addressed

| Id | Hazard | Mitigations in vxredund | Integrator obligations |
|---|---|---|---|
| H1 | **Two nodes drive outputs at once** (dual active) | Epoch rule: the most recent takeover wins and the other demotes in one heartbeat (§ STATE_MACHINE rows 23–24). An optional arbiter must grant every activation. A lost lease forces SAFE. `rdn_output_permitted()` combines role, activation completion and monitor-task liveness. The cycle bracket stops a demotion interleaving with an application cycle | For SIL, provide a **hardware** interlock on the outputs (cross-inhibit relays / output selector driven by the active unit), with the software arbiter as a second barrier. Without an arbiter, a total loss of *all* links with both CPUs alive yields dual-active until the links recover |
| H2 | **No node drives outputs** (both passive) | A startup timeout forces a cold start. Standbys retry a denied claim every cycle. Tie-breaks are deterministic. The randomised simulation shows convergence after faults | Hardware watchdog on each board (reset on monitor-task starvation) |
| H3 | **New active runs on stale or corrupt state** | A standby is `HOT` only after a verified full snapshot. Transactions reach the replica atomically, and only after fragment count, transaction chain, layout signature and a CRC-32C of the *complete* resulting state have verified. `RDN_REPL_SYNC` means a committed-and-acknowledged cycle is never lost. Cold takeover from `STANDBY_SYNC` is disabled by default (→ SAFE) | Choose SYNC vs ASYNC per data item. `on_activate(cold=1)` must restore a safe initial state (e.g. all signals at danger) |
| H4 | **Active is faulty but keeps sending heartbeats** | `health_check()` callback every monitor cycle. `rdn_report_fault()`. Lease loss. Monitor starvation inhibits outputs | Implement `health_check()` to cover the *application* (cycle counter watchdog, I/O read-back, memory tests). Heartbeats only prove the redundancy task is alive |
| H5 | **Standby fails silently** | The active sees `peer lost` (degraded), counted in stats. `rdn_commit()` returns `RDN_E_NOPEER` | Alarm and maintenance procedure for degraded operation |
| H6 | **False failover** (spurious switch) | Timeout sized to tolerate lost heartbeats (validated). Heartbeats on all links. Peer declared lost only when all links are silent | Measure `L_max` and `J` on the target. Keep the links physically diverse (backplane + separate cable) |
| H7 | **Corruption / masquerade on the link** | See § 2 | Closed transmission system (EN 50159 category 1). Add cryptographic authentication for category 2/3 |
| H8 | **Common-cause software fault** (same code on both nodes) | Not addressed by redundancy | Systematic-fault measures of EN 50128, diversity where required |

## 2. Communication defences (EN 50159 threats)

| Threat | Defence | Where | Test |
|---|---|---|---|
| Repetition | Per-link sequence number, strictly increasing within a sender incarnation | `rdn_seq_accept()` | `test_sequence_window` |
| Deletion | Sequence gap counted. Heartbeat timeout. Replication: fragment index, fragment count in COMMIT, transaction chain (`base == applied`) | `rdn_proto.c`, `rdn_repl.c` | `test_lost_fragment_is_atomic`, `test_lost_commit_detected`, `test_whole_txn_lost_detected` |
| Insertion | CRC, cluster id, source and destination node id, sequence window. UDP: source address and port must match the peer | `rdn_proto_decode()`, `udp_recv()` | `test_length_and_identity` |
| Re-sequencing | Sequence window rejects old frames. Fragments must arrive in order. Replication uses one link at a time | `rdn_seq_accept()`, `rdn_repl_rx_data()` | `test_sequence_window` |
| Corruption | CRC-32C per frame (every 1- and 2-bit error detected in tests), plus CRC-32C of the full state per transaction (end-to-end) | `rdn_proto.c`, `rdn_repl.c` | `test_every_bit_flip_detected`, `test_double_bit_flips_detected`, `test_payload_corruption_detected_by_state_crc`, `test_lossy_corrupting_link` |
| Delay | Bounded staleness: a peer silent for `T_to` is lost. Delayed old frames are rejected by sequence. The clocks are not synchronised, so there is no timestamp-based freshness check | monitor task | `test_failover_*` |
| Masquerade | Cluster id + node ids + (UDP) peer address. No cryptography | – | `test_length_and_identity` |

CRC-32C (Castagnoli) is used because its Hamming distance at these frame
lengths is higher than IEEE CRC-32's. The residual error rate on the
target channel must be evaluated in the safety case. The frame CRC is not
the only barrier: the transaction's state CRC is a second, independent
check over the replicated data.

## 3. Design rules followed

The code follows these MISRA-C:2012-inspired rules:

- All memory is allocated in `rdn_create()`, `rdn_add_link()` and
  `rdn_region_register()`. Nothing is allocated after `rdn_start()`.
- No recursion. Every loop has a static bound, or is a task main loop.
- Every blocking call has a finite timeout (receive, semaphore, ACK wait).
- The state machine is a pure function with a documented, exhaustively
  tested transition table. A corrupted state value leads to SAFE.
- Fixed action order: outputs are inhibited before anything else happens.
- Lock order `commit_lock → lock → reg_lock → tx_lock` is documented and
  never inverted.
- The wire format is serialised byte by byte (no packed structs), so it is
  independent of endianness and compiler layout.
- Builds warning-free with `-Wall -Wextra -Wconversion -Werror`.
- Tests are clean under AddressSanitizer + UBSan and ThreadSanitizer.
  The only TSan suppression covers the VME ring, whose cross-CPU protocol
  (ordered stores and barriers, no RMW) TSan cannot model; it is justified
  in `tests/tsan.supp`.

Known deviations to justify or remove in a certified derivative:

- Function pointers (callbacks, link ops, arbiter).
- `memcpy`/`memcmp`.
- Variadic logging.
- GCC `__sync` builtins and inline `sync`.
- Casts between `volatile` pointers in the VME ring.

## 4. Requirements and traceability

| Req | Requirement | Verified by |
|---|---|---|
| R1 | Exactly one node is ACTIVE in steady state; roles are decided deterministically | `test_cold_start_and_replication`, `test_simulation_*` |
| R2 | On failure of the active, the standby permits outputs within `takeover_us` | `test_failover_loop`, `test_failover_vme_ring`, `test_failover_udp` |
| R3 | No acknowledged state is lost on failover (SYNC mode) | `run_failover_series` (continuity check), `test_planned_switchover` |
| R4 | Loss of one link never causes failover; replication moves to the remaining link | `test_link_redundancy` |
| R5 | Planned switchover completes within `switchover_us`, and the old active re-synchronises as standby | `test_planned_switchover` |
| R6 | A local fault inhibits outputs immediately and hands over to a hot standby; the faulty node stays SAFE until a maintenance reset | `test_local_fault_and_maintenance` |
| R7 | After a total partition, dual-active is resolved on recovery, and the most recent active survives | `test_split_brain_no_arbiter` |
| R8 | With an arbiter, no two nodes ever permit outputs, even under total partition | `test_split_brain_with_arbiter`, `test_simulation_with_arbiter` |
| R9 | Link loss and corruption at 2 % each cause neither SAFE nor dual output nor false failover | `test_lossy_corrupting_link` |
| R10 | A replica is never observed partially updated; any gap or corruption is detected and repaired by a full resync | `test_repl.c` (all) |
| R11 | A failed standby does not affect the active (degraded operation reported) | `test_standby_crash_keeps_active` |
| R12 | Transition table and state-machine invariants as specified | `test_transition_table`, `test_exhaustive_invariants` |
| R13 | VME ring: ordered, bounded, recovers from either side restarting | `test_vme_ring.c` (all) |
| R14 | Invalid configurations are rejected | `test_config_and_bound` |

## 5. What remains for a real deployment

1. Establish `L_max`, `J`, `T_act`, `T_arb` and `T_cyc` on the target by
   measurement under worst-case load. Re-run the fault-injection campaign
   on hardware.
2. Hardware output interlock and a hardware watchdog (H1, H2).
3. Application `health_check()` (H4).
4. Port the system test harness to the target (it uses only POSIX threads
   and the public API), or drive the same scenarios from an external test
   rig.
5. Independent review of this document, the transition table and the
   timing derivation, as part of the verification plan.
6. Static analysis (MISRA checker) and structural coverage measurement as
   required by the target SIL.
