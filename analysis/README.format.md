# Trace Output Format

This document describes the line format printed by the `trace_reader`.
Each log line corresponds to a traced packet, whose fields depend on the Layer 3 type.

All lines begin with core fields identifying timestamp, node, interface, queue, and event metadata.

***

### Common Fields (present in most cases)

| Field       | Description                                  |
|-------------|----------------------------------------------|
| `time`      | Simulation timestamp (nanoseconds).          |
| `n:``node`  | ID of the node.                              |
| `intf:qidx` | Interface index and queue index.             |
| `qlen`      | Current queue length.                        |
| `event`     | Event type (`Recv`, `Enqu`, `Dequ`, `Drop`). |
| `ecn`       | ECN bits (Explicit Congestion Notification). |
| `sip`       | Source IP address in hexadecimal.            |
| `dip`       | Destination IP address in hexadecimal.       |

***

### Case T (TCP) and U (UDP)

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <sport> <dport> <proto> <seq> <ts> <pg> <size> (<payload>)
```

| Field             | Description                      |
|-------------------|----------------------------------|
| `sport` / `dport` | Source / destination ports.      |
| `proto`           | Protocol character (`T` or `U`). |
| `seq`             | Sequence number.                 |
| `ts`              | Tx timestamp.                    |
| `pg`              | Priority group.                  |
| `size`            | Packet size in bytes.            |
| `payload`         | Payload size.                    |

***

### Case A (ACK)

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <sport> <dport> <proto> 0x<flags> <pg> <seq> <ts> <size>
```

| Field             | Description                 |
|-------------------|-----------------------------|
| `sport` / `dport` | Source / destination ports. |
| `proto`           | Protocol character (`A`).   |
| `flags`           | ACK flags field (bitmask).  |
| `pg`              | Priority group.             |
| `seq`             | Sequence number.            |
| `ts`              | Tx timestamp.               |

***

### Case N (NACK)

Format identical to ACK, implying a negative acknowledgment message (protocol character is `N`).

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <sport> <dport> <proto> 0x<flags> <pg> <seq> <ts> <size>
```

***

### Case P (PFC — Priority Flow Control)

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <proto> <pfc.time> <pfc.qlen> <pfc.qIndex> <size>
```

| Field        | Description                  |
|--------------|------------------------------|
| `proto`      | Protocol character (`P`).    |
| `pfc.time`   | Pause duration.              |
| `pfc.qlen`   | Queue length triggering PFC. |
| `pfc.qIndex` | Paused queue index.          |
| `size`       | Packet size in bytes.        |

***

### Case C (CNP — Congestion Notification Packet)

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <proto> <fid> <qIndex> <ecnBits> <seq> <size>
```

| Field     | Description                          |
|-----------|--------------------------------------|
| `proto`   | Protocol character (`C`).            |
| `fid`     | Flow ID associated with the CNP.     |
| `qIndex`  | Queue index of the congestion event. |
| `ecnBits` | ECN marking received.                |
| `seq`     | Affected sequence number.            |

> **TODO:** Doublecheck.

***

### QpAv — Queue Pair Available

```
<time> n:<node> <intf>:<qidx> <event> <sip> <dip> <sport> <dport>
```

| Field             | Description                                              |
|-------------------|----------------------------------------------------------|
| `sport` / `dport` | Ports associated with the queue pair availability event. |

***

### Unknown Protocols

```
<time> n:<node> <intf>:<qidx> <qlen> <event> ecn:<ecn> <sip> <dip> <proto> <size>
```

Used for unrecognized or custom Layer 3 protocol types.

| Field   | Description              |
|---------|--------------------------|
| `proto` | Hexadecimal protocol ID. |
| `size`  | Packet size.             |

***
