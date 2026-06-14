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


# capture_session.sh / import_vehicle_session.sh live in telemetry/ (one dir up).
_TELEMETRY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURE_SH = os.path.join(_TELEMETRY_DIR, "capture_session.sh")
IMPORT_SH = os.path.join(_TELEMETRY_DIR, "import_vehicle_session.sh")

# Strict input gates for the vehicle-fetch endpoint. The import shell interpolates
# both values into a remote `ssh` command (`ssh root@$IP "ls $WALKOUT/${SEL}_*"`),
# so validate here even though we pass argv (no local shell) — these block any
# injection / stray ssh option from reaching the vehicle.
import re  # noqa: E402
_IP_RE = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")
_SEL_RE = re.compile(r"^(latest|\d{6})$")


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


@app.route("/api/vehicle/fetch", methods=["POST"])
def vehicle_fetch():
    """Pull a vehicle walkout session over the management link and import its
    link_controller status.jsonl as a `vehicle-uplink` session, so the uplink
    sits next to the GS downlink for the same walk. Wraps import_vehicle_session.sh
    (scp + the LOG_SYNC marker-fit importer); the alignment line it prints is
    surfaced back to the UI. Runs as the web-UI user (its ssh keys reach the
    vehicle) — no sudo, unlike the root-owned capture ingester."""
    if not os.path.exists(IMPORT_SH):
        return jsonify(ok=False, error="import_vehicle_session.sh not found"), 500
    d = request.get_json(silent=True) or request.form
    ip = (d.get("ip") or "").strip()
    sel = (d.get("session") or "latest").strip()
    if not _IP_RE.match(ip):
        return jsonify(ok=False, error=f"invalid vehicle IP {ip!r}"), 400
    if not _SEL_RE.match(sel):
        return jsonify(ok=False, error="session must be 'latest' or 6 digits"), 400
    # Point the importer at the SAME db the UI reads (it defaults to its own).
    env = dict(os.environ, WFB_DB=os.path.abspath(app.config["DB"]))
    try:
        r = subprocess.run(["bash", IMPORT_SH, ip, sel],
                           capture_output=True, text=True, timeout=180, env=env)
    except subprocess.TimeoutExpired:
        return jsonify(ok=False, error="vehicle fetch timed out (link slow?)"), 504
    except OSError as e:
        return jsonify(ok=False, error=str(e)), 500
    out = (r.stdout or "").strip()
    err = (r.stderr or "").strip()
    # The importer reports its fit/alignment on stderr ("[import-vehicle] aligned …");
    # carry the last informative line back so the user sees the GS-session match.
    align = next((ln for ln in reversed(err.splitlines())
                  if "[import-vehicle]" in ln), "")
    msg = " · ".join(x for x in (out.splitlines()[-1:] or [""]) + [align] if x).strip(" ·")
    return jsonify(ok=(r.returncode == 0), message=msg or err or out, code=r.returncode)


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


def _uplink_in_window(conn, lo, hi):
    """Vehicle-uplink records (from ANY vehicle-uplink session) whose stamped
    gs_unix_ms falls in [lo, hi] gs-unix-seconds. Keyed on wall-clock, not on the
    vehicle session's notes label — so a single continuous vehicle log that spans
    several GS sessions (one file → many GS windows, because the vehicle keeps
    logging across GS `new capture` breaks) still slices to the right walk."""
    t, rssi, per, mcs = [], [], [], []
    for r in conn.execute(
            "SELECT r.rssi_comb, r.per, r.mcs, r.raw_json FROM records r "
            "JOIN sessions s ON s.id = r.session_id "
            "WHERE s.source = 'vehicle-uplink' ORDER BY r.ts_ms"):
        try:
            g = json.loads(r["raw_json"]).get("gs_unix_ms")
        except (ValueError, TypeError):
            g = None
        if not isinstance(g, (int, float)):
            continue
        gs = g / 1000.0
        if lo is not None and (gs < lo or gs > hi):
            continue
        t.append(gs)
        rssi.append(r["rssi_comb"])
        per.append(r["per"])
        mcs.append(r["mcs"])
    return t, rssi, per, mcs


def _downlink_in_window(conn, gs_sid, lo, hi):
    """GS (downlink) records for `gs_sid`, mapped to gs-wall-clock seconds via the
    session start epoch + wfb_rx's 100 ms cadence, clipped to [lo, hi]."""
    t, rssi, per, snr = [], [], [], []
    gsess = store.get_session(conn, gs_sid)
    start = _epoch(gsess["started_at"]) if gsess else None
    rows = list(conn.execute(
        "SELECT ts_ms, rssi_comb, per, snr_avg FROM records "
        "WHERE session_id=? ORDER BY ts_ms", (gs_sid,)))
    if rows and start is not None:
        t0 = rows[0]["ts_ms"]
        for r in rows:
            gt = start + (r["ts_ms"] - t0) / 1000.0
            if lo is not None and (gt < lo or gt > hi):
                continue
            t.append(gt)
            rssi.append(r["rssi_comb"])
            per.append(r["per"])
            snr.append(r["snr_avg"])
    return t, rssi, per, snr


@app.route("/api/session/<int:sid>/overlay")
def overlay_series(sid: int):
    """Uplink (vehicle, 1 Hz) + downlink (GS, 10 Hz) on ONE gs-wall-clock axis.
    Works from EITHER end of a walk:
      * a vehicle-uplink session — window = its stamped gs_unix range, GS side is
        the paired GS session (latest-started overlapper);
      * a GS (`live-*`) session — window = the GS session's own span, uplink is
        the vehicle slice that falls inside it.
    The uplink is always pulled by wall-clock window (`_uplink_in_window`), so it's
    correct even when one continuous vehicle log covers several GS sessions."""
    conn = _conn()
    s = store.get_session(conn, sid)
    if not s:
        conn.close()
        abort(404)
    src = s["source"] or ""
    gs_sid = lo = hi = None
    if src == "vehicle-uplink":
        for r in conn.execute("SELECT raw_json FROM records WHERE session_id=? "
                              "ORDER BY ts_ms", (sid,)):
            try:
                g = json.loads(r["raw_json"]).get("gs_unix_ms")
            except (ValueError, TypeError):
                g = None
            if isinstance(g, (int, float)):
                gs = g / 1000.0
                lo = gs if lo is None else min(lo, gs)
                hi = gs if hi is None else max(hi, gs)
        gs_sid = _find_gs_pair(conn, lo, hi)
    elif src.startswith("live-"):
        gs_sid = sid
        start = _epoch(s["started_at"])
        rows = list(conn.execute("SELECT ts_ms FROM records WHERE session_id=? "
                                 "ORDER BY ts_ms", (sid,)))
        if rows and start is not None:
            lo = start
            hi = start + (rows[-1]["ts_ms"] - rows[0]["ts_ms"]) / 1000.0
    else:
        conn.close()
        abort(404)

    ut, u_rssi, u_per, u_mcs = _uplink_in_window(conn, lo, hi)
    gt, g_rssi, g_per, g_snr = (_downlink_in_window(conn, gs_sid, lo, hi)
                                if gs_sid else ([], [], [], []))
    conn.close()
    return app.response_class(json.dumps({
        "session": sid, "source": src, "gs_session": gs_sid,
        "vehicle_session": sid if src == "vehicle-uplink" else None,
        "aligned": bool(ut and gt),
        "uplink": {"t": ut, "rssi": u_rssi, "per": u_per, "mcs": u_mcs},
        "downlink": {"t": gt, "rssi": g_rssi, "per": g_per, "snr": g_snr},
    }), mimetype="application/json")


@app.route("/overlay/<int:sid>")
def overlay_page(sid: int):
    conn = _conn()
    s = store.get_session(conn, sid)
    conn.close()
    if not s or not ((s["source"] or "").startswith(("vehicle-uplink", "live-"))):
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


@app.route("/api/session/<int:sid>", methods=["DELETE"])
def delete_session(sid: int):
    """Remove a session and all its rows (records, their predictions, labels).
    Refuses the ACTIVE capture (an open live-* session the ingester is still
    writing) — deleting it would make every subsequent ingester insert fail the
    session_id foreign key. Roll a new capture first, then delete the old one."""
    conn = _conn()
    s = store.get_session(conn, sid)
    if not s:
        conn.close()
        abort(404)
    if s["source"] and s["source"].startswith("live-") and not s["ended_at"]:
        conn.close()
        return jsonify(ok=False,
                       error="refusing to delete the active capture — roll a new "
                             "session first, then delete this one"), 409
    conn.execute("PRAGMA busy_timeout=8000")
    try:
        with conn:  # one transaction; children before parent (FKs are ON)
            conn.execute("DELETE FROM predictions WHERE record_id IN "
                         "(SELECT id FROM records WHERE session_id=?)", (sid,))
            conn.execute("DELETE FROM records WHERE session_id=?", (sid,))
            conn.execute("DELETE FROM labels WHERE session_id=?", (sid,))
            conn.execute("DELETE FROM sessions WHERE id=?", (sid,))
    except Exception as e:  # noqa: BLE001 — surface DB errors (e.g. lock) to the UI
        conn.close()
        return jsonify(ok=False, error=str(e)), 500
    conn.close()
    return jsonify(ok=True, deleted=sid)


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
        # No `ant[]` fallback for vehicle-uplink rows: they have no per-packet
        # MCS distribution to stack (the old single-unit mark just drew dots).
        # The vehicle view charts current_mcs as a step line instead.

    # Vehicle-uplink rows carry the controller's STATE, not packet stats. Pull
    # the two signals the GS columns don't cover — adapter_count (diversity
    # health) and rssi_slope_db_s (the fade-rate the controller demotes on) —
    # so the vehicle view can show what actually drove its decisions.
    vehicle_extra = {}
    if is_vehicle:
        adapter, slope = [], []
        for r in rows:
            try:
                sc = json.loads(r["raw_json"]).get("status", {}).get("score", {})
            except (ValueError, TypeError, AttributeError):
                sc = {}
            if not isinstance(sc, dict):
                sc = {}
            ac = sc.get("adapter_count")
            sl = sc.get("rssi_slope_db_s")
            # adapter_count == -1 is the pre-init sentinel — null it, not a dip.
            adapter.append(ac if isinstance(ac, (int, float)) and not isinstance(ac, bool) and ac >= 0 else None)
            slope.append(sl if isinstance(sl, (int, float)) and not isinstance(sl, bool) else None)
        vehicle_extra = {"adapter_count": adapter, "rssi_slope": slope}

    # overlay spans converted to x-seconds for the chart's band plugin
    overlays = [{"x0": (h["t0_ms"] - t0) / 1000.0,
                 "x1": (h["t1_ms"] - t0) / 1000.0,
                 "kind": h["kind"], "value": h["value"], "author": h["author"]}
                for h in human]

    return app.response_class(
        json.dumps({"series": series, "overlays": overlays, "mcs_dist": mcs_dist,
                    "is_vehicle": is_vehicle, "vehicle_extra": vehicle_extra,
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
    # Ensure the DB exists with schema before serving. sqlite creates an empty
    # file on first connect, but connect() does NOT run schema.sql — so a
    # missing/fresh DB (e.g. the live file was rotated or never initialised)
    # would 500 on the first SELECT in list_sessions(). init_db is idempotent
    # (schema.sql is all CREATE ... IF NOT EXISTS), so this is a no-op on a
    # populated store and yields an empty-but-valid UI on a fresh one.
    _seed = store.connect(args.db)
    try:
        store.init_db(_seed)
    finally:
        _seed.close()
    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == "__main__":
    main()
