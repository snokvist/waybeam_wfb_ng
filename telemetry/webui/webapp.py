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
import sys

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


@app.route("/")
def index():
    conn = _conn()
    sessions = [dict(r) for r in store.list_sessions(conn)]
    conn.close()
    return render_template("index.html", sessions=sessions)


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
