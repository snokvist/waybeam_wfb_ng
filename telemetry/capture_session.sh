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

# gs_supervisor runs system.up/down via execvp with a minimal PATH, so tools
# like pgrep/tr may not resolve — pin a standard PATH up front.
export PATH="/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-python3}"
DB="${WFB_DB:-$HERE/wfb.sqlite}"
LISTEN="${INGEST_LISTEN:-6700}"
SOURCE="${INGEST_SOURCE:-live-gs}"
# Objective metadata stamped on every session so it's self-describing (the GS
# config is the source of truth for these). Subjective fields (scenario /
# location / weather / notes) stay manual via the web UI editor.
CHANNEL="${INGEST_CHANNEL:-161}"
TXPOWER="${INGEST_TXPOWER:-2000mBm}"
ANTENNA="${INGEST_ANTENNA:-2x RTL88x2 diversity}"
RUN="${RUN_DIR:-/var/run}"
PIDFILE="${INGEST_PID:-$RUN/wfb_ingest.pid}"
LOG_DIR="${LOG_DIR:-/var/log/waybeam-walkout}"
mkdir -p "$LOG_DIR" 2>/dev/null

alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

# Stop every ingester on our port — by pidfile AND by command signature. A hard
# gs_supervisor kill does NOT run system.down, so an ingester can orphan (it
# reparents to init) and keep holding udp:$LISTEN; the next `start` must reclaim
# it or the new ingester can't bind. SIGINT (not TERM) so the ingester's
# `finally` runs close_session() — stamps ended_at + commits the final window.
# pids of ingesters on our port, found via /proc (no pgrep dependency, so it
# works under gs_supervisor's restricted exec environment).
ingester_pids() {
    for d in /proc/[0-9]*; do
        p=${d#/proc/}
        [ "$p" = "$$" ] && continue
        c=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null) || continue
        case "$c" in
            *"wfb_ingest.py --listen $LISTEN"*) printf '%s\n' "$p" ;;
        esac
    done
}

kill_ingesters() {
    [ -f "$PIDFILE" ] && kill -INT "$(cat "$PIDFILE")" 2>/dev/null
    rm -f "$PIDFILE" 2>/dev/null
    for p in $(ingester_pids); do kill -INT "$p" 2>/dev/null; done
    # ≤5 s for a clean close_session() exit, then escalate any survivor to TERM
    # so the UDP port is always freed for the fresh bind.
    for _ in 1 2 3 4 5; do
        [ -z "$(ingester_pids)" ] && return 0
        sleep 1
    done
    for p in $(ingester_pids); do kill -TERM "$p" 2>/dev/null; done
    sleep 2
    for p in $(ingester_pids); do kill -KILL "$p" 2>/dev/null; done   # last resort: free the port
    sleep 1
}

case "${1:-start}" in
    start)
        # Always reclaim the port and roll a FRESH session per bring-up: an
        # orphan from a prior run would otherwise hold udp:$LISTEN and keep one
        # sessions row open across multiple walks. (Per-walk keying is Phase 2c.)
        kill_ingesters
        # Daemonize; the ingester opens a sessions row on the first datagram.
        "$PY" "$HERE/wfb_ingest.py" --listen "$LISTEN" --db "$DB" \
            --source "$SOURCE" --channel "$CHANNEL" --tx-power "$TXPOWER" \
            --antenna-cfg "$ANTENNA" </dev/null >>"$LOG_DIR/ingest.log" 2>&1 &
        echo $! > "$PIDFILE"
        echo "capture(gs): ingester $(cat "$PIDFILE") on udp:$LISTEN -> $DB"
        ;;
    stop)
        kill_ingesters
        echo "capture(gs): stopped"
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
