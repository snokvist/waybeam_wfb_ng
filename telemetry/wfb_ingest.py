#!/usr/bin/env python3
"""Live ingester for wfb_rx -Y telemetry → SQLite data-session store.

Phase 2 of the data-session store (see DATASTORE.md). A stdlib UDP listener that
sits on a `wfb_stats_tee.py --tap` and writes every datagram into a fresh
`sessions` row in real time, so a live flight lands in the same store as
imported historical captures and the web UI can read it as it arrives.

Wiring (non-disruptive — the tee forwards upstream FIRST):

    wfb_rx -Y → gs_supervisor → tee --forward :6600 (upstream, untouched)
                                    --tap 127.0.0.1:6700  ──► this ingester

    python3 telemetry/wfb_stats_tee.py --listen 6650 \
        --forward 127.0.0.1:6600 --tap 127.0.0.1:6700
    python3 telemetry/wfb_ingest.py --listen 6700 --scenario flight

A new session opens on the first datagram and closes on Ctrl-C or after
--idle-timeout seconds without traffic. Writes are committed every
--commit-every records (or --commit-secs) so a crash loses at most that window;
WAL mode (set in schema.sql) lets the web UI read while we write.

Stdlib only (matches the telemetry guardrail).
"""
from __future__ import annotations

import argparse
import json
import signal
import socket
import sys
import time

import wfb_store as store

META_FIELDS = store.META_FIELDS


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=store.DEFAULT_DB, help="sqlite file")
    ap.add_argument("--listen", type=int, default=6700, help="UDP port to receive stats on")
    ap.add_argument("--bind", default="127.0.0.1", help="address to bind --listen")
    ap.add_argument("--source", default="live-tee", help="sessions.source tag")
    ap.add_argument("--commit-every", type=int, default=20, help="commit every N records")
    ap.add_argument("--commit-secs", type=float, default=2.0, help="...or every this many seconds")
    ap.add_argument("--idle-timeout", type=float, default=0.0,
                    help="close the session after this many idle seconds (0 = run until Ctrl-C)")
    ap.add_argument("--stats-every", type=float, default=5.0, help="seconds between log lines (0=silent)")
    for f in META_FIELDS:
        ap.add_argument(f"--{f.replace('_', '-')}", dest=f, default=None)
    args = ap.parse_args()

    # Daemonized with `&`, this process inherits SIG_IGN for SIGINT and Python
    # keeps an inherited ignore — so a bare `kill -INT` is a no-op and the
    # session would never close cleanly (ended_at stays NULL, port held).
    # Install explicit handlers so BOTH signals unwind through the `finally`
    # (close_session + final commit).
    def _shutdown(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    conn = store.connect(args.db)
    store.init_db(conn)  # idempotent

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind((args.bind, args.listen))
    if args.idle_timeout > 0:
        rx.settimeout(args.idle_timeout)

    print(f"[ingest] listening udp {args.bind}:{args.listen} -> {args.db}",
          file=sys.stderr, flush=True)

    session_id: int | None = None
    n = n_bad = 0                       # totals
    pend = 0                            # uncommitted since last commit
    last_commit = last_log = time.monotonic()
    win = 0                             # records in the current log window

    def commit():
        nonlocal pend, last_commit
        if pend:
            conn.commit()
            pend = 0
        last_commit = time.monotonic()

    try:
        while True:
            try:
                data, _ = rx.recvfrom(65535)
            except socket.timeout:
                print(f"[ingest] idle {args.idle_timeout}s — closing session", file=sys.stderr)
                break
            try:
                rec = json.loads(data)
            except (ValueError, UnicodeDecodeError):
                n_bad += 1
                continue
            if session_id is None:
                meta = {f: getattr(args, f) for f in META_FIELDS}
                session_id = store.create_session(conn, args.source, **meta)
                print(f"[ingest] opened session {session_id}", file=sys.stderr, flush=True)
            store.insert_record(conn, session_id, rec)
            n += 1
            pend += 1
            win += 1

            now = time.monotonic()
            if pend >= args.commit_every or (now - last_commit) >= args.commit_secs:
                commit()
            if args.stats_every > 0 and (now - last_log) >= args.stats_every:
                rate = win / (now - last_log)
                print(f"[ingest] session {session_id}: {n} records "
                      f"({rate:.1f}/s, {n_bad} bad)", file=sys.stderr, flush=True)
                win = 0
                last_log = now
    except KeyboardInterrupt:
        print("\n[ingest] stopped", file=sys.stderr)
    finally:
        commit()
        if session_id is not None:
            store.close_session(conn, session_id)
            print(f"[ingest] closed session {session_id}: {n} records ({n_bad} bad)",
                  file=sys.stderr)
        conn.close()


if __name__ == "__main__":
    main()
