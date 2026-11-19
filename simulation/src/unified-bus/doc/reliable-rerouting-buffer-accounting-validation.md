# Reliable rerouting storage and admission validation

All four implementation tickets are complete under ADR 0029. This record preserves per-ticket
validation and the final integration limitation; local commands/logs live under `.scratch/ub21-reliable-rerouting-buffer-accounting/evidence/`.
All builds and tests run sequentially on `ub-2.1-dev`, release profile, tests enabled, MPI/MTP off.

## Ticket 01: original packet retention

Baseline: `f992c11de0710ecbab51caa82e2ab31eecc799bc`.
Implementation commit: `f702c63c`.

Normal armed transmission retains the original `Packet` until complete link delivery. Physical
failure transfers all unreceived originals to retry; local detection closes the send set. Disabled
and host access endpoints skip the bookkeeping. The existing permanent-failure delivery gate
invalidates old events before recovery reuses these objects. Port disposal releases the retention
owner, including incomplete work at an early stop.

Executed:

- `python3.12 ./ns3 build -j 4`: PASS, baseline and implementation (all configured targets).
- `build/utils/ns3.44-test-runner --suite=unified-bus-reliable-rerouting --verbose`: PASS.
- `build/utils/ns3.44-test-runner --suite=unified-bus-link-fault-input --verbose`: PASS.
- Isolated baseline and implementation small-case runs via
  `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=<isolated-case>'`: PASS.
- `python3.12 scratch/reliable-rerouting/2nodes_reliable-rerouting/check_results.py <isolated-case>`:
  PASS, 382 transfers, both directions, four completed tasks, actual LastAck paths verified.

Red evidence: the new healthy propagation assertion found four retry entries where the contract
requires zero. Green tests additionally compare original object pointers at destination, cover four
simultaneously propagating packets, recovery before the old delivery deadlines, and an already
delivered packet still occupying the sending port's long IFG. Existing sequence-wrap, bilateral,
error/timeout, duplicate LastAck and same-timestamp event fixtures remain active. An early-stop
assertion verifies retained originals lose all owner/event references at simulator destruction.

Evidence: `evidence/01/{baseline-build,baseline-reliable-rerouting,baseline-link-fault-input,
baseline-case,baseline-check,red,green-build,final-build,green-reliable-rerouting,
green-link-fault-input,implemented-case,implemented-check}.log`; isolated inputs/outputs under
`evidence/01/{baseline,implemented}/2nodes_reliable-rerouting/`.

Review: separate read-only standards/spec reviews against the pinned baseline. The spec review
requested positive-delay CBFC exhaustion evidence; the added production fixture passes, observing
retry growth from one to two, uncredited VOQ retention, detection cutoff and late delivery suppression.
Both axes have zero outstanding findings. The final run is `evidence/01/review-tests.log`;
`review-build.log` records its current-source build. No performance claim is inferred from these runs.
Ticket 02 will change ingress accounting; this ticket deliberately uses the existing generous
small-case capacity. Tickets 03/04 cover migration order and occupancy output.

## Knowledge Sources

- `doc/adr/0029-retain-original-packets-for-failure-recovery.md`: approved ownership/accounting contract.
- `doc/adr/0027-use-packet-boundaries-for-routed-lastack.md`: complete-packet receipt and real LastAck.
- `doc/adr/0021-model-switch-allocation-as-wakeup-snapshot.md`: preserve allocation transactions.
- `dev-knowledge/specification/`: UB specification baseline; this change remains packet-level and
  does not model flit retry memory, finite SRAM, or normal on-wire link ACKs.

## Ticket 02: recovery VOQ and ingress isolation

Baseline `0be480ef`; implementation `f7a1e709`. Both retained attempts and failed-egress packets
enter the existing VOQ with a local origin tag and independent storage accounting. Actual queued
bytes include recovery; ordinary ingress reservations exclude it on both enqueue and dequeue.
Queue accounting and allocator credit context share the marker, which is removed before forwarding.
Paper PFC capacity thresholds exclude the recovery subset. Obsolete ingress-capacity retry/cache
and wakeup machinery is removed; healthy egress and credit backpressure use the existing allocator.

Executed and passed, sequentially:

- `python3.12 ./ns3 build -j 4` (all configured targets).
- `build/utils/ns3.44-test-runner --suite=unified-bus-reliable-rerouting --verbose`.
- `build/utils/ns3.44-test-runner --suite=unified-bus-switch-voq --verbose`.
- `build/utils/ns3.44-test-runner --suite=unified-bus-flow-control --verbose`.

Evidence: `evidence/02/acceptance-build.log` and `acceptance-<suite>.log`. The red fixture retained
the prior occupied-ingress scenario and observed an unwanted wait until 20 us instead of the actual
LastAck at 2.041 us. Eight real-network variants cover exclusive/shared CBFC, RR/DWRR, and healthy
egress-full/credit-empty blocking. Retry and failed-egress packets coexist with ordinary packets,
including an arrival after recovery handoff; two ordinary reservations coexist with two recovery
packets in one four-packet VOQ. The credit-limited variants spend credit using a real warmup transfer
and resume via a peer control frame carried over the healthy link. Delivery, no leaked marker,
zero drops, exact upstream credit return, and final occupancy are asserted.

Independent standards/spec review requested downstream-credit evidence and removal of unreachable
old admission machinery; both are resolved. Intermediate build failure (vector API typo) and fixture
configuration failure (typed enum value) were corrected before the final passing run; their logs
remain in `evidence/02/`. No failed or unrun command is counted as passing. Existing PFC and flow
control regression cases pass. Backlog order and final occupancy output remain tickets 03/04.

## Ticket 03: detection routing and backlog order

Baseline `f2a0cd55`; implementation `8e4c4115`. New route selection already excluded detected failed
outputs through ADR 0028 and remains on that path. Valid LastAck now hands off retry attempts before
switch backlog; the switch collects failed egress before old VOQ, then already-ready processing.
Later processing completion keeps its existing reservation and migrates when ready. An explicit
switch-handoff completion gate prevents an empty retry owner from prematurely reporting DRAINED.

Executed: `python3.12 ./ns3 build -j 4`, followed sequentially by
`build/utils/ns3.44-test-runner --suite=unified-bus-<suite> --verbose` for
`reliable-rerouting`, `link-fault-input`, `switch-voq`, `congestion-control`: all PASS.
Evidence: `evidence/03/green-build.log` and `acceptance-<suite>.log`.

The new real-network order fixture first failed on the old category ordering (`red.log`). It checks
retry, failed egress and two old VOQ packets remain frozen before actual LastAck, while a newcomer
already selects the healthy VOQ. Under egress backpressure all five coexist, retaining precisely
three ordinary ingress reservations. Destination object identities then prove FIFO append order:
existing newcomer, retry, failed egress, first old VOQ, second old VOQ. Existing tests additionally
verify actual newcomer transmission before LastAck, cancelled grants, late processing on both sides
of LastAck, bilateral recovery, route constraints/representations, and no-legal-route errors.

Independent read-only standards and spec reviews both reported zero findings. No scenario output was
overwritten; final small/Clos integration and occupancy CSV validation remain ticket 04.


## Ticket 04: occupancy output and final acceptance

Baseline `ba098bab`; implementation `99b903a4`. `GetOccupancy(stage)` and both CSV outputs read
one authoritative current/peak packet/byte counter set for `in_flight`, `retry`, `failed_egress`,
and `recovery_voq`. Source-to-VOQ handoff transfers counts after successful insertion; service
removes the recovery subset without adding it twice to physical VOQ occupancy. Pending recovery
uses maintained totals; processing work is counted once at detection and decremented on completion.
No per-handoff queue scan or extra disabled-path per-packet accounting is introduced.

Default output is `output/reliable_rerouting_occupancy_summary.csv`; opt-in
`ns3::UbReliableRerouting::TraceOccupancyEvents` adds `reliable_rerouting_occupancy_events.csv`.
Both sample the same counters with integer `time_ps`, explicit handoff `source`, and origin port.
Summary sampling precedes cleanup and is independent of master/packet trace gates. Disabled and
ineligible endpoints emit no rows. DRAINED means failed-output handoff completion, not delivery.

Executed sequentially with the current-source build:

- `python3.12 ./ns3 build -j 4`: PASS, all configured targets (`evidence/04/final-build.log`).
- `build/utils/ns3.44-test-runner --suite=unified-bus-<suite> --verbose`: all six PASS:
  `reliable-rerouting`, `link-fault-input`, `switch-voq`, `flow-control`, `congestion-control`,
  `trace-writer` (`acceptance-<suite>.log`). Real-network fixtures cover query/CSV equality,
  current/peak counts, both handoff sources, and early stop with retry zero and recovery VOQ nonzero.
- `python3.12 -m unittest tests.openusim_skills.test_skill_docs`: 20/20 PASS
  (`docs-tests-final.log`). The initial missing Contents failure was fixed; the failed log is retained.
- `python3.12 test.py --no-build --jobs 1 --text <evidence/04/full-tests>`:
  **181/182 passed; one failed**, `pcap-file`, because `src/network/test/known.pcap` is missing.
  `git ls-tree f992c11d src/network/test/known.pcap` also returns no entry. This unrelated baseline
  fixture remains unresolved; the full suite is not reported as passing. See `full-tests.txt`,
  `full-tests-console.log`, and `baseline-pcap-limitation.txt`. Examples and MPI/MTP were disabled;
  no claim is made for those configurations.
- `python3.12 scratch/reliable-rerouting/validate_routes.py <isolated-Clos>`: PASS,
  16,256 task pairs, 49,272 route states and both three-hop LastAck paths (`clos-route-preflight.log`).
- `python3.12 scratch/reliable-rerouting/run_case.py <isolated-case>`: final small/default,
  detail, no-fault enabled/disabled, master-trace-off, and full Clos runs all exit 0.
- `python3.12 <isolated-small-or-Clos>/check_results.py --self-test`: small/default and detail each
  pass the positive run plus 15 rejection paths; Clos passes those plus four provenance/configuration
  rejection paths (`2nodes_reliable-rerouting-checker.log`, `small-detail-checker.log`,
  `clos-checker.log`). Missing summaries and retry-zero/nonempty-VOQ mutations are rejected.
- The evidence-local `run_extra.py` reuses the existing runner with only its root location and driver
  command suffix adapted. It invokes existing `--canonical-output` and `--stop-ms=1` options for
  two trace-gate canonical runs and the early-stop run; all exit 0 with unchanged runtime fingerprints.
- `python3.12 <evidence/04>/check_variants.py`: PASS (`variant-checks.log`). All nine final scenarios
  have exit-zero manifests binding inputs, source state, scripts, binary and output hashes.

All scenarios use fresh `evidence/04/cases-final/` directories. Preliminary `evidence/04/cases/`
and preexisting checked-in results are preserved. The full test suite's generated smoke files were
moved to `evidence/04/full-suite-generated-smoke-test/`, not added to the source change.

| Scenario / comparison | Observed result |
| --- | --- |
| Small failure, default/detail | Four tasks, 382 recovery transfers, zero runtime drops; 28 protocol result files byte-identical |
| No fault, feature enabled/disabled | 24 packet/lifecycle/port/task/throughput result files byte-identical; disabled emits no occupancy output |
| Master trace on/off | Canonical START/COMPLETE_VISIBLE bytes identical, all four tasks complete; occupancy summary bytes identical |
| Controlled stop at 1 ms | Workload starts 990 us, physical fault 999 us; both directions retain 46 retry packets / 159,108 bytes, DETECTION_PENDING; no completed tasks |
| 128-host Clos | 16,256 unique 64 KiB tasks / 1,065,353,216 bytes complete; 233 transfers; six actual three-hop LastAck sends; zero runtime drops; TP retransmission disabled |

Clos latest task completion is **417.783760 us**. Failed endpoints `(128,8)` and `(144,0)` have
retry/recovery-VOQ peaks of **116 packets / 225,128 bytes** and **117 packets / 229,302 bytes**,
respectively; failed-egress peaks are **2 packets / 4,174 bytes** per endpoint. Completed runs end
with all four current stages zero. Stage peaks occur independently and should not be summed as
simultaneous occupancy.

The Clos wrapper measured **41.613059 s** wall time; the driver reported **40.489697 s** simulation
execution and **0.586056 s** trace parsing. These are observations from one run, not a speedup claim
or simulated throughput measurement. Reporting-toggle comparisons establish behavior parity, not
a statistically controlled overhead benchmark.

Independent read-only standards/spec reviews closed three findings: invoke the occupancy checker,
label each handoff source, and remove repeated processing scans. Both final reviews report zero
outstanding findings (`independent-review.md`). The root agent also reviewed ownership, disabled
paths, cleanup sampling, marker consumption and preserved scheduler/credit/routing behavior.
No feature requirement remains unresolved; the missing base-module pcap fixture is recorded above.
