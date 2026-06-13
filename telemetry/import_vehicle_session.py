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

import wfb_store as store


def _g(d, *path):
    for k in path:
        if not isinstance(d, dict):
            return None
        d = d.get(k)
    return d


def import_vehicle_jsonl(conn, path: str, vehicle_session: str, **meta) -> tuple[int, int]:
    notes = f"vehicle={vehicle_session}"
    if meta.get("notes"):
        notes += f" · {meta.pop('notes')}"
    sid = store.create_session(conn, "vehicle-uplink", notes=notes, **meta)
    n = bad = 0
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
            up = d.get("up")
            if not isinstance(up, (int, float)):
                bad += 1   # no monotonic timestamp — skip, don't pile rows at t=0
                continue
            ts_ms = int(up * 1000)
            rssi = _g(d, "status", "score", "smoothed_rssi")
            raw = _g(d, "status", "score", "raw_rssi")
            spread = abs(rssi - raw) if isinstance(rssi, (int, float)) and isinstance(raw, (int, float)) else None
            per = _g(d, "status", "score", "smoothed_lost_ratio")
            mcs = _g(d, "status", "mcs", "current_mcs")
            conn.execute(
                """INSERT INTO records
                   (session_id, ts_ms, seq, mcs, rssi_comb, rssi_spread, snr_avg,
                    pkt_all, pkt_uniq, pkt_lost, fec_rec, dec_err, per, raw_json)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (sid, ts_ms, None, mcs, rssi, spread, None,
                 None, None, None, None, None, per,
                 json.dumps(d, separators=(",", ":"))))
            n += 1
    store.close_session(conn, sid)
    if bad:
        print(f"[import-vehicle] skipped {bad} unparseable lines", file=sys.stderr)
    return sid, n


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
