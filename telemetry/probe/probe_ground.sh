#!/bin/bash
# probe_ground.sh -- GROUND side of the probe-PER ladder.
#
# For each probed rung: one wfb_rx_native (matching link_id/radio_port, AEAD off,
# -Y stats at 10 Hz) on the RX-ONLY adapter, feeding probe_log.py which computes
# windowed per-rung PER and emits unified probe records into OUT (JSONL the gemma4
# loop ingests). Mirror of probe_drone.sh; same link_id and rung ports.
#
# Usage:  sudo ./probe_ground.sh <cur_mcs> [link_id] [out.jsonl]
#   e.g.  sudo ./probe_ground.sh 2 50 telemetry/loop/probe.jsonl
#
# IMPORTANT (bench-validated): use the RX-ONLY adapter (wlx...229b here) -- the
# other ground adapter is shared with the uplink wfb_tx_native and a 2nd RX there
# contends. wfb_rx_native needs -x (AEAD off) to match the probe TX.
set -u

CUR_MCS="${1:?usage: probe_ground.sh <cur_mcs> [link_id] [out.jsonl]}"
LINK_ID="${2:-50}"
OUT="${3:-}"
KEY="${KEY:-/etc/drone.key}"
ADAPTER="${ADAPTER:-wlx40a5ef2f229b}"           # RX-only adapter
# NOTE: absolute path -- under sudo $HOME=/root, so do NOT derive from $HOME.
RX="${RX:-/home/snokvist/dev/waybeam-coordination/waybeam_wfb_ng/wfb-ng/build/wfb_rx_native}"
STATS_BASE="${STATS_BASE:-5850}"                # rung r -> -Y 127.0.0.1:STATS_BASE+r
# CRITICAL: wfb_rx defaults its decoded-payload forward to 127.0.0.1:5600 -- the
# RTP VIDEO port. Without an explicit -u, accepted probe packets get injected into
# the live H.265 decoder and flap the video (root-caused 2026-06-10). The probe
# only needs -Y rx_ant stats; the forwarded payload is unused, so send it to a
# dead local port (one per rung, off 5600). NEVER let this default to 5600.
CLIENT_BASE="${CLIENT_BASE:-5751}"              # rung r -> -u CLIENT_BASE+r
HERE="$(cd "$(dirname "$0")" && pwd)"

RUNGS="$CUR_MCS $((CUR_MCS + 1))"               # match probe_drone.sh

PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT INT TERM

rung_args=""
r=0
for M in $RUNGS; do
    P=$((LINK_ID + r))
    SP=$((STATS_BASE + r))
    CP=$((CLIENT_BASE + r))
    echo "[probe-gs] rung MCS=$M radio_port=$P stats=127.0.0.1:$SP fwd=127.0.0.1:$CP"
    "$RX" -K "$KEY" -i "$LINK_ID" -p "$P" -c 127.0.0.1 -u "$CP" \
          -x -Y "127.0.0.1:$SP" -l 100 "$ADAPTER" \
          >"/tmp/rxgs_$P.log" 2>&1 &
    PIDS="$PIDS $!"
    rung_args="$rung_args --rung $P:$M:$SP"
    r=$((r + 1))
done

sleep 1
out_arg=""; [ -n "$OUT" ] && out_arg="--out $OUT"
echo "[probe-gs] starting probe_log $rung_args $out_arg"
python3 "$HERE/probe_log.py" $rung_args --window-s 1.0 $out_arg &
PIDS="$PIDS $!"
wait
