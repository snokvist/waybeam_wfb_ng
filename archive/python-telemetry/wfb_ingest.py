#!/usr/bin/env python3
"""Standalone live ingester for wfb_rx -Y telemetry → SQLite data-session store.

A thin CLI wrapper around the shared capture core (wfb_capture.Capture). The
same loop powers the consolidated one-app deploy (webui/webapp.py runs it in a
background thread), so the two never drift.

Wiring (non-disruptive — gs_supervisor emits the stats_tap copy AFTER the
back-channel forward):

    wfb_rx -Y → gs_supervisor stats_drain():
        stats_out 127.0.0.1:6600   (uplink back-channel — UNCHANGED)
        stats_tap 127.0.0.1:6700 → this ingester → telemetry/wfb.sqlite

    python3 telemetry/wfb_ingest.py --listen 6700 --scenario flight

A session opens on the first datagram and closes on Ctrl-C/SIGTERM, after
--idle-timeout idle seconds, or every --max-duration seconds (rolling a fresh
one so files stay bounded). Writes commit every --commit-every records (or
--commit-secs); WAL (schema.sql) lets the web UI read while we write.

Tip: for an all-in-one deploy run `webui/webapp.py` instead — it captures AND
serves the UI from one process (no separate ingester, no sudo). See DATASTORE.md.

Stdlib only.
"""
from __future__ import annotations

import argparse
import signal

import wfb_store as store
from wfb_capture import Capture, CaptureConfig, META_FIELDS


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
    ap.add_argument("--max-duration", type=float, default=1200.0,
                    help="hard-cap a session at this many seconds, then roll a fresh "
                         "one so files stay bounded (default 1200 = 20 min; 0 = unbounded)")
    ap.add_argument("--stats-every", type=float, default=5.0, help="seconds between log lines (0=silent)")
    for f in META_FIELDS:
        ap.add_argument(f"--{f.replace('_', '-')}", dest=f, default=None)
    args = ap.parse_args()

    cfg = CaptureConfig(
        db=args.db, listen=args.listen, bind=args.bind, source=args.source,
        commit_every=args.commit_every, commit_secs=args.commit_secs,
        idle_timeout=args.idle_timeout, max_duration=args.max_duration,
        stats_every=args.stats_every,
        meta={f: getattr(args, f) for f in META_FIELDS},
    )
    cap = Capture(cfg)

    # Daemonized with `&`, this process inherits SIG_IGN for SIGINT and Python
    # keeps the inherited ignore — so install explicit handlers. They just set
    # the stop event; the loop closes the session cleanly within ~1 s.
    def _shutdown(_signum, _frame):
        cap.stop()
    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    cap.run()   # blocking, this (main) thread


if __name__ == "__main__":
    main()
