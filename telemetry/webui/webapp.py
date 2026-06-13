#!/usr/bin/env python3
"""Flask + uPlot web UI for the wfb data-session store (see DATASTORE.md).

Phase 3: read-only browsing. Lists sessions, plots a session's telemetry as
synchronised uPlot time-series, and overlays the ML labels (Tier-1 state /
MCS reco from the `predictions` table) plus any human `labels` spans on top of
the raw stream. Read-only over WAL, so it can run while wfb_ingest.py writes.

    pip install -r telemetry/requirements-webui.txt
    python3 telemetry/webui/webapp.py --db telemetry/wfb.sqlite --port 8080
    # open http://127.0.0.1:8080

The store layer (wfb_store) is stdlib; only this presentation layer needs Flask.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime

# wfb_store lives one dir up (telemetry/); make it importable when run directly.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import wfb_store as store  # noqa: E402

from flask import Flask, abort, jsonify, render_template, request  # noqa: E402

app = Flask(__name__)
app.config["DB"] = store.DEFAULT_DB

# series columns surfaced to the chart (record column -> JSON key)
SERIES_COLS = ("rssi_comb", "rssi_spread", "snr_avg", "per",
               "mcs", "pkt_lost", "fec_rec")


def _conn():
    return store.connect(app.config["DB"])


def _latest_model_ver(conn) -> str | None:
    row = conn.execute("SELECT model_ver FROM predictions ORDER BY id DESC LIMIT 1").fetchone()
    return row["model_ver"] if row else None


# capture_session.sh lives in telemetry/ (one dir up from webui/).
CAPTURE_SH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "capture_session.sh")


@app.route("/")
def index():
    conn = _conn()
    sessions = [dict(r) for r in store.list_sessions(conn)]
    conn.close()
    return render_template("index.html", sessions=sessions)


@app.route("/api/capture/new", methods=["POST"])
def capture_new():
    """Roll a fresh GS capture session for a clean break: close the current
    ingester (stamps ended_at) and start a new one (new `sessions` row).
    `capture_session.sh start` reclaims the UDP port and re-launches, so it IS
    the roll. The ingester runs as root (launched by gs_supervisor), so we go
    through `sudo -n`. With LOG_SYNC active the vehicle keeps logging
    continuously and the importer attributes its records to whichever GS
    session was live — so this boundary carries to the vehicle data with no
    vehicle restart."""
    if not os.path.exists(CAPTURE_SH):
        return jsonify(ok=False, error="capture_session.sh not found"), 500
    try:
        # Fixed argv, no user input — not shell-interpolated, so no injection.
        r = subprocess.run(["sudo", "-n", "bash", CAPTURE_SH, "start"],
                           capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return jsonify(ok=False, error="capture_session.sh timed out"), 504
    except OSError as e:
        return jsonify(ok=False, error=str(e)), 500
    msg = (r.stdout or "").strip()
    if r.stderr.strip():
        msg = (msg + "  " + r.stderr.strip()).strip()
    return jsonify(ok=(r.returncode == 0), message=msg, code=r.returncode)


def _epoch(iso):
    """ISO8601 (as stored by wfb_store) -> unix seconds, or None."""
    if not iso:
        return None
    try:
        return datetime.fromisoformat(iso).timestamp()
    except (ValueError, TypeError):
        return None


def _find_gs_pair(conn, gs_lo, gs_hi):
    """The GS (downlink) session active over a vehicle walk's gs_unix range
    [gs_lo, gs_hi] seconds. Latest-started among overlappers (orphan-robust),
    matching import_vehicle_session._identify_gs_session. Returns id or None."""
    if gs_lo is None or gs_hi is None:
        return None
    mid = (gs_lo + gs_hi) / 2.0
    contain, overlap = [], []
    for r in conn.execute("SELECT id, started_at, ended_at FROM sessions "
                          "WHERE source LIKE 'live-%'"):
        s0 = _epoch(r["started_at"])
        s1 = _epoch(r["ended_at"]) or (gs_hi + 1e9)
        if s0 is None:
            continue
        if min(gs_hi, s1) - max(gs_lo, s0) <= 0:
            continue
        overlap.append((min(gs_hi, s1) - max(gs_lo, s0), r["id"]))
        if s0 <= mid <= s1:
            contain.append((s0, r["id"]))
    if contain:
        return max(contain)[1]
    if overlap:
        return max(overlap)[1]
    return None


@app.route("/api/session/<int:vid>/overlay")
def overlay_series(vid: int):
    """Uplink (this vehicle-uplink session) + downlink (its paired GS session)
    on ONE gs-wall-clock axis. Vehicle records carry gs_unix_ms (stamped by the
    importer's LOG_SYNC fit); GS records are mapped via the session start epoch +
    (ts_ms - first_ts_ms), i.e. wfb_rx's real-time 100 ms cadence. The shared
    x-axis is gs unix SECONDS so uPlot renders wall-clock time."""
    conn = _conn()
    vs = store.get_session(conn, vid)
    if not vs or vs["source"] != "vehicle-uplink":
        conn.close()
        abort(404)
    vt, v_rssi, v_per, v_mcs = [], [], [], []
    gs_lo = gs_hi = None
    for r in conn.execute("SELECT rssi_comb, per, mcs, raw_json FROM records "
                          "WHERE session_id=? ORDER BY ts_ms", (vid,)):
        try:
            g = json.loads(r["raw_json"]).get("gs_unix_ms")
        except (ValueError, TypeError):
            g = None
        if not isinstance(g, (int, float)):
            continue
        gs = g / 1000.0
        vt.append(gs)
        v_rssi.append(r["rssi_comb"])
        v_per.append(r["per"])
        v_mcs.append(r["mcs"])
        gs_lo = gs if gs_lo is None else min(gs_lo, gs)
        gs_hi = gs if gs_hi is None else max(gs_hi, gs)

    gs_sid = _find_gs_pair(conn, gs_lo, gs_hi)
    gt, g_rssi, g_per, g_snr = [], [], [], []
    if gs_sid:
        gsess = store.get_session(conn, gs_sid)
        start = _epoch(gsess["started_at"]) if gsess else None
        grows = list(conn.execute(
            "SELECT ts_ms, rssi_comb, per, snr_avg FROM records "
            "WHERE session_id=? ORDER BY ts_ms", (gs_sid,)))
        if grows and start is not None:
            t0 = grows[0]["ts_ms"]
            for r in grows:
                gt.append(start + (r["ts_ms"] - t0) / 1000.0)
                g_rssi.append(r["rssi_comb"])
                g_per.append(r["per"])
                g_snr.append(r["snr_avg"])
    conn.close()
    return app.response_class(json.dumps({
        "vehicle_session": vid, "gs_session": gs_sid,
        "aligned": bool(vt and gt),
        "uplink": {"t": vt, "rssi": v_rssi, "per": v_per, "mcs": v_mcs},
        "downlink": {"t": gt, "rssi": g_rssi, "per": g_per, "snr": g_snr},
    }), mimetype="application/json")


@app.route("/overlay/<int:vid>")
def overlay_page(vid: int):
    conn = _conn()
    s = store.get_session(conn, vid)
    conn.close()
    if not s or s["source"] != "vehicle-uplink":
        abort(404)
    return render_template("overlay.html", session=dict(s))


@app.route("/session/<int:sid>")
def session(sid: int):
    conn = _conn()
    s = store.get_session(conn, sid)
    conn.close()
    if not s:
        abort(404)
    return render_template("session.html", session=dict(s))


@app.route("/api/sessions")
def api_sessions():
    conn = _conn()
    out = [dict(r) for r in store.list_sessions(conn)]
    conn.close()
    return jsonify(out)


@app.route("/api/session/<int:sid>/series")
def api_series(sid: int):
    """Aligned arrays for uPlot + label/ML overlays. x is seconds since session start."""
    conn = _conn()
    sess = store.get_session(conn, sid)
    if not sess:
        conn.close()
        abort(404)
    is_vehicle = (sess["source"] == "vehicle-uplink")
    rows = store.get_records(conn, sid)
    model_ver = _latest_model_ver(conn)

    # ML labels: latest model's tier1_state per record (None where unscored)
    tier1 = {}
    if model_ver:
        for p in conn.execute(
                "SELECT record_id, tier1_state FROM predictions WHERE model_ver = ?",
                (model_ver,)):
            tier1[p["record_id"]] = p["tier1_state"]

    human = [dict(r) for r in conn.execute(
        "SELECT t0_ms, t1_ms, kind, value, author FROM labels "
        "WHERE session_id = ? ORDER BY t0_ms", (sid,))]
    conn.close()

    t0 = rows[0]["ts_ms"] if rows else 0
    series = {"t": [(r["ts_ms"] - t0) / 1000.0 for r in rows]}
    for c in SERIES_COLS:
        series[c] = [r[c] for r in rows]            # may contain nulls (e.g. snr_avg)
    series["tier1_state"] = [tier1.get(r["id"]) for r in rows]

    # Per-received-MCS packet counts per record, for a stacked-bar view of the
    # MCS mix over time (a single mcs line can't show bulk-vs-protected split).
    # Diversity antennas repeat the same packet set, so the count at a rung is
    # the MAX pkts across ant entries at that rung, not their sum. Fixed 0..7
    # rung set keeps the series count stable across live polls.
    def _rung(m):
        # accept a real int rung 0..7 only — reject bools ("mcs": true) and floats
        return m if isinstance(m, int) and not isinstance(m, bool) and 0 <= m < 8 else None

    mcs_dist = {str(m): [0] * len(rows) for m in range(8)}
    for i, r in enumerate(rows):
        best: dict[int, int] = {}
        try:
            ants = json.loads(r["raw_json"]).get("ant")
            if isinstance(ants, list):
                for a in ants:
                    if not isinstance(a, dict):
                        continue
                    m, p = _rung(a.get("mcs")), a.get("pkts", 0)
                    if m is not None and isinstance(p, (int, float)) \
                            and not isinstance(p, bool) and p > best.get(m, 0):
                        best[m] = p
        except (ValueError, TypeError, AttributeError):
            best = {}
        if best:
            # Keep ALL rungs present in the record — the downlink legitimately
            # carries packets at two rungs at once: the bulk video stream plus a
            # steady low-rate peek-PROTECT sub-stream (key/param frames sent at a
            # more robust MCS). Charting both is the point — it shows protection
            # working. The apparent "all rungs active" smear was the cumulative
            # tooltip (fixed client-side to show per-rung counts), not the data.
            for m, p in best.items():
                mcs_dist[str(m)][i] = int(p)
        elif is_vehicle:
            # Vehicle-uplink sessions have no ant[]: show the denormalised
            # current MCS as a single-rung mark so the stacked bar still renders
            # the active rung. Gated on source so a GS blackout row (empty ant)
            # doesn't draw a phantom rung from its denormalised mcs.
            m = _rung(r["mcs"])
            if m is not None:
                mcs_dist[str(m)][i] = 1

    # overlay spans converted to x-seconds for the chart's band plugin
    overlays = [{"x0": (h["t0_ms"] - t0) / 1000.0,
                 "x1": (h["t1_ms"] - t0) / 1000.0,
                 "kind": h["kind"], "value": h["value"], "author": h["author"]}
                for h in human]

    return app.response_class(
        json.dumps({"series": series, "overlays": overlays, "mcs_dist": mcs_dist,
                    "model_ver": model_ver, "n": len(rows)}),
        mimetype="application/json")


@app.route("/api/session/<int:sid>/labels", methods=["GET", "POST"])
def api_labels(sid: int):
    """GET: list labels (with x-seconds for the chart). POST: add one from a
    chart selection ({t0_s, t1_s, kind, value, author})."""
    conn = _conn()
    if not store.get_session(conn, sid):
        conn.close()
        abort(404)
    t0 = store.first_ts(conn, sid)
    if request.method == "POST":
        d = request.get_json(force=True) or {}
        t0_ms = round(float(d["t0_s"]) * 1000) + t0
        t1_ms = round(float(d["t1_s"]) * 1000) + t0
        lid = store.add_label(conn, sid, t0_ms, t1_ms,
                              (d.get("kind") or "event").strip(),
                              (d.get("value") or "").strip(),
                              (d.get("author") or "human:web").strip())
        conn.close()
        return jsonify({"id": lid}), 201
    rows = []
    for r in store.list_labels(conn, sid):
        row = dict(r)
        row["t0_s"] = (r["t0_ms"] - t0) / 1000.0
        row["t1_s"] = (r["t1_ms"] - t0) / 1000.0
        rows.append(row)
    conn.close()
    return jsonify(rows)


@app.route("/api/label/<int:lid>", methods=["DELETE"])
def api_label_delete(lid: int):
    conn = _conn()
    store.delete_label(conn, lid)
    conn.close()
    return "", 204


@app.route("/api/session/<int:sid>/meta", methods=["POST"])
def api_meta(sid: int):
    conn = _conn()
    if not store.get_session(conn, sid):
        conn.close()
        abort(404)
    d = request.get_json(force=True) or {}
    fields = {k: (d[k] if d[k] != "" else None)
              for k in store.META_FIELDS if k in d}
    if "channel" in fields and fields["channel"] is not None:
        try:
            fields["channel"] = int(fields["channel"])
        except (TypeError, ValueError):
            fields["channel"] = None
    store.update_session_meta(conn, sid, **fields)
    conn.close()
    return "", 204


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=store.DEFAULT_DB)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    app.config["DB"] = args.db
    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == "__main__":
    main()
