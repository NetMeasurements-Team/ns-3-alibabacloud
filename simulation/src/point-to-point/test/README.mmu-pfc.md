# Small MMU/PFC simulation test

This is a native ns-3 system test suite for the network backend. It does not
need an AI workload, GPU, or MPI launch.
The suite is implemented in [mmu-pfc-test-suite.cc](mmu-pfc-test-suite.cc).

## Topology and workload

```text
source 0 --100 Gb/s--+                    
                    S0 --100 Gb/s-- S1 --25 Gb/s-- sink
source 1 --100 Gb/s--+
```

All links have 1 us one-way propagation. Both ordinary switches have 1.5 us
forwarding delay, 512 KiB buffers, 4 KiB reservation per port, and threshold
shift 3. Headroom uses the same local-delay formula as common.h, including
modeled headers and forwarding delay. There are no NVSwitches. Each source
queues 4,096 packets of 1,000 payload bytes at simulated time 10 us. All data
uses priority 3. The simulation runs for 5 ms, allowing the traffic to drain.

Sources use the real Qbb transmit queue and PFC receive/resume implementation,
with a tiny Node subclass that omits switch accounting at the source. The sink
counts individual source/sequence pairs. No retransmission, ACK traffic, or
congestion-control algorithm can conceal loss or relieve the congestion.
This tests switch backpressure and the Qbb priority scheduler; it does **not**
test the separate RDMA QP scheduler, full SimAI frontend initialization, PFC
expiry/refresh, reverse control-queue contention, or NVSwitch flow control.
The headroom formula is reproduced here, rather than invoking common.h.

## Three cases

| Mode     | Traffic and bottleneck                         | Required result                                                                                        |
|----------|------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| baseline | Only source 0; final link 100 Gb/s             | 4,096 unique packets delivered; no PFC                                                                 |
| on       | Both sources; final link 25 Gb/s; PFC enabled  | All 8,192 unique packets delivered; headroom exercised; S1 pauses S0 and S0 pauses at least one source |
| off      | Same congestion, PFC disabled on both switches | Fewer than 8,192 packets delivered; no PFC; queues eventually drain                                    |

The negative control must lose packets: otherwise the workload has not shown
that PFC is necessary under these conditions. A run finishing without crashing
is insufficient; each case has automatic assertions and the native test runner reports success only
when its expectations hold. Exact PFC counts and peak occupancies are not fixed
because small timing/model changes can legitimately change them.

Every 100 ns the test checks ingress occupancy equals egress occupancy, the
shared counter equals the sum of per-queue shared usage, per-port headroom is
within its configured allocation, and accounted occupancy is within the switch
buffer. At the end, all data queues and MMU counters must be empty and all MMU
pause flags cleared. Received XOFF/XON counts must balance on each device.
Sampling can miss brief peaks; runtime MMU admission checks provide the
complementary per-packet guards. Control priority 0 is outside MMU accounting.

## Build and run

When this backend is used as the SimAI submodule, run from the enclosing
SimAI repository root on a Linux build host:

```bash
bash scripts/build.sh -c ns3 --ns3-tests --ns3-asserts --sys-asserts

ns-3-alibabacloud/simulation/build/utils/ns3.36.1-test-runner-default \
  --suite=mmu-pfc --verbose --tempdir="$PWD/mmu-pfc-test-output"
```

`--ns3-tests` enables the standard `NS3_TESTS=ON` CMake option and builds the
native test suites and `test-runner`. The suite registers three cases: baseline,
on, and off. ns-3 reports their results and returns nonzero when a check fails.
Production MMU fatal guards still terminate the process on invalid accounting.
Normal builds disable tests. For a Debug build use `-d debug` and the executable
ending in `-debug`. For direct CMake builds enable `NS3_TESTS=ON`, then build
`test-runner`. No custom scratch-test option or backend shell runner is needed.

The native runner's temporary output tree contains per-case `*-occupancy.csv`
and `*-pfc.csv` files. Use a fresh `--tempdir` for each run to retain earlier
results. Redirect stdout/stderr to a file if a persistent console log is needed.
The PFC-disabled case intentionally emits MMU packet-drop diagnostics.

The test keeps case state separate, destroys each simulation, and restores the
INT mode and simulator implementation setting after each case. Occupancy and
PFC trace formats are unchanged; filenames are prefixed by the case name.

Device IDs in pfc.csv: 0=source 0, 1=S0 toward source 0, 2=source 1,
3=S0 toward source 1, 4=S0 toward S1, 5=S1 toward S0, 6=S1 toward sink,
7=sink. Device 4 receiving XOFF is evidence of S1 pausing its adjacent S0;
device 0 or 2 receiving XOFF is the next hop of backpressure.

A positive-case packet deficit indicates admission loss, a stuck pause, or a
routing/delivery error. Unbalanced PFC or nonempty queues indicates failure to
drain before the deadline. The negative case is supposed to report packet loss.
The simulator uses DefaultSimulatorImpl for deterministic single-threaded
execution; it is not a test of concurrent MTP execution.
