#!/usr/bin/env python3
"""Import a vehicle-side walkout session (link_controller `status.jsonl`) into
wfb.sqlite as a `vehicle-uplink` session, so the *uplink* (the vehicle's own
view of the link) sits in the same store as the GS *downlink* capture for the
same walk. They are correlated by the vehicle's monotonic session filename,
recorded in `notes` as `vehicle=<seq>` (see WALKOUT_TELEMETRY_PLAN.md Phase 2).

The vehicle log is 1 Hz link_controller STATE, not wfb_rx `-Y` datagrams, so we
map its fields onto the `records` schema directly (raw_json keeps the full
sample so nothing is lost):

    ts_ms       <- up * 1000               vehicle uptime — monotonic, immune to
                                           the vehicle RTC reset-to-21:14
    mcs         <- status.mcs.current_mcs
    rssi_comb   <- status.score.smoothed_rssi
    rssi_spread <- |smoothed - raw|        (None if either absent)
    per         <- status.score.smoothed_lost_ratio   (already a 0..1 ratio)
    snr_avg / pkt_* / dec_err              None — not available on this side

Time-alignment with the GS session is left to a later step (a one-shot
`(gs_wallclock <-> vehicle_up)` anchor); for now both sessions live in the DB,
tagged, browsable side by side.
"""
from __future__ import annotations
import argparse
import json
import sys
from datetime import datetime, timezone

import wfb_store as store


def _g(d, *path):
    for k in path:
        if not isinstance(d, dict):
            return None
        d = d.get(k)
    return d


def _vehicle_uptime(d):
    """Per-walk time axis. Prefer link_controller's own uptime (status.uptime_s,
    sub-second precise and the axis LOG_SYNC markers anchor to); fall back to the
    walkout logger's integer outer `up` (1 Hz, ±0.5 s) for pre-uptime_s logs."""
    inner = _g(d, "status", "uptime_s")
    if isinstance(inner, (int, float)):
        return float(inner)
    up = d.get("up")
    return float(up) if isinstance(up, (int, float)) else None


def _linfit(xs, ys):
    """Ordinary least-squares slope/intercept for y = a*x + b. Returns (a, b) or
    None if degenerate (fewer than 2 distinct x). Pure stdlib — no numpy."""
    n = len(xs)
    if n < 2:
        return None
    sx = sum(xs); sy = sum(ys)
    sxx = sum(x * x for x in xs); sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if abs(denom) < 1e-9:
        return None
    a = (n * sxy - sx * sy) / denom
    b = (sy - a * sx) / n
    return a, b


def _epoch(iso):
    """ISO8601 (as stored by wfb_store._now) -> unix seconds, or None."""
    if not iso:
        return None
    try:
        return datetime.fromisoformat(iso).timestamp()
    except (ValueError, TypeError):
        return None


def _identify_gs_session(conn, gs_lo, gs_hi):
    """Find the GS capture session active when the walk's markers were emitted.
    Returns (gs_session_id, overlap_seconds) or None.

    Several GS sessions can read as "open" (ended_at NULL) if an earlier ingester
    was orphaned without closing — so >1 may span the marker range. The session
    that was actually live during the walk is the latest-STARTED one whose span
    overlaps the markers; rank by start time, not by overlap width (which ties
    when old open sessions subsume the new one). Falls back to widest overlap."""
    mid = (gs_lo + gs_hi) / 2.0
    contain = []   # (started_epoch, id, overlap) for sessions covering the midpoint
    overlap = []   # (overlap, id) for any session overlapping the range
    for r in conn.execute(
            "SELECT id, started_at, ended_at FROM sessions "
            "WHERE source='live-gs' OR source LIKE 'live-%'"):
        s0 = _epoch(r["started_at"])
        s1 = _epoch(r["ended_at"]) or (gs_hi + 1e9)   # open session: still ongoing
        if s0 is None:
            continue
        ov = min(gs_hi, s1) - max(gs_lo, s0)
        if ov <= 0:
            continue
        overlap.append((ov, r["id"]))
        if s0 <= mid <= s1:
            contain.append((s0, r["id"], ov))
    if contain:
        s0, sid, ov = max(contain)       # latest-started session live at the walk
        return (sid, ov)
    if overlap:
        ov, sid = max(overlap)
        return (sid, ov)
    return None


def import_vehicle_jsonl(conn, path: str, vehicle_session: str, **meta) -> tuple[int, int]:
    notes = f"vehicle={vehicle_session}"
    extra = meta.pop("notes", None)   # always remove — create_session takes notes= explicitly
    if extra:
        notes += f" · {extra}"

    # Pass 1: read the walk into memory (a walk is ~a few thousand 1 Hz lines)
    # and collect LOG_SYNC markers — one (recv_uptime_s, gs_unix_s) anchor per
    # distinct seq, taken at first sight (closest to actual receipt).
    rows = []
    markers: dict[int, tuple[float, int]] = {}
    bad = 0
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                bad += 1
                continue
            ut = _vehicle_uptime(d)
            if ut is None:
                bad += 1   # no time axis — skip, don't pile rows at t=0
                continue
            rows.append((ut, d))
            ls = _g(d, "status", "logsync")
            if isinstance(ls, dict):
                seq = ls.get("seq")
                gs = ls.get("gs_unix_s")
                ruts = ls.get("recv_uptime_s")
                anchor = ruts if isinstance(ruts, (int, float)) else ut
                if isinstance(seq, int) and isinstance(gs, (int, float)) \
                        and seq not in markers:
                    markers[seq] = (float(anchor), int(gs))

    # Fit gs_unix = a*uptime + b from the marker anchors, then identify which
    # GS session this walk belongs to by the fitted wall-clock span.
    fit = align_note = None
    gs_sid = None
    if len(markers) >= 2:
        anchors = sorted(markers.values())
        fit = _linfit([a for a, _ in anchors], [g for _, g in anchors])
    if fit:
        a, b = fit
        # Fit confidence: residual of each marker's gs_unix vs the fitted line.
        # gs_unix_s is whole seconds (the GS sends time(NULL)), so a clean fit
        # still shows ~0.5-1.5 s residual from that quantization + uplink jitter.
        # A large max residual (>~3 s) means the anchors disagree with a single
        # line — markers from different walks mixed in, or a clock discontinuity.
        resid = [g - (a * ut + b) for ut, g in anchors]
        max_resid = max(abs(r) for r in resid)
        rms_resid = (sum(r * r for r in resid) / len(resid)) ** 0.5
        quality = "ok" if max_resid <= 3.0 else "SUSPECT"
        ups = [ut for ut, _ in rows]
        gs_lo, gs_hi = a * min(ups) + b, a * max(ups) + b
        ident = _identify_gs_session(conn, gs_lo, gs_hi)
        gs_sid = ident[0] if ident else None
        align_note = (f"aligned a={a:.6f} b={b:.0f} markers={len(markers)} "
                      f"resid={rms_resid:.1f}s_rms/{max_resid:.1f}s_max[{quality}]"
                      + (f" gs_session={gs_sid}" if gs_sid else " gs_session=?"))
        notes += f" · {align_note}"

    sid = store.create_session(conn, "vehicle-uplink", notes=notes, **meta)
    for ut, d in rows:
        ts_ms = int(ut * 1000)
        rssi = _g(d, "status", "score", "smoothed_rssi")
        raw = _g(d, "status", "score", "raw_rssi")
        spread = abs(rssi - raw) if isinstance(rssi, (int, float)) and isinstance(raw, (int, float)) else None
        per = _g(d, "status", "score", "smoothed_lost_ratio")
        mcs = _g(d, "status", "mcs", "current_mcs")
        # Vehicle-side UPLINK reception (GS->vehicle) — genuinely independent
        # antenna data from rssi_comb above (which is the GS-relayed DOWNLINK
        # score). present=false until the vehicle's wfb_rx -Y has been seen.
        up = _g(d, "status", "uplink_rx")
        if isinstance(up, dict) and up.get("present"):
            uplink_rssi = up.get("smoothed_rssi")
            uplink_pkt = up.get("pkt_last")
            uplink_lost = up.get("lost_last")
        else:
            uplink_rssi = uplink_pkt = uplink_lost = None
        # Stamp the GS wall-clock estimate into raw_json (no schema change) so a
        # future overlay can place vehicle + GS records on one epoch axis.
        if fit:
            d = dict(d)
            d["gs_unix_ms"] = round((fit[0] * ut + fit[1]) * 1000.0)
        conn.execute(
            """INSERT INTO records
               (session_id, ts_ms, seq, mcs, rssi_comb, rssi_spread, snr_avg,
                pkt_all, pkt_uniq, pkt_lost, fec_rec, dec_err, per,
                uplink_rssi, uplink_pkt, uplink_lost, raw_json)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (sid, ts_ms, None, mcs, rssi, spread, None,
             None, None, None, None, None, per,
             uplink_rssi, uplink_pkt, uplink_lost,
             json.dumps(d, separators=(",", ":"))))
    store.close_session(conn, sid)
    if bad:
        print(f"[import-vehicle] skipped {bad} unparseable lines", file=sys.stderr)
    if align_note:
        print(f"[import-vehicle] {align_note}", file=sys.stderr)
    elif markers:
        print(f"[import-vehicle] {len(markers)} LOG_SYNC marker(s) — need ≥2 to fit",
              file=sys.stderr)
    else:
        print("[import-vehicle] no LOG_SYNC markers (pre-LOG_SYNC walk) — "
              "not time-aligned to a GS session", file=sys.stderr)
    return sid, len(rows)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jsonl", help="vehicle status.jsonl path")
    ap.add_argument("--db", default=store.DEFAULT_DB)
    ap.add_argument("--vehicle-session", required=True,
                    help="vehicle session id/filename, e.g. 000002 (the correlation key)")
    for f in store.META_FIELDS:
        if f != "notes":
            ap.add_argument(f"--{f.replace('_', '-')}", dest=f, default=None)
    ap.add_argument("--notes", default=None)
    args = ap.parse_args()

    conn = store.connect(args.db)
    store.init_db(conn)
    meta = {f: getattr(args, f) for f in store.META_FIELDS}
    sid, n = import_vehicle_jsonl(conn, args.jsonl, args.vehicle_session, **meta)
    conn.commit()
    conn.close()
    print(f"[import-vehicle] session {sid}: {n} records (vehicle={args.vehicle_session}) -> {args.db}")


if __name__ == "__main__":
    main()
