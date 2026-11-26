#!/usr/bin/env python3
"""
trace_gap_analyzer.py

Analyze a packet-level simulator trace and report time gaps larger than a
threshold along the flows that connect two hosts through a switch.

By default the analysis stops at the first finished chunk -- the first
non-full data packet (a U packet whose payload is smaller than a full MTU),
which is the tail of the first message. Use --whole-trace to analyze the
entire window instead.

Trace line format (whitespace separated), e.g.:

    87 n:16 7:3 0 Enqu ecn:0 0b000501 0b000601 10000 100 U 0 0 3 9052(9000)
    |  |    |   | |    |     |        |        |     |   | ............ |
    |  |    |   | |    |     src      dst      sport dpt type          size
    |  |    |   | event ecn
    |  |    port:queue (and a running byte counter after it)
    |  device  (n:<rank>)
    time

ACK lines use type 'A' and a trailing scalar size (e.g. '... 0 60' -> 60 bytes).

Node rank is recovered from the address as (int(addr,16) >> 8) & 0xff
so that 0b000101 -> 1, 0b000001 -> 0, 0b000901 -> 9, etc.

Usage:
    python3 trace_gap_analyzer.py TRACEFILE --threshold T --hostA A --hostB B --switch S
    cat trace.txt | python3 trace_gap_analyzer.py - --threshold T --hostA A --hostB B --switch S

Useful options:
    --event {Dequ,Enqu,Recv}   which event marks "the packet left/arrived" (default Dequ)
    --type  {U,A,both}         data only (U), ack only (A), or both (default U)
    --all-flows                also print the gap report for every flow found, not just A<->B
    --top N                    show only the N largest gaps per stream (default: all)
    --full-payload N           payload bytes of a full data packet (0 = auto-detect)
    --whole-trace              analyze all gaps instead of stopping at the first chunk
"""

import argparse
import sys
from collections import defaultdict, OrderedDict

EVENTS = ("Enqu", "Dequ", "Recv")


def addr_to_node(a):
    """Recover node rank from a hex-ish address string like '0b000101'."""
    try:
        v = int(a, 16)
    except ValueError:
        return None
    return (v >> 8) & 0xff


def parse_size(tok):
    """Last token is either '9052(9000)' (data) or a plain integer (ack)."""
    if "(" in tok:
        tok = tok.split("(", 1)[0]
    try:
        return int(tok)
    except ValueError:
        return None


def parse_payload(tok):
    """Data payload bytes: the value in parentheses ('9052(9000)' -> 9000).
    Returns None for tokens with no parens (e.g. a plain ack size), since
    those are not full/partial data packets."""
    if "(" in tok and ")" in tok:
        inner = tok[tok.index("(") + 1:tok.index(")")]
        try:
            return int(inner)
        except ValueError:
            return None
    return None


def parse_line(line):
    t = line.split()
    if len(t) < 8:
        return None
    # time
    try:
        time = int(t[0])
    except ValueError:
        return None
    # device n:<rank>
    if not t[1].startswith("n:"):
        return None
    try:
        dev = int(t[1][2:])
    except ValueError:
        return None
    # port:queue
    pq = t[2]
    if ":" in pq:
        port_s, queue = pq.split(":", 1)
    else:
        port_s, queue = pq, ""
    try:
        port = int(port_s)
    except ValueError:
        port = port_s
    # event must be one of EVENTS (normally token index 4)
    event = t[4] if len(t) > 4 else None
    if event not in EVENTS:
        # fall back: search for it
        event = next((x for x in t if x in EVENTS), None)
        if event is None:
            return None
    # locate ecn token; src/dst follow it. Fast path: it is normally t[5].
    if len(t) > 5 and t[5].startswith("ecn:"):
        eidx = 5
    else:
        eidx = None
        for i, x in enumerate(t):
            if x.startswith("ecn:"):
                eidx = i
                break
    if eidx is None or eidx + 2 >= len(t):
        return None
    src, dst = t[eidx + 1], t[eidx + 2]
    sport = t[eidx + 3] if eidx + 3 < len(t) else None
    dport = t[eidx + 4] if eidx + 4 < len(t) else None
    # packet type: first 'U' or 'A' after dst
    ptype = next((x for x in t[eidx + 3:] if x in ("U", "A")), None)
    last = t[-1]
    return {
        "time": time, "dev": dev, "port": port, "queue": queue,
        "event": event, "src": src, "dst": dst,
        "srcn": addr_to_node(src), "dstn": addr_to_node(dst),
        "sport": sport, "dport": dport, "ptype": ptype,
        "size": parse_size(last), "payload": parse_payload(last),
        "raw": line.rstrip("\n"),
    }


def load(fp):
    events = []
    for line in fp:
        e = parse_line(line)
        if e is not None:
            events.append(e)
    return events


def consecutive_gaps(stream, threshold):
    """stream: list of event dicts. Returns (gaps_over_threshold, sorted_stream)."""
    s = sorted(stream, key=lambda e: e["time"])
    over = []
    for prev, cur in zip(s, s[1:]):
        g = cur["time"] - prev["time"]
        if g > threshold:
            over.append((prev, cur, g))
    return over, s


def first_chunk(stream, full_payload):
    """Truncate a stream at the first finished chunk.

    Sorts by time and returns the prefix up to *and including* the first
    non-full data packet (a U packet whose payload < full_payload). That
    packet is the tail of the first message/chunk, so from the flow's point
    of view the chunk is "done" there.

    Returns (prefix, found). If full_payload is None or no non-full data
    packet exists, returns the whole sorted stream with found=False.
    """
    s = sorted(stream, key=lambda e: e["time"])
    if full_payload is None:
        return s, False
    for i, e in enumerate(s):
        if (e["ptype"] == "U" and e["payload"] is not None
                and e["payload"] < full_payload):
            return s[:i + 1], True
    return s, False


def stream_stats(s):
    """Summary stats for a time-sorted stream."""
    if not s:
        return None
    times = [e["time"] for e in s]
    span = times[-1] - times[0]
    diffs = [b - a for a, b in zip(times, times[1:])]
    nbytes = sum(e["size"] or 0 for e in s)
    st = {
        "n": len(s), "first": times[0], "last": times[-1], "span": span,
        "bytes": nbytes,
        "min_gap": min(diffs) if diffs else None,
        "max_gap": max(diffs) if diffs else None,
        "mean_gap": (sum(diffs) / len(diffs)) if diffs else None,
        "throughput": (nbytes / span) if span else None,
    }
    # excess idle = total time beyond the back-to-back (min-gap) serialization.
    # This is the link/flow time that a perfectly pipelined (e.g. multi-QP)
    # transfer would NOT have spent idle.
    if diffs:
        mg = min(diffs)
        st["excess_idle"] = sum(d - mg for d in diffs)
    else:
        st["excess_idle"] = 0
    return st


def fmt_ev(e):
    q = f"{e['port']}:{e['queue']}" if e["queue"] != "" else str(e["port"])
    return (f"t={e['time']:<8} n:{e['dev']:<3} {q:<6} {e['event']:<4} "
            f"{e['srcn']}->{e['dstn']} {e['ptype']} sz={e['size']} "
            f"(sport={e['sport']} dport={e['dport']})")


def report_stream(title, stream, threshold, top, full_payload=None,
                  whole_trace=False):
    print(f"\n  --- {title} ---")
    s = sorted(stream, key=lambda e: e["time"])
    if not whole_trace and full_payload is not None:
        prefix, found = first_chunk(s, full_payload)
        if found:
            last = prefix[-1]
            print(f"    [first chunk ends at t={last['time']}: non-full data "
                  f"packet payload={last['payload']} < {full_payload}; "
                  f"analyzing {len(prefix)} of {len(s)} events]")
            s = prefix
        else:
            print(f"    [no non-full data packet found; chunk not finished in "
                  f"this window -> analyzing whole stream]")
    over, s = consecutive_gaps(s, threshold)
    st = stream_stats(s)
    if st is None:
        print("    (no events)")
        return st
    print(f"    packets={st['n']}  bytes={st['bytes']}  "
          f"window=[{st['first']}..{st['last']}] span={st['span']}")
    if st["min_gap"] is not None:
        print(f"    inter-packet gap: min={st['min_gap']} "
              f"mean={st['mean_gap']:.1f} max={st['max_gap']}")
        print(f"    excess idle (beyond min-gap serialization) = "
              f"{st['excess_idle']}   "
              f"throughput = {st['throughput']:.2f} bytes/time")
    if not over:
        print(f"    >>> no gaps over threshold {threshold}")
    else:
        shown = sorted(over, key=lambda x: -x[2])
        if top:
            shown = shown[:top]
        print(f"    >>> {len(over)} gap(s) over threshold {threshold} "
              f"(showing {len(shown)}):")
        for prev, cur, g in shown:
            print(f"        GAP {g:<8} between t={prev['time']} and "
                  f"t={cur['time']}")
            print(f"            before: {fmt_ev(prev)}")
            print(f"            after : {fmt_ev(cur)}")
    return st


def main():
    ap = argparse.ArgumentParser(
        description="Find time gaps over a threshold along the directional "
                    "host-A -> host-B flow through a switch.")
    ap.add_argument("tracefile", help="trace file path, or '-' for stdin")
    ap.add_argument("--threshold", type=float, required=True,
                    help="report inter-packet time gaps strictly greater than this")
    ap.add_argument("--hostA", type=int, required=True)
    ap.add_argument("--hostB", type=int, required=True)
    ap.add_argument("--switch", type=int, required=True)
    ap.add_argument("--event", choices=EVENTS, default="Dequ",
                    help="event used as the transmit/arrival marker (default Dequ)")
    ap.add_argument("--type", choices=["U", "A", "both"], default="U",
                    help="packet type to analyze: U=data, A=ack, both (default U)")
    ap.add_argument("--all-flows", action="store_true",
                    help="also report gaps for every flow in the trace")
    ap.add_argument("--both-dirs", action="store_true",
                    help="also analyze the reverse direction B -> A "
                         "(default: only the directional A -> B flow)")
    ap.add_argument("--top", type=int, default=0,
                    help="show only the N largest gaps per stream (0 = all)")
    ap.add_argument("--full-payload", type=int, default=0,
                    help="payload size (bytes) of a full data packet; "
                         "0 = auto-detect as the max U payload in the trace. "
                         "A data packet smaller than this ends a chunk.")
    ap.add_argument("--whole-trace", action="store_true",
                    help="analyze every gap in the window instead of stopping "
                         "at the first finished chunk (the default)")
    args = ap.parse_args()

    fp = sys.stdin if args.tracefile == "-" else open(args.tracefile)
    events = load(fp)
    if fp is not sys.stdin:
        fp.close()

    A, B, S = args.hostA, args.hostB, args.switch
    top = args.top or None
    type_ok = (lambda p: True) if args.type == "both" else (lambda p, t=args.type: p == t)

    # ---- index events once + detect full payload -------------------------
    # Without the index, every (flow x device) stream re-scanned the whole
    # event list -> O(flows * devices * N). The index makes each stream a
    # dict lookup over only the matching events. The max-payload scan rides
    # along on the same pass, so chunk detection costs no extra traversal.
    index = defaultdict(list)
    max_payload = None
    for e in events:
        if e["srcn"] is None or e["dstn"] is None or e["ptype"] is None:
            continue
        index[(e["srcn"], e["dstn"], e["dev"], e["event"])].append(e)
        if e["ptype"] == "U" and e["payload"] is not None:
            if max_payload is None or e["payload"] > max_payload:
                max_payload = e["payload"]
    full_payload = args.full_payload or max_payload

    print("=" * 78)
    print(f"Parsed {len(events)} trace events")
    print(f"hostA={A}  hostB={B}  switch={S}  threshold={args.threshold}  "
          f"event={args.event}  type={args.type}")
    if args.whole_trace:
        print("scope=whole-trace (analyzing all gaps in the window)")
    else:
        print(f"scope=first-chunk (stop at first non-full data packet; "
              f"full_payload={full_payload})")
    print("=" * 78)

    # ---- flow inventory --------------------------------------------------
    flows = defaultdict(lambda: {"U": 0, "A": 0, "bytes": 0,
                                 "first": None, "last": None})
    for e in events:
        if e["srcn"] is None or e["dstn"] is None:
            continue
        # count each (src,dst) once per packet using the Dequ at the source,
        # but for inventory just count Recv at destination to avoid double count.
        key = (e["srcn"], e["dstn"])
        f = flows[key]
        if e["ptype"] in ("U", "A"):
            if e["event"] == "Recv":
                f[e["ptype"]] += 1
                f["bytes"] += e["size"] or 0
            f["first"] = e["time"] if f["first"] is None else min(f["first"], e["time"])
            f["last"] = e["time"] if f["last"] is None else max(f["last"], e["time"])

    print("\nFLOW INVENTORY (counts from Recv events)")
    print(f"  {'src->dst':<10} {'#data':>6} {'#ack':>6} {'bytes':>10} "
          f"{'first':>8} {'last':>8}")
    for (s, d), f in sorted(flows.items()):
        mark = ""
        if (s, d) == (A, B):
            mark = "  <== A->B"
        elif (s, d) == (B, A):
            mark = "  <== B->A (reverse)"
        print(f"  {s}->{d:<7} {f['U']:>6} {f['A']:>6} {f['bytes']:>10} "
              f"{str(f['first']):>8} {str(f['last']):>8}{mark}")

    # ---- which flows to deep-dive ---------------------------------------
    # "A -> B" is directional: by default only the A-to-B flow. --both-dirs
    # adds the reverse (B -> A) as a separate report.
    ab_flows = [(A, B)]
    if args.both_dirs:
        ab_flows.append((B, A))
    ab_flows = [(s, d) for (s, d) in ab_flows if s != d]
    deep = ab_flows if not args.all_flows else sorted(
        (k for k in flows if k[0] != k[1]))

    devices = [("hostA(%d)" % A, A), ("switch(%d)" % S, S), ("hostB(%d)" % B, B)]

    for (s, d) in deep:
        print("\n" + "#" * 78)
        print(f"# FLOW {s} -> {d}   (analyzing event '{args.event}', type {args.type})")
        print("#" * 78)
        stage_idle = OrderedDict()
        for label, dev in devices:
            stream = [e for e in index.get((s, d, dev, args.event), ())
                      if type_ok(e["ptype"])]
            st = report_stream(f"{label}  [{args.event}]", stream,
                               args.threshold, top,
                               full_payload=full_payload,
                               whole_trace=args.whole_trace)
            if st:
                stage_idle[label] = st["excess_idle"]

        # arrival view at the destination is a Recv, which --event Dequ misses
        if args.event != "Recv":
            recv_stream = [e for e in index.get((s, d, d, "Recv"), ())
                           if type_ok(e["ptype"])]
            st = report_stream(f"hostB(%d)  [arrival/Recv]" % d, recv_stream,
                               args.threshold, top,
                               full_payload=full_payload,
                               whole_trace=args.whole_trace)
            if st:
                stage_idle["arrival@%d" % d] = st["excess_idle"]

        if stage_idle:
            worst = max(stage_idle, key=lambda k: stage_idle[k])
            print(f"\n  DIAGNOSIS: most idle (under-utilization) accumulates at "
                  f"'{worst}'  (excess-idle breakdown: "
                  + ", ".join(f"{k}={v}" for k, v in stage_idle.items()) + ")")
            print("  -> a multi-QP transfer fills these idle gaps with other QPs'"
                  " packets;\n     a single-QP 512KB transfer leaves them idle, so it"
                  " finishes later.")


if __name__ == "__main__":
    main()