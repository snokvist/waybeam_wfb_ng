#!/bin/bash
# probe_ground_swept.sh -- GROUND side for the SINGLE swept-TX probe.
#
# One wfb_rx_native on the RX-only adapter (matching the swept TX's link_id /
# radio_port, AEAD off, -Y stats at 10 Hz) feeding probe_log.py in --by-mcs mode,
# so the swept ladder shows up as one clean PER record per received MCS per window.
# Pairs with probe_drone_swept.sh (same LINK_ID / RADIO_PORT). Mirror for the
# N-parallel form is probe_ground.sh.
#
# Usage:  sudo ./probe_ground_swept.sh <cur_mcs> [link_id] [out.jsonl]
#   e.g.  sudo ./probe_ground_swept.sh 2 50 telemetry/loop/probe_walk.jsonl
#
# Bench-validated: use the RX-ONLY adapter (wlx...229b) -- the other ground adapter
# is shared with the uplink wfb_tx_native and a 2nd RX there contends. -x (AEAD
# off) matches the probe TX. Under sudo $HOME=/root, so binary paths are ABSOLUTE.
set -u

CUR_MCS="${1:?usage: probe_ground_swept.sh <cur_mcs> [link_id] [out.jsonl]}"
LINK_ID="${2:-50}"
OUT="${3:-}"
KEY="${KEY:-/etc/drone.key}"
ADAPTER="${ADAPTER:-wlx40a5ef2f229b}"           # RX-only adapter
# NOTE: absolute path -- under sudo $HOME=/root, so do NOT derive from $HOME.
RX="${RX:-/home/snokvist/dev/waybeam-coordination/waybeam_wfb_ng/wfb-ng/build/wfb_rx_native}"
RADIO_PORT="${RADIO_PORT:-50}"                  # must match probe_drone_swept.sh
STATS_PORT="${STATS_PORT:-5850}"
# CRITICAL: wfb_rx defaults its decoded-payload forward to 127.0.0.1:5600 -- the
# RTP VIDEO port. Without an explicit -u, accepted probe packets get injected into
# the live H.265 decoder and flap the video (root-caused 2026-06-10). The probe
# only needs -Y rx_ant stats; the forwarded payload is unused, so send it to a
# dead local port. NEVER let this default to 5600.
CLIENT_PORT="${CLIENT_PORT:-5751}"
HERE="$(cd "$(dirname "$0")" && pwd)"

PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT INT TERM

echo "[probe-gs] swept RX: link_id=$LINK_ID radio_port=$RADIO_PORT stats=127.0.0.1:$STATS_PORT"
"$RX" -K "$KEY" -i "$LINK_ID" -p "$RADIO_PORT" -c 127.0.0.1 -u "$CLIENT_PORT" \
      -x -Y "127.0.0.1:$STATS_PORT" -l 100 "$ADAPTER" \
      >"/tmp/rxgs_swept_$RADIO_PORT.log" 2>&1 &
PIDS="$PIDS $!"

sleep 1
out_arg=""; [ -n "$OUT" ] && out_arg="--out $OUT"
echo "[probe-gs] starting probe_log --by-mcs (nominal cur=$CUR_MCS) $out_arg"
python3 "$HERE/probe_log.py" --rung "$RADIO_PORT:$CUR_MCS:$STATS_PORT" --by-mcs \
        --window-s 1.0 $out_arg &
PIDS="$PIDS $!"
wait
