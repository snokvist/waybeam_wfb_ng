#!/usr/bin/env python3
"""Reusable UDP→SQLite capture core for the wfb data-session store.

This is the single implementation of the live-ingest loop, shared by:
  - wfb_ingest.py        — standalone CLI daemon (runs Capture in the main thread)
  - webui/webapp.py      — the consolidated one-app deploy (runs Capture in a
                           background thread alongside the Flask UI)

so the two never drift in how they parse datagrams or roll sessions.

Design for the two-thread (webui) case:
  - the Capture thread owns its OWN sqlite write connection (sqlite3's
    check_same_thread is satisfied — only this thread touches it);
  - other threads (Flask request handlers) never touch that connection — they
    read via their own short-lived connections over WAL;
  - roll()/stop() from another thread only SET events; the actual close+reopen
    and connection teardown happen inside the capture loop. So all DB mutation
    stays on one thread, lock-free apart from a tiny lock for the roll handoff.

Stdlib only (matches the telemetry guardrail).
"""
from __future__ import annotations

import json
import socket
import sys
import threading
import time
from dataclasses import dataclass, field

import wfb_store as store

META_FIELDS = store.META_FIELDS


@dataclass
class CaptureConfig:
    db: str = store.DEFAULT_DB
    listen: int = 6700
    bind: str = "127.0.0.1"
    source: str = "live-gs"
    commit_every: int = 20
    commit_secs: float = 2.0
    idle_timeout: float = 0.0          # >0: standalone auto-close after idle (webui: 0)
    max_duration: float = 1200.0       # roll a fresh session at this age (0 = unbounded)
    stats_every: float = 5.0           # stderr log cadence (0 = silent)
    meta: dict = field(default_factory=dict)   # META_FIELDS values stamped on each session


def _log(msg: str) -> None:
    print(f"[capture] {msg}", file=sys.stderr, flush=True)


class Capture:
    """A UDP telemetry ingester. Call run() to drive it in the current thread,
    or start()/stop() to drive it in a background thread."""

    def __init__(self, cfg: CaptureConfig):
        self.cfg = cfg
        self._stop = threading.Event()
        self._roll = threading.Event()      # request: close + roll a fresh session
        self._lock = threading.Lock()       # guards _pending_duration
        self._pending_duration: float | None = None
        self._thread: threading.Thread | None = None
        # status snapshot — written by the capture thread, read by others. Plain
        # attribute reads are GIL-atomic; a status() snapshot may be momentarily
        # mixed but never corrupt, which is fine for display/health.
        self.running = False
        self.bind_error: str | None = None
        self.session_id: int | None = None
        self.session_start = 0.0
        self.records = 0
        self.bad = 0
        self.max_duration = cfg.max_duration

    # ---- control (thread-safe; callable from any thread) ------------------

    def start(self) -> None:
        """Spawn the capture loop in a daemon background thread."""
        self._thread = threading.Thread(target=self.run, name="wfb-capture", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Signal the loop to close its session and exit; join if backgrounded."""
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=6)

    def roll(self, duration: float | None = None) -> None:
        """Request a clean session roll: close the current session now, open a
        fresh one on the next datagram. Optional new max-duration. Safe to call
        from another thread — the loop does the actual DB work."""
        with self._lock:
            self._pending_duration = duration
        self._roll.set()

    def status(self) -> dict:
        age = (time.monotonic() - self.session_start) if self.session_id is not None else 0.0
        return {
            "running": self.running,
            "bind_error": self.bind_error,
            "session_id": self.session_id,
            "records": self.records,
            "bad": self.bad,
            "age_s": round(age, 1),
            "max_duration": self.max_duration,
            "listen": self.cfg.listen,
            "source": self.cfg.source,
        }

    # ---- the loop ---------------------------------------------------------

    def run(self) -> None:
        cfg = self.cfg
        conn = store.connect(cfg.db)
        store.init_db(conn)  # idempotent

        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            rx.bind((cfg.bind, cfg.listen))
        except OSError as e:
            # Port already held (e.g. a standalone ingester is running). Don't
            # crash — in the one-app deploy the UI must keep serving read-only.
            self.bind_error = f"bind {cfg.bind}:{cfg.listen} failed: {e}"
            _log(self.bind_error + " — capture disabled (UI read-only)")
            rx.close()
            conn.close()
            return
        # A short timeout keeps the loop responsive to stop()/roll() and the
        # max-duration deadline regardless of datagram rate.
        rx.settimeout(1.0)

        self.running = True
        _log(f"listening udp {cfg.bind}:{cfg.listen} -> {cfg.db} (source={cfg.source})")

        pend = 0
        last_commit = last_log = time.monotonic()
        last_data = time.monotonic()
        win = 0

        def commit():
            nonlocal pend, last_commit
            if pend:
                conn.commit()
                pend = 0
            last_commit = time.monotonic()

        def close_current():
            nonlocal win
            if self.session_id is not None:
                commit()
                store.close_session(conn, self.session_id)
                _log(f"closed session {self.session_id}: {self.records} records ({self.bad} bad)")
                self.session_id = None
                win = 0

        try:
            while not self._stop.is_set():
                data = None
                try:
                    data, _ = rx.recvfrom(65535)
                    last_data = time.monotonic()
                except socket.timeout:
                    pass

                if data is not None:
                    try:
                        rec = json.loads(data)
                    except (ValueError, UnicodeDecodeError):
                        rec = None
                        self.bad += 1
                    if rec is not None:
                        if self.session_id is None:
                            self.session_id = store.create_session(conn, cfg.source, **cfg.meta)
                            self.session_start = time.monotonic()
                            self.records = 0
                            self.bad = 0
                            _log(f"opened session {self.session_id}")
                        store.insert_record(conn, self.session_id, rec)
                        self.records += 1
                        pend += 1
                        win += 1

                now = time.monotonic()

                # Explicit roll request from another thread (e.g. the New-capture
                # button). Pick up an optional new max-duration, then close — the
                # next datagram opens the fresh session.
                if self._roll.is_set():
                    with self._lock:
                        nd = self._pending_duration
                        self._pending_duration = None
                    self._roll.clear()
                    if nd is not None:
                        self.max_duration = nd
                    if self.session_id is not None:
                        _log(f"session {self.session_id} rolled on request")
                    close_current()

                # Standalone idle auto-close (webui leaves idle_timeout=0).
                if (cfg.idle_timeout > 0 and self.session_id is not None
                        and (now - last_data) >= cfg.idle_timeout):
                    _log(f"idle {cfg.idle_timeout:.0f}s — closing session")
                    break

                # Hard session cap: roll so files stay bounded. The vehicle keeps
                # logging continuously; LOG_SYNC re-attributes across the boundary.
                if (self.session_id is not None and self.max_duration > 0
                        and (now - self.session_start) >= self.max_duration):
                    _log(f"session {self.session_id} hit max-duration "
                         f"{self.max_duration:.0f}s — rolled")
                    close_current()

                if pend >= cfg.commit_every or (now - last_commit) >= cfg.commit_secs:
                    commit()
                if cfg.stats_every > 0 and win > 0 and (now - last_log) >= cfg.stats_every:
                    rate = win / (now - last_log)
                    _log(f"session {self.session_id}: {self.records} records "
                         f"({rate:.1f}/s, {self.bad} bad)")
                    win = 0
                    last_log = now
        finally:
            self.running = False
            try:
                commit()
                if self.session_id is not None:
                    store.close_session(conn, self.session_id)
                    _log(f"closed session {self.session_id}: {self.records} records ({self.bad} bad)")
                    self.session_id = None
            finally:
                rx.close()
                conn.close()
