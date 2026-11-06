#!/usr/bin/env python3
"""Closed-loop time of the PRB block: gNB capture -> dApp -> E3Controller -> gNB scheduler.

Reads the latrec rings of one run, the controller's and the dApp's, and joins
them into one row per PRB block control (Spectrum SM, RAN function 1, control 1):

  T0  RECORD_BEGIN        gNB hook of the uplink slot the dApp decided on
                          (controller ring e3controller.l1_kpm, t = gnb_ts_ns, aux = sfn<<16|slot)
  T1  PROCESS_DONE        the dApp's decision for that slot           (dApp, seq = slot key)
  T2  SEND_DONE           the control left the dApp                   (dApp, seq = E3 message id)
  T3  RECV                the controller received it                  (controller libe3.inbound)
      DECODE_E3SM_DONE    decoded by the Spectrum SM                  (controller libe3.inbound)
  T4  APPLY_CONTROL_DONE  handed to the gNB (control input block_in)  (controller e3controller.prb_block)
  T5  LIVE_ON_AIR         the first scheduler slot that applies it    (controller e3controller.prb_block_gnb,
                          back-dated to the codelet's time, aux = that slot's sfn<<16|slot)
      ACK_RECV            the dApp got the ACK                        (dApp, seq = E3 message id)

All stamps are CLOCK_MONOTONIC on one host, so the segments are plain
differences. The loop is also given in air slots: from the slot the dApp read
(T0's slot) to the first slot the scheduler applies the block to (T5's slot).
That slot's DL goes on air with the block. A UL grant made in it is for k2
slots later, so UL is under the block from the first UL slot at least k2 slots
after it; the UL slots are the ones the gNB captured.

The dApp's records are read with its E3 backend in mind: the native backend
stamps EMIT_ENTER and SEND_DONE with seq = the E3 message id and EMIT_ENTER.aux
= the slot key, which is what links a control to its slot.

Usage:
  latrec_closed_loop.py --ctrl <controller latrec_dir> --dapp <dApp latrec_dir> [--csv out.csv]

Standard library only. Uses libe3's tools/latrec_reader.py, found next to this
repository's libe3 submodule or with --reader.
"""
import argparse
import bisect
import csv
import os
import sys
from collections import Counter, defaultdict

STAGE = {
    "RECORD_BEGIN": 0x10,
    "DECODE_E3SM_BEGIN": 0x1A,
    "DECODE_E3SM_DONE": 0x1B,
    "EMIT_ENTER": 0x20,
    "DEQUEUE": 0x22,
    "SEND_DONE": 0x24,
    "RECV": 0x25,
    "DECODE_E3AP_DONE": 0x26,
    "PROCESS_DONE": 0x40,
    "APPLY_CONTROL_DONE": 0x48,
    "LIVE_ON_AIR": 0x49,
    "ACK_SENT": 0x4A,
    "ACK_RECV": 0x4B,
}
PDU_DAPP_CONTROL_ACTION = 6

SEGMENTS = [
    # name, from, to
    ("hook_to_decision_us", "t0", "t1"),
    ("decision_to_send_us", "t1", "t2"),
    ("send_to_ctrl_rx_us", "t2", "t3"),
    ("ctrl_rx_to_decoded_us", "t3", "t3d"),
    ("decoded_to_gnb_us", "t3d", "t4"),
    ("gnb_to_applied_us", "t4", "t5"),
    ("loop_us", "t0", "t5"),
    ("decision_to_applied_us", "t1", "t5"),
    ("send_to_ack_us", "t2", "tack"),
]


def find_reader(explicit):
    here = os.path.dirname(os.path.abspath(__file__))
    for d in [explicit, os.path.join(here, "..", "libe3", "tools"), "/usr/local/share/libe3/tools"]:
        if d and os.path.isfile(os.path.join(d, "latrec_reader.py")):
            sys.path.insert(0, d)
            import latrec_reader  # noqa: E402
            return latrec_reader
    sys.exit("latrec_reader.py not found; pass --reader <libe3/tools>")


def role(ring):
    """The ring's role: its name without the trailing .<tid>, if any."""
    head, _, tail = ring.name.rpartition(".")
    return head if tail.isdigit() else ring.name


def load(reader, path, label):
    rings = reader.read_dir(path)
    if not rings:
        sys.exit(f"no latrec rings in {path}")
    for r in rings:
        if r.wrapped or r.lost_records:
            print(f"WARNING {label} ring {r.name}: wrapped, {r.lost_records} records lost; "
                  "spans across the wrap are wrong", file=sys.stderr)
    return rings


def slot_diff(later, earlier, slots_per_frame):
    """Slots from `earlier` to `later` (sfn<<16|slot keys), modulo the 1024-frame SFN cycle."""
    period = 1024 * slots_per_frame
    a = (later >> 16) * slots_per_frame + (later & 0xFFFF)
    b = (earlier >> 16) * slots_per_frame + (earlier & 0xFFFF)
    return (a - b) % period


def slots_to_ul(key, k2, ul_slots, slots_per_frame):
    """Slots from `key` to the first UL slot at least k2 slots later: the first PUSCH a grant made in `key` can have."""
    for d in range(k2, k2 + slots_per_frame):
        if ((key & 0xFFFF) + d) % slots_per_frame in ul_slots:
            return d
    return None


def air_counts(slots, slot_us):
    """'7 slots (3.50 ms) x297, 8 slots (4.00 ms) x3'."""
    return ", ".join(f"{n} slots ({n * slot_us / 1000:.2f} ms) x{c}" for n, c in sorted(Counter(slots).items()))


def sfn_slot(key):
    """sfn<<16|slot as "sfn.slot"."""
    return "" if key is None else f"{key >> 16}.{key & 0xFFFF}"


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ctrl", required=True, help="the controller's logging.latrec_dir")
    ap.add_argument("--dapp", required=True, help="the dApp's e3_manager.latrec_dir")
    ap.add_argument("--csv", help="write one row per control here")
    ap.add_argument("--reader", help="directory of libe3's latrec_reader.py")
    ap.add_argument("--slots-per-frame", type=int, default=20, help="20 at 30 kHz SCS")
    ap.add_argument("--slot-us", type=float, default=500.0, help="slot duration, 500 at 30 kHz SCS")
    ap.add_argument("--k2-min", type=int, default=4, help="the scheduler's smallest k2 (ocudu min_k2, 4)")
    args = ap.parse_args()

    reader = find_reader(args.reader)
    ctrl = load(reader, args.ctrl, "controller")
    dapp = load(reader, args.dapp, "dApp")

    # ---- controller ----
    record_begin = defaultdict(list)  # slot key -> [t_ns], T0 candidates
    ctrl_rx = {}                      # message id -> RECV t_ns
    sm = defaultdict(dict)            # message id -> {stage name: (t_ns, aux, aux2)}
    for r in ctrl:
        rl = role(r)
        if rl == "e3controller.l1_kpm":
            for rec in r.records:
                if rec.stage == STAGE["RECORD_BEGIN"]:
                    record_begin[rec.aux].append(rec.t_ns)
        elif rl == "libe3.inbound":
            recv = {}
            for rec in r.records:
                if rec.stage == STAGE["RECV"]:
                    recv[rec.seq] = rec.t_ns
                elif rec.stage == STAGE["DECODE_E3AP_DONE"] and rec.aux2 == PDU_DAPP_CONTROL_ACTION:
                    if rec.seq in recv:
                        ctrl_rx[rec.aux] = recv[rec.seq]
                elif rec.stage in (STAGE["DECODE_E3SM_BEGIN"], STAGE["DECODE_E3SM_DONE"], STAGE["ACK_SENT"]):
                    sm[rec.seq][reader.STAGES[rec.stage]] = (rec.t_ns, rec.aux, rec.aux2)
        elif rl in ("e3controller.prb_block", "e3controller.prb_block_gnb"):
            for rec in r.records:
                if rec.stage in (STAGE["DEQUEUE"], STAGE["APPLY_CONTROL_DONE"], STAGE["ACK_SENT"],
                                 STAGE["LIVE_ON_AIR"]):
                    sm[rec.seq][reader.STAGES[rec.stage]] = (rec.t_ns, rec.aux, rec.aux2)
    for v in record_begin.values():
        v.sort()
    ul_slots = {key & 0xFFFF for key in record_begin}  # the gNB captures UL slots only

    # ---- dApp ----
    decision = defaultdict(list)  # slot key -> [t_ns of PROCESS_DONE]
    emit = {}                     # message id -> (t_ns, slot key)
    sent = {}                     # message id -> t_ns
    ack_recv = {}                 # message id -> t_ns
    for r in dapp:
        for rec in r.records:
            if rec.stage == STAGE["PROCESS_DONE"]:
                decision[rec.seq].append(rec.t_ns)
            elif rec.stage == STAGE["EMIT_ENTER"]:
                emit[rec.seq] = (rec.t_ns, rec.aux)
            elif rec.stage == STAGE["SEND_DONE"]:
                sent[rec.seq] = rec.t_ns
            elif rec.stage == STAGE["ACK_RECV"]:
                ack_recv[rec.seq] = rec.t_ns
    for v in decision.values():
        v.sort()

    # ---- one row per control the controller handed to the gNB, or rejected ----
    rows = []
    for mid in sorted(sm):
        st = sm[mid]
        if "DECODE_E3SM_BEGIN" not in st:
            continue  # not a Spectrum control (another SM's sequence space)
        row = {"msg_id": mid, "n_prbs": st.get("DECODE_E3SM_DONE", (0, "", 0))[1]}
        row["t3d"] = st.get("DECODE_E3SM_DONE", (None,))[0]
        row["t4"] = st.get("APPLY_CONTROL_DONE", (None,))[0]
        row["handed_ok"] = st.get("APPLY_CONTROL_DONE", (0, ""))[1]
        row["t3"] = ctrl_rx.get(mid)
        live = st.get("LIVE_ON_AIR")
        row["t5"] = live[0] if live else None
        row["applied_slot"] = live[1] if live else None
        row["coalesced"] = live[2] if live else None
        row["t2"] = sent.get(mid)
        row["tack"] = ack_recv.get(mid)
        slot_key = emit[mid][1] if mid in emit else None
        row["slot_key"] = slot_key
        row["t1"] = row["t0"] = None
        if slot_key is not None and mid in emit:
            ts = decision.get(slot_key, [])
            i = bisect.bisect_right(ts, emit[mid][0]) - 1
            if i >= 0:
                row["t1"] = ts[i]
                rb = record_begin.get(slot_key, [])
                j = bisect.bisect_right(rb, row["t1"]) - 1
                # the slot key wraps every 10.24 s: take the capture just before the decision
                if j >= 0 and row["t1"] - rb[j] < 5_000_000_000:
                    row["t0"] = rb[j]
        for name, a, b in SEGMENTS:
            row[name] = (row[b] - row[a]) / 1000.0 if row.get(a) is not None and row.get(b) is not None else None
        row["loop_air_slots"] = row["ul_air_slots"] = None
        if row["applied_slot"] is not None and slot_key is not None:
            row["loop_air_slots"] = slot_diff(row["applied_slot"], slot_key, args.slots_per_frame)
            to_ul = slots_to_ul(row["applied_slot"], args.k2_min, ul_slots, args.slots_per_frame)
            if to_ul is not None:
                row["ul_air_slots"] = row["loop_air_slots"] + to_ul
        rows.append(row)

    if not rows:
        sys.exit("no PRB block control in the controller's rings (built with --latrec? prb_block.enabled?)")

    applied = [r for r in rows if r["t5"] is not None]
    print(f"controls {len(rows)}: applied {len(applied)}, superseded or not handed off {len(rows) - len(applied)}, "
          f"joined to their slot {sum(1 for r in rows if r['t0'] is not None)}")
    print(f"{'segment':<26}{'n':>6}{'p50':>10}{'p90':>10}{'p99':>10}{'max':>10}   (us)")
    for name, _, _ in SEGMENTS:
        vals = [r[name] for r in rows if r[name] is not None]
        print(f"{name:<26}{len(vals):>6}{pct(vals, 50):>10.1f}{pct(vals, 90):>10.1f}{pct(vals, 99):>10.1f}"
              f"{(max(vals) if vals else float('nan')):>10.1f}")
    air = [r["loop_air_slots"] for r in rows if r["loop_air_slots"] is not None]
    ul = [r["ul_air_slots"] for r in rows if r["ul_air_slots"] is not None]
    if air:
        print(f"\nair slots from the slot read to the first slot under the block ({args.slot_us:.0f} us each):")
        print(f"  DL  {air_counts(air, args.slot_us)}")
    if ul:
        print(f"  UL  {air_counts(ul, args.slot_us)}")
        print(f"      (first UL slot at least k2 = {args.k2_min} slots after the DL one; "
              f"UL slots {','.join(str(s) for s in sorted(ul_slots))} = the captured ones)")

    if args.csv:
        cols = ["msg_id", "slot", "n_prbs", "handed_ok", "applied_slot", "coalesced", "loop_air_slots",
                "ul_air_slots"] + \
               [s[0] for s in SEGMENTS] + ["t0", "t1", "t2", "t3", "t3d", "t4", "t5", "tack"]
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                out = dict(r)
                out["slot"] = sfn_slot(r["slot_key"])
                out["applied_slot"] = sfn_slot(r["applied_slot"])
                w.writerow(out)
        print(f"\nwrote {args.csv}")


if __name__ == "__main__":
    main()
