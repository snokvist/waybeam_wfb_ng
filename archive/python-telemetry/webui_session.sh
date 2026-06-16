#!/bin/bash
# webui_session — start/stop the CONSOLIDATED telemetry app (capture + web UI in
# ONE process), wired into gs_supervisor system.up/down (see DATASTORE.md).
#
# This is the one-app replacement for capture_session.sh: instead of a headless
# ingester (capture only) plus a separately-launched UI, webapp.py captures the
# stats_tap (udp:6700) into wfb.sqlite AND serves the web UI from a single
# process. So the whole telemetry stack comes up with gs_supervisor — nothing
# extra to launch.
#
# Topology (additive "stats_tap", NOT an inline tee — identical to before):
#   wfb_rx -Y → gs_supervisor stats_drain():
#       sendto stats_out  127.0.0.1:6600   (uplink back-channel — UNCHANGED)
#       sendto stats_tap  127.0.0.1:6700 → THIS app → telemetry/wfb.sqlite + UI
#
#   "system": { "up":   [ ".../webui_session.sh start [ui_port]" ],
#               "down": [ ".../webui_session.sh stop" ] }
#
# `start` daemonizes and returns immediately — gs_supervisor system.up commands
# run under a ~10 s deadline. `stop` sends SIGTERM so webapp.py's make_server
# shutdown path runs (closes the live session, commits the final window).

# gs_supervisor runs system.up/down via execvp with a minimal PATH, so tools
# like pgrep/tr may not resolve — pin a standard PATH up front.
export PATH="/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
# webapp.py needs Flask, so default to the deploy venv (install.sh creates it).
# Override with PY=/path/to/python for a system-wide Flask install.
PY="${PY:-$HERE/.venv/bin/python}"
[ -x "$PY" ] || PY="python3"
DB="${WFB_DB:-$HERE/wfb.sqlite}"
LISTEN="${INGEST_LISTEN:-6700}"
SOURCE="${INGEST_SOURCE:-live-gs}"
# Hard session cap (s): roll a fresh session at this age so files stay bounded.
# Default 20 min (a typical max flight).
MAX_DURATION="${INGEST_MAX_DURATION:-1200}"
# Objective metadata stamped on every session (the GS config is the source of
# truth for these). Subjective fields stay manual via the web UI editor.
CHANNEL="${INGEST_CHANNEL:-161}"
TXPOWER="${INGEST_TXPOWER:-2000mBm}"
ANTENNA="${INGEST_ANTENNA:-2x RTL88x2 diversity}"
UI_HOST="${UI_HOST:-0.0.0.0}"
# UI port: positional $2 wins (system.up can't use env-prefix syntax), else env,
# else 8080. (On hosts where 8080 is taken, pass it explicitly: `start 8088`.)
UI_PORT="${2:-${UI_PORT:-8080}}"
RUN="${RUN_DIR:-/var/run}"
PIDFILE="${WEBUI_PID:-$RUN/wfb_webui.pid}"
LOG_DIR="${LOG_DIR:-/var/log/waybeam-walkout}"
mkdir -p "$LOG_DIR" 2>/dev/null

alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

# pids of our webapp on this capture port, found via /proc (no pgrep dependency,
# so it works under gs_supervisor's restricted exec environment). A hard
# gs_supervisor kill does NOT run system.down, so the app can orphan (reparents
# to init) and keep holding udp:$LISTEN + the UI port; the next `start` must
# reclaim it or the fresh bind fails.
webui_pids() {
    for d in /proc/[0-9]*; do
        p=${d#/proc/}
        [ "$p" = "$$" ] && continue
        c=$(cat "$d/cmdline" 2>/dev/null | tr '\0' ' ') || continue
        [ -n "$c" ] || continue
        case "$c" in
            # anchor on the trailing " --db" so --listen 6700 can't match 67005
            *"webapp.py --listen $LISTEN --db"*) printf '%s\n' "$p" ;;
        esac
    done
}

kill_webui() {
    # SIGTERM so webapp.py's signal handler runs srv.shutdown()+capturer.stop()
    # → close_session() stamps ended_at + commits the final window.
    [ -f "$PIDFILE" ] && kill -TERM "$(cat "$PIDFILE")" 2>/dev/null
    rm -f "$PIDFILE" 2>/dev/null
    for p in $(webui_pids); do kill -TERM "$p" 2>/dev/null; done
    # ≤8 s for a clean shutdown (Flask make_server + capture join), then escalate
    # any survivor so the UDP + UI ports are always freed for the fresh bind.
    for _ in 1 2 3 4 5 6 7 8; do
        [ -z "$(webui_pids)" ] && return 0
        sleep 1
    done
    for p in $(webui_pids); do kill -TERM "$p" 2>/dev/null; done
    sleep 2
    for p in $(webui_pids); do kill -KILL "$p" 2>/dev/null; done   # last resort
    sleep 1
}

case "${1:-start}" in
    start)
        # Always reclaim the ports and roll a FRESH session per bring-up: an
        # orphan from a prior run would otherwise hold them and keep one
        # sessions row open across multiple walks.
        kill_webui
        # Daemonize; webapp.py opens a sessions row on the first datagram and
        # serves the UI immediately.
        "$PY" "$HERE/webui/webapp.py" --listen "$LISTEN" --db "$DB" \
            --host "$UI_HOST" --port "$UI_PORT" \
            --source "$SOURCE" --channel "$CHANNEL" --tx-power "$TXPOWER" \
            --antenna-cfg "$ANTENNA" --max-duration "$MAX_DURATION" \
            </dev/null >>"$LOG_DIR/webui.log" 2>&1 &
        echo $! > "$PIDFILE"
        echo "webui(gs): app $(cat "$PIDFILE") capture udp:$LISTEN + UI http://$UI_HOST:$UI_PORT -> $DB (max ${MAX_DURATION}s)"
        ;;
    stop)
        kill_webui
        echo "webui(gs): stopped"
        ;;
    status)
        if alive "$PIDFILE"; then
            echo "webui(gs): running ($(cat "$PIDFILE"))"
        else
            echo "webui(gs): not running"
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|status} [ui_port]"; exit 1 ;;
esac
exit 0
