#!/bin/bash
# capture_session — start/stop the SQLite telemetry ingester for a walkout,
# wired into gs_supervisor system.up/down (see DATASTORE.md).
#
# Topology (additive "stats_tap", NOT an inline tee):
#   wfb_rx -Y → gs_supervisor stats_drain():
#       sendto stats_out  127.0.0.1:6600   (uplink back-channel — UNCHANGED)
#       sendto stats_tap  127.0.0.1:6700 → this ingester → telemetry/wfb.sqlite
#
# The tap is a pure fire-and-forget copy emitted AFTER the back-channel
# forward, so the ingester (or the DB) stalling/dying can never affect the
# vehicle link. No tee process sits in the stats path — gs_supervisor emits the
# second copy itself. The supervisor's config gives the video tunnel
# "stats_tap": "127.0.0.1:6700".
#
# `start` daemonizes and returns immediately — gs_supervisor system.up commands
# run under a ~10 s deadline.

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-python3}"
DB="${WFB_DB:-$HERE/wfb.sqlite}"
LISTEN="${INGEST_LISTEN:-6700}"
SOURCE="${INGEST_SOURCE:-live-gs}"
RUN="${RUN_DIR:-/var/run}"
PIDFILE="${INGEST_PID:-$RUN/wfb_ingest.pid}"
LOG_DIR="${LOG_DIR:-/var/log/waybeam-walkout}"
mkdir -p "$LOG_DIR" 2>/dev/null

alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

case "${1:-start}" in
    start)
        if alive "$PIDFILE"; then
            echo "capture(gs): already running ($(cat "$PIDFILE"))"
            exit 0
        fi
        # Daemonize; the ingester opens a sessions row on the first datagram.
        "$PY" "$HERE/wfb_ingest.py" --listen "$LISTEN" --db "$DB" \
            --source "$SOURCE" </dev/null >>"$LOG_DIR/ingest.log" 2>&1 &
        echo $! > "$PIDFILE"
        echo "capture(gs): ingester $(cat "$PIDFILE") on udp:$LISTEN -> $DB"
        ;;
    stop)
        # SIGINT (not TERM) so the ingester's `finally` runs close_session()
        # — sets ended_at and commits the final window.
        if [ -f "$PIDFILE" ]; then
            kill -INT "$(cat "$PIDFILE")" 2>/dev/null
            rm -f "$PIDFILE"
            echo "capture(gs): stopped"
        else
            echo "capture(gs): not running"
        fi
        ;;
    status)
        if alive "$PIDFILE"; then
            echo "capture(gs): running ($(cat "$PIDFILE"))"
        else
            echo "capture(gs): not running"
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|status}"; exit 1 ;;
esac
exit 0
