#!/bin/sh
#
# install.sh — stand up the consolidated wfb telemetry app (capture + web UI)
# as a service. One command: makes a venv, installs Flask, and installs the
# right service unit for this host's init system (systemd or BusyBox).
#
#   telemetry/deploy/install.sh            # defaults: UI :8080, capture udp:6700
#   PORT=9000 LISTEN=6700 telemetry/deploy/install.sh
#
# The app captures the gs_supervisor stats_tap into wfb.sqlite AND serves the
# UI from ONE process — no separate ingester, no sudo rule.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)          # telemetry/
PY="${PYTHON:-python3}"
VENV="$HERE/.venv"
DB="${WFB_DB:-$HERE/wfb.sqlite}"
HOST="${HOST:-0.0.0.0}"
PORT="${PORT:-8080}"
LISTEN="${LISTEN:-6700}"

echo "[install] venv + Flask -> $VENV"
[ -d "$VENV" ] || "$PY" -m venv "$VENV"
"$VENV/bin/pip" install -q -r "$HERE/requirements-webui.txt"

RUN="$VENV/bin/python webui/webapp.py --host $HOST --port $PORT --db $DB --listen $LISTEN"

if command -v systemctl >/dev/null 2>&1; then
    UNIT=/etc/systemd/system/wfb-telemetry.service
    echo "[install] systemd unit -> $UNIT"
    cat > "$UNIT" <<EOF
[Unit]
Description=Waybeam wfb telemetry capture + web UI
After=network.target

[Service]
Type=simple
WorkingDirectory=$HERE
ExecStart=$RUN
Restart=on-failure
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=12

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --now wfb-telemetry.service
    echo "[install] started — journalctl -u wfb-telemetry -f"
else
    INIT=/etc/init.d/S47wfb-telemetry
    echo "[install] BusyBox init -> $INIT"
    sed -e "s#@HERE@#$HERE#g" -e "s#@RUN@#$RUN#g" \
        "$HERE/deploy/S47wfb-telemetry" > "$INIT"
    chmod +x "$INIT"
    "$INIT" restart || "$INIT" start
    echo "[install] started — $INIT status"
fi

echo
echo "[install] UI:      http://<this-host>:$PORT"
echo "[install] capture: udp:$LISTEN -> $DB (in-process)"
echo "[install] NOTE: this app now OWNS udp:$LISTEN. If gs_supervisor's"
echo "          system.up still runs 'capture_session.sh start', remove it"
echo "          (or run this app with --no-capture) so they don't fight for the port."
