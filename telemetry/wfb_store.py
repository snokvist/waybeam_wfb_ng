#!/usr/bin/env python3
"""SQLite data-session store for wfb telemetry (see DATASTORE.md).

Phase 1 of the data-session store: schema + JSONL importer + query helpers,
stdlib `sqlite3` only (no Flask/uPlot here — the web layer sits on top of this).

Each capture (a range walk, a bench run, a live flight) becomes one `sessions`
row; every `wfb_rx -Y` datagram becomes one `records` row. `raw_json` is stored
verbatim as the source of truth; the hot columns (mcs, rssi_comb, per, ...) are
a denormalised cache derived per record for fast plotting/filtering and can be
rebuilt from raw_json with `rederive`.

CLI:
    python3 telemetry/wfb_store.py init [--db telemetry/wfb.sqlite]
    python3 telemetry/wfb_store.py import telemetry/real_wfb.jsonl \
        --scenario range-walk --location "field N" --notes "move-away run"
    python3 telemetry/wfb_store.py list
    python3 telemetry/wfb_store.py show <session_id> [--limit 10]
    python3 telemetry/wfb_store.py rederive [<session_id>]   # rebuild hot columns
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
from datetime import datetime, timezone

SCHEMA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "schema.sql")
DEFAULT_DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wfb.sqlite")

# session metadata columns a caller/CLI may set on import
META_FIELDS = ("location", "antenna_cfg", "tx_power", "channel",
               "scenario", "weather", "notes")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def connect(db_path: str = DEFAULT_DB) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


# Columns added to `records` after the store first shipped. CREATE TABLE
# IF NOT EXISTS can't add a column to an existing table, so init_db applies
# these additively to a pre-existing store (idempotent — skips ones present).
_RECORD_ADDED_COLUMNS = (
    ("uplink_rssi", "REAL"),
    ("uplink_pkt", "INTEGER"),
    ("uplink_lost", "INTEGER"),
)


def init_db(conn: sqlite3.Connection) -> None:
    with open(SCHEMA_PATH) as fh:
        conn.executescript(fh.read())
    have = {row[1] for row in conn.execute("PRAGMA table_info(records)")}
    for name, decl in _RECORD_ADDED_COLUMNS:
        if name not in have:
            conn.execute(f"ALTER TABLE records ADD COLUMN {name} {decl}")
    conn.commit()


# --- denormalised column derivation ---------------------------------------

def _num(x) -> float | None:
    return float(x) if isinstance(x, (int, float)) and not isinstance(x, bool) else None


def derive_columns(rec: dict) -> dict:
    """Compute the hot/denormalised columns from one raw record.

    Schema-tolerant: every field falls back to None if absent (matches the
    RSSI-only live path where `snr` may be missing). Chains are combined the way
    MCS_STRATEGY.md prescribes — rssi_comb is the *best* chain (diversity), with
    rssi_spread as the per-chain disagreement.
    """
    ant = rec.get("ant") or []
    rssi_avgs, snr_avgs, mcs_vals = [], [], []
    for a in ant:
        if not isinstance(a, dict):
            continue
        r = _num((a.get("rssi") or {}).get("avg")) if isinstance(a.get("rssi"), dict) else None
        if r is not None:
            rssi_avgs.append(r)
        s = _num((a.get("snr") or {}).get("avg")) if isinstance(a.get("snr"), dict) else None
        if s is not None:
            snr_avgs.append(s)
        m = _num(a.get("mcs"))
        if m is not None:
            mcs_vals.append(int(m))

    pkt = rec.get("pkt") or {}
    uniq = _num(pkt.get("uniq"))
    lost = _num(pkt.get("lost"))
    per = None
    if uniq is not None and lost is not None:
        denom = uniq + lost
        per = (lost / denom) if denom > 0 else 0.0

    # Vehicle-side uplink reception, present only when a record carries the
    # link_controller /status "uplink_rx" block (the importer path). Raw GS-side
    # rx_ant datagrams have no such block, so these stay None on the live path.
    up = rec.get("uplink_rx")
    if isinstance(up, dict) and up.get("present"):
        uplink_rssi = _num(up.get("smoothed_rssi"))
        uplink_pkt = _num(up.get("pkt_last"))
        uplink_lost = _num(up.get("lost_last"))
    else:
        uplink_rssi = uplink_pkt = uplink_lost = None

    return {
        "ts_ms": _num(rec.get("ts_ms")),
        "seq": _num(rec.get("seq")),
        # per-antenna mcs is uniform in practice; take the mode defensively
        "mcs": max(set(mcs_vals), key=mcs_vals.count) if mcs_vals else None,
        "rssi_comb": max(rssi_avgs) if rssi_avgs else None,
        "rssi_spread": (max(rssi_avgs) - min(rssi_avgs)) if rssi_avgs else None,
        "snr_avg": (sum(snr_avgs) / len(snr_avgs)) if snr_avgs else None,
        "pkt_all": _num(pkt.get("all")),
        "pkt_uniq": uniq,
        "pkt_lost": lost,
        "fec_rec": _num(pkt.get("fec_recovered")),
        "dec_err": _num(pkt.get("dec_err")),
        "per": per,
        "uplink_rssi": uplink_rssi,
        "uplink_pkt": uplink_pkt,
        "uplink_lost": uplink_lost,
    }


# --- writes ----------------------------------------------------------------

def create_session(conn: sqlite3.Connection, source: str, **meta) -> int:
    cols = ["started_at", "source"] + [k for k in META_FIELDS if meta.get(k) is not None]
    vals = [_now(), source] + [meta[k] for k in META_FIELDS if meta.get(k) is not None]
    ph = ", ".join("?" * len(cols))
    cur = conn.execute(
        f"INSERT INTO sessions ({', '.join(cols)}) VALUES ({ph})", vals)
    return int(cur.lastrowid)


def close_session(conn: sqlite3.Connection, session_id: int) -> None:
    """Stamp ended_at = now and commit (call when a capture finishes)."""
    conn.execute("UPDATE sessions SET ended_at = ? WHERE id = ?", (_now(), session_id))
    conn.commit()


def insert_record(conn: sqlite3.Connection, session_id: int, rec: dict) -> int:
    d = derive_columns(rec)
    cur = conn.execute(
        """INSERT INTO records
           (session_id, ts_ms, seq, mcs, rssi_comb, rssi_spread, snr_avg,
            pkt_all, pkt_uniq, pkt_lost, fec_rec, dec_err, per,
            uplink_rssi, uplink_pkt, uplink_lost, raw_json)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
        (session_id, d["ts_ms"] or 0, d["seq"], d["mcs"], d["rssi_comb"],
         d["rssi_spread"], d["snr_avg"], d["pkt_all"], d["pkt_uniq"],
         d["pkt_lost"], d["fec_rec"], d["dec_err"], d["per"],
         d["uplink_rssi"], d["uplink_pkt"], d["uplink_lost"],
         json.dumps(rec, separators=(",", ":"))))
    return int(cur.lastrowid)


def import_jsonl(conn: sqlite3.Connection, path: str, **meta) -> tuple[int, int]:
    """Import a JSONL capture as one session. Returns (session_id, n_records)."""
    source = meta.pop("source", None) or f"import:{os.path.basename(path)}"
    session_id = create_session(conn, source, **meta)
    n = 0
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            insert_record(conn, session_id, json.loads(line))
            n += 1
    close_session(conn, session_id)  # span is recoverable from records.ts_ms
    return session_id, n


def rederive(conn: sqlite3.Connection, session_id: int | None = None) -> int:
    """Rebuild the denormalised columns from raw_json (after a schema change)."""
    q = "SELECT id, raw_json FROM records"
    args: tuple = ()
    if session_id is not None:
        q += " WHERE session_id = ?"
        args = (session_id,)
    rows = conn.execute(q, args).fetchall()
    for row in rows:
        d = derive_columns(json.loads(row["raw_json"]))
        conn.execute(
            """UPDATE records SET mcs=?, rssi_comb=?, rssi_spread=?, snr_avg=?,
               pkt_all=?, pkt_uniq=?, pkt_lost=?, fec_rec=?, dec_err=?, per=?
               WHERE id=?""",
            (d["mcs"], d["rssi_comb"], d["rssi_spread"], d["snr_avg"],
             d["pkt_all"], d["pkt_uniq"], d["pkt_lost"], d["fec_rec"],
             d["dec_err"], d["per"], row["id"]))
    conn.commit()
    return len(rows)


# --- reads -----------------------------------------------------------------

def list_sessions(conn: sqlite3.Connection) -> list[sqlite3.Row]:
    return conn.execute(
        """SELECT s.*, COUNT(r.id) AS n_records,
                  MIN(r.ts_ms) AS first_ts, MAX(r.ts_ms) AS last_ts
           FROM sessions s LEFT JOIN records r ON r.session_id = s.id
           GROUP BY s.id ORDER BY s.id DESC""").fetchall()


def get_session(conn: sqlite3.Connection, session_id: int) -> sqlite3.Row | None:
    return conn.execute("SELECT * FROM sessions WHERE id = ?", (session_id,)).fetchone()


def get_records(conn: sqlite3.Connection, session_id: int,
                limit: int | None = None, offset: int = 0) -> list[sqlite3.Row]:
    q = "SELECT * FROM records WHERE session_id = ? ORDER BY ts_ms, id"
    args: list = [session_id]
    if limit is not None:
        q += " LIMIT ? OFFSET ?"
        args += [limit, offset]
    return conn.execute(q, args).fetchall()


def first_ts(conn: sqlite3.Connection, session_id: int) -> int:
    """ts_ms of the first record (the t0 the web UI plots seconds relative to)."""
    row = conn.execute(
        "SELECT MIN(ts_ms) AS m FROM records WHERE session_id = ?", (session_id,)).fetchone()
    return int(row["m"]) if row and row["m"] is not None else 0


# --- labels (human/rule/outcome/model annotations) -------------------------

def add_label(conn: sqlite3.Connection, session_id: int, t0_ms: int, t1_ms: int,
              kind: str, value: str, author: str, confidence: float | None = None) -> int:
    if t1_ms < t0_ms:
        t0_ms, t1_ms = t1_ms, t0_ms
    cur = conn.execute(
        """INSERT INTO labels (session_id, t0_ms, t1_ms, kind, value, author,
                               confidence, created_at)
           VALUES (?,?,?,?,?,?,?,?)""",
        (session_id, t0_ms, t1_ms, kind, value, author, confidence, _now()))
    conn.commit()
    return int(cur.lastrowid)


def list_labels(conn: sqlite3.Connection, session_id: int) -> list[sqlite3.Row]:
    return conn.execute(
        "SELECT * FROM labels WHERE session_id = ? ORDER BY t0_ms, id", (session_id,)).fetchall()


def delete_label(conn: sqlite3.Connection, label_id: int) -> None:
    conn.execute("DELETE FROM labels WHERE id = ?", (label_id,))
    conn.commit()


# --- predictions (Tier-1 scores + Tier-2 notes, versioned by model) --------

def add_prediction(conn: sqlite3.Connection, record_id: int, model_ver: str,
                   tier1_state: int | None = None, scores=None,
                   mcs_reco: int | None = None, tier2_note: str | None = None) -> int:
    """Insert one prediction. Caller commits (scoring inserts in a batch)."""
    cur = conn.execute(
        """INSERT INTO predictions (record_id, model_ver, tier1_state, scores,
                                    mcs_reco, tier2_note)
           VALUES (?,?,?,?,?,?)""",
        (record_id, model_ver, tier1_state,
         json.dumps(scores) if scores is not None else None, mcs_reco, tier2_note))
    return int(cur.lastrowid)


def delete_predictions(conn: sqlite3.Connection, model_ver: str,
                       session_id: int | None = None) -> None:
    """Clear a model version's predictions (all sessions, or one) before re-score."""
    if session_id is None:
        conn.execute("DELETE FROM predictions WHERE model_ver = ?", (model_ver,))
    else:
        conn.execute(
            "DELETE FROM predictions WHERE model_ver = ? AND record_id IN "
            "(SELECT id FROM records WHERE session_id = ?)", (model_ver, session_id))
    conn.commit()


def get_scored_records(conn: sqlite3.Connection, session_id: int,
                       model_ver: str) -> list[sqlite3.Row]:
    """Records joined to one model version's predictions, time-ordered (for span
    finding / Tier-2 escalation)."""
    return conn.execute(
        """SELECT r.id, r.ts_ms, r.raw_json, p.id AS pred_id, p.tier1_state
           FROM records r JOIN predictions p
             ON p.record_id = r.id AND p.model_ver = ?
           WHERE r.session_id = ? ORDER BY r.ts_ms, r.id""",
        (model_ver, session_id)).fetchall()


def set_tier2_note(conn: sqlite3.Connection, prediction_id: int, note: str) -> None:
    conn.execute("UPDATE predictions SET tier2_note = ? WHERE id = ?", (note, prediction_id))
    conn.commit()


def session_ids(conn: sqlite3.Connection) -> list[int]:
    return [r["id"] for r in conn.execute("SELECT id FROM sessions ORDER BY id")]


def update_session_meta(conn: sqlite3.Connection, session_id: int, **fields) -> None:
    cols = [k for k in META_FIELDS if k in fields]
    if not cols:
        return
    sets = ", ".join(f"{c} = ?" for c in cols)
    conn.execute(f"UPDATE sessions SET {sets} WHERE id = ?",
                 [fields[c] for c in cols] + [session_id])
    conn.commit()


# --- CLI -------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=DEFAULT_DB, help="sqlite file (default: telemetry/wfb.sqlite)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("init", help="create the schema")

    imp = sub.add_parser("import", help="import a JSONL capture as one session")
    imp.add_argument("jsonl")
    for f in META_FIELDS:
        imp.add_argument(f"--{f.replace('_', '-')}", dest=f, default=None)

    sub.add_parser("list", help="list sessions")

    sh = sub.add_parser("show", help="show a session + sample records")
    sh.add_argument("session_id", type=int)
    sh.add_argument("--limit", type=int, default=10)

    rd = sub.add_parser("rederive", help="rebuild denormalised columns from raw_json")
    rd.add_argument("session_id", type=int, nargs="?", default=None)

    args = ap.parse_args()
    conn = connect(args.db)

    if args.cmd == "init":
        init_db(conn)
        print(f"initialised schema in {args.db}")

    elif args.cmd == "import":
        init_db(conn)  # idempotent; safe if the DB is fresh
        meta = {f: getattr(args, f) for f in META_FIELDS}
        sid, n = import_jsonl(conn, args.jsonl, **meta)
        print(f"imported {n} records from {args.jsonl} -> session {sid}")

    elif args.cmd == "list":
        rows = list_sessions(conn)
        if not rows:
            print("(no sessions)")
        for r in rows:
            span = ""
            if r["first_ts"] is not None and r["last_ts"] is not None:
                span = f"  span={ (r['last_ts'] - r['first_ts']) / 1000:.1f}s"
            print(f"#{r['id']:<4} {r['source']:<24} records={r['n_records']:<6}"
                  f"{span}  scenario={r['scenario'] or '-'}  notes={r['notes'] or '-'}")

    elif args.cmd == "show":
        s = get_session(conn, args.session_id)
        if not s:
            print(f"no session {args.session_id}")
            return
        print(dict(s))
        for r in get_records(conn, args.session_id, limit=args.limit):
            print(f"  ts={r['ts_ms']:<8} mcs={r['mcs']} rssi_comb={r['rssi_comb']} "
                  f"snr_avg={r['snr_avg']} per={r['per']} lost={r['pkt_lost']}")

    elif args.cmd == "rederive":
        n = rederive(conn, args.session_id)
        print(f"rederived {n} records")

    conn.close()


if __name__ == "__main__":
    main()
