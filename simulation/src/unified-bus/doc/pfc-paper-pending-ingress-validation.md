# PAPER pending ingress accounting validation

Ticket 01 fixes PAPER capacity accounting while a retained packet still owns its original ingress
reservation. Baseline: `3529213c5ff5acd00a0ee2d01bbb73b0c80ae811`; branch: `fix/rerouting-fc`.
Implementation commit: `18a984b6`.
The approved local design, ticket and raw evidence are under
`.scratch/pfc-paper-pending-ingress-accounting/` (ignored local tracker).

## Change and boundaries

QueueManager maintains one unsigned subset counter for held pending bytes. Processing and ordinary
VOQ transfers increment it; pending handoff/removal decrements it according to the existing
`ingressHeld` ownership. Init clears it and decrements check underflow. Both PAPER consumers use
one private constant-time calculation: generic switch total minus recovery VOQ plus held pending.
No new packet marker, queue, scan, routing algorithm or PFC recheck is introduced. Generic switch
total/output backlog, ordinary egress inclusion, independent recovery storage, LastAck gating,
credit return and allocation/scheduling retain their existing meaning.

## Regression evidence

- `red-flow-control.log`: unmodified production fails both processing and ordinary VOQ variants.
  Moving 40,960 B to pending incorrectly changes XOFF/XON from 25,500/17,308 to 66,460/58,268 B.
  The observed ingress changes from PAUSE to RESUME, a 70,000 B boundary packet is incorrectly
  admitted, and a 60,000 B packet misses the required headroom allocation.
- `red-network-3.log`: unmodified production sends a RESUME that reaches the healthy upstream at
  4.026 us, before the real routed LastAck at 11.025 us. This proves the erroneous RESUME
  through the production control receiver.
  The fixture injects 40 KiB through an independent healthy ingress, with 35 KiB held on the
  observed ingress. Small ordinary completions trigger existing production PFC rechecks.
- The network fixture uses the existing ReroutingTopology, production URMA builder and
  SwitchHandlePacket control receiver. No direct LastAck-success call or test protocol parser is
  used. Its final byte multiset includes recovered, trigger and subsequent upstream packets.
- Earlier network fixture attempts are retained as failures: `red-network-build.log` had two test
  compilation errors; `red-network.log` violated original-ingress routing exclusion;
  `red-network-2.log` lacked an initial completed-enqueue PFC recheck. Correcting only the fixture
  produced the definitive baseline failure above. Initial GREEN followup probes also used the
  wrong source boundary (direct egress, then forwarded accounting for a local source). Final tests
  reuse the existing generated-source `UbPacketQueue` / `RegisterTpWithAllocator` fixture and
  assert actual queue retention/drain. Earlier followup probes are not progress evidence; these
  intermediate runs are not acceptance.

## Acceptance map

| Contract | Regression seam |
| --- | --- |
| A01 | Both queue producers preserve ingress and literal PAPER XOFF/XON; no RESUME |
| A02 | Two partial held handoffs conserve capacity; true dequeue restores RESUME and full capacity |
| A03 | Mixed independent pending, recovery VOQ and removal leave held capacity unchanged |
| A04 | Boundary rejection and actual headroom allocation use corrected PAPER admission |
| A05 | Real failure/WAIT_LAST_ACK retains 40 KiB; healthy upstream sees no early RESUME |
| A06 | Network LastAck, true release, upstream RESUME and queued followup progress; exact byte set |
| A07 | Existing PAPER/fixed/dynamic PFC and exclusive/shared CBFC regressions remain in acceptance |
| A08 | Held removal, partial/all handoff, Init and full network drain have no residual correction |

## Commands and final results

Executed sequentially on the release profile with tests enabled:

| Command | Result | Local evidence |
| --- | --- | --- |
| `ninja -C cmake-cache -j4 test-runner` | PASS | `green-build-3.log` (final source) |
| `build/utils/ns3.44-test-runner --test-name=unified-bus-flow-control --verbose` | PASS, 35 cases | `acceptance-flow-control.log` |
| `build/utils/ns3.44-test-runner --test-name=unified-bus-reliable-rerouting --verbose` | PASS, 57 cases | `final-reliable-rerouting.log` |
| `build/utils/ns3.44-test-runner --test-name=unified-bus-switch-voq --verbose` | PASS, 32 cases | `final-switch-voq.log` |
| `python3.12 ./ns3 build -j4` | PASS, all configured targets | `final-build.log` |
| `git diff --check` | PASS | run before implementation commit |

The flow-control suite was not repeated after network-only fixture changes. The final network
regression observes PAUSE at the healthy upstream, zero RESUMEs before LastAck, one queued source
packet still blocked at 8 us, one genuine RESUME after the received LastAck, and subsequent source
queue drain. All 20 offered packets arrive with the exact expected byte multiset, zero runtime
packet drops, zero final switch/pending occupancy and fully restored PAPER threshold (102,400 B).
Expected child-process aborts in negative tests are part of the passing suites.

## Separate review

Root baseline review checked ownership transitions, all counter producers/exits, callback ordering,
old error branches, generic occupancy, credit/LastAck/scheduler preservation and fixture reuse.
Independent read-only Standards and Spec reviews used `3529213c...18a984b6`:
Standards **0 findings**, Spec **0 findings**. Both confirmed the owner/helper/fixture reuse and
preserved protocol boundaries; the Spec reviewer also inspected the actual RED/GREEN logs.
Evidence: local `evidence/review.md`. No outstanding ticket defects.


## Knowledge sources

- `doc/adr/0029-retain-original-packets-for-failure-recovery.md`: retained ingress versus independent
  recovery storage is the existing ownership contract, not a new protocol choice.
- `doc/adr/0027-use-packet-boundaries-for-routed-lastack.md`: actual network LastAck and packet-level
  recovery boundary.
- `doc/adr/0021-model-switch-allocation-as-wakeup-snapshot.md`: preserve allocator transactions.
- `CONTEXT.md`: pending recovery ingress occupancy terminology. Pre-existing user edits are preserved
  and excluded from this ticket's commits.

## Limits

This ticket does not fix late control frames on failed ports, fixed-PFC zero resume thresholds,
LastAck timeout policy or runner stopping. No large Clos experiment or repository-wide test pass
is claimed; acceptance is the three suites named by the approved design and a full configured build.
