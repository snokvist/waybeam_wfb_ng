#!/bin/sh
# probe_drone_swept.sh -- DRONE side, SINGLE swept-TX probe (preferred design).
#
# Instead of one wfb_tx per rung (probe_drone.sh), run ONE probe wfb_tx with a
# control port (-C) and step its MCS live with `wfb_tx_cmd <ctrl> set_radio`,
# dwelling ~1-2 s per rung and cycling continuously. One paced feeder drives the
# single stream. Lighter on the 8812eu driver than N parallel TXs, and the ground
# probe_log buckets PER by the *received* mcs, so one swept TX yields the whole
# ladder. Drone -> ground (video-headroom direction). See PROBE_PER_SPEC.md sec 8.
#
# Bench-validated rules baked in (gemma4 memory wfb-manual-probe-setup):
#   * MIRROR THE VIDEO PHY (-B 20 -S 1 -L 1), vary ONLY -M. Without LDPC the upper
#     rungs read a false cliff (MCS7 99.6%->0.8% PER once -L 1 was added).
#   * FEC OFF (-k 1 -n 1) so raw loss isn't masked; with 1/1 each lost pkt counts.
#   * PACE the feed (<=20 pps): blasting overruns the wfb_tx UDP input and drops
#     packets silently (PER understated). 20 pps is the spec rate.
#   * Fresh LINK_ID (channel_id = link_id<<8 | radio_port) keeps it off video
#     (i=207/p0) and uplink (i=208/p0). Probe RX on the ground RX-only adapter.
#   * AEAD off (-x); the ground probe RX must also run -x.
#   * Stay in video's 1SS regime: rungs are CLAMPED to MCS 0..7 (8-15 = 2SS).
#   * set_radio is FLAG-based (tx_cmd.c:232), -G omitted = long GI (= video's
#     short_gi=0), -N defaults to 1 -- mirrors video exactly.
#
# Usage:  sh probe_drone_swept.sh <cur_mcs> [link_id] [pps] [secs] ["rung list"]
#   e.g.  sh probe_drone_swept.sh 2              # sweep MCS 2,3,4 @20pps, forever
#         sh probe_drone_swept.sh 2 50 20 30     # 30 s then stop
#         sh probe_drone_swept.sh 2 50 20 0 "2 3"  # boundary-only (cur,cur+1)
#
# Cleanup via trap: the TX + feeder PIDs are killed on exit/INT/TERM so no probe
# tx can leak into production (we hit exactly that leak once).
set -u

CUR_MCS="${1:?usage: probe_drone_swept.sh <cur_mcs> [link_id] [pps] [secs] [rungs]}"
LINK_ID="${2:-50}"
PPS="${3:-20}"
SECS="${4:-0}"                     # 0 = run until killed
RUNGS="${5:-}"                     # explicit MCS list; default = cur,cur+1,cur+2
KEY="${KEY:-/etc/drone.key}"
IFACE="${IFACE:-wlan0}"
BW="${BW:-20}"; STBC="${STBC:-1}"; LDPC="${LDPC:-1}"   # MIRROR VIDEO PHY
PKT_BYTES="${PKT_BYTES:-1400}"
RADIO_PORT="${RADIO_PORT:-50}"     # single radio_port for the swept stream
CTRL_PORT="${CTRL_PORT:-7050}"     # wfb_tx_cmd command port (NOT video's 8000)
UDP_IN="${UDP_IN:-5750}"           # wfb_tx reads udp 127.0.0.1:UDP_IN
DWELL="${DWELL:-2}"                # seconds per rung
WFB_TX="${WFB_TX:-wfb_tx}"
WFB_TX_CMD="${WFB_TX_CMD:-wfb_tx_cmd}"

# Default rung set: current and the two rungs above (headroom). One stream at a
# time, so 3 rungs is still light. Clamp to the 1SS regime (0..7).
[ -z "$RUNGS" ] && RUNGS="$CUR_MCS $((CUR_MCS + 1)) $((CUR_MCS + 2))"
_clamped=""
for M in $RUNGS; do
    if [ "$M" -ge 0 ] && [ "$M" -le 7 ]; then _clamped="$_clamped $M"; fi
done
RUNGS="$_clamped"
[ -z "$RUNGS" ] && { echo "[probe] no valid rungs in 0..7" >&2; exit 1; }
# Launch at the first rung in the set (keeps the initial PHY = first probed MCS).
START_MCS="${RUNGS%% *}"; START_MCS="${START_MCS# }"

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    # The shell feeder spawns one short-lived socat per packet; a kill landing
    # mid-send can orphan it. socat is used ONLY by this probe on the drone, so
    # reap any stray as a backstop (the deploy-target C feeder won't need this).
    killall -q socat 2>/dev/null
}
# INT/TERM must cleanup AND exit: a bare `trap cleanup TERM` reaps the children
# but the shell then RESUMES the `while` loop (POSIX returns from the handler),
# so the swept TX leaks back into production on the next rung. Force exit.
trap 'cleanup; exit 0' INT TERM
trap cleanup EXIT

# Paced PRB feeder: ASCII "PRB<10-digit seq>" + padding to PKT_BYTES, at PPS
# packets/sec, to udp 127.0.0.1:$UDP_IN. (C feeder is the deploy target; this
# shell feeder is the validated prototype -- see PROBE_PER_SPEC.md sec 8.)
feeder() {
    _pad=$(awk "BEGIN{for(i=0;i<$PKT_BYTES-14;i++)printf \"x\"}")
    _n=0; _c=0
    while :; do
        printf 'PRB%010d%s' "$_n" "$_pad" | socat -u - "UDP-SENDTO:127.0.0.1:$UDP_IN"
        _n=$((_n + 1)); _c=$((_c + 1))
        if [ "$_c" -ge "$PPS" ]; then _c=0; sleep 1; fi
    done
}

echo "[probe] single swept TX: link_id=$LINK_ID radio_port=$RADIO_PORT ctrl=$CTRL_PORT"
echo "[probe] PHY: B$BW S$STBC L$LDPC, FEC 1/1, ${PPS}pps; rungs:$RUNGS dwell=${DWELL}s"

"$WFB_TX" -K "$KEY" -i "$LINK_ID" -p "$RADIO_PORT" -C "$CTRL_PORT" \
          -M "$START_MCS" -B "$BW" -S "$STBC" -L "$LDPC" \
          -k 1 -n 1 -x -u "$UDP_IN" -l 1000 "$IFACE" >/dev/null 2>&1 &
PIDS="$PIDS $!"
sleep 1                            # let the TX + control socket come up
feeder &
PIDS="$PIDS $!"

echo "[probe] running. PIDs:$PIDS  (Ctrl-C / trap cleans up)"
elapsed=0
while :; do
    for M in $RUNGS; do
        "$WFB_TX_CMD" "$CTRL_PORT" set_radio -B "$BW" -S "$STBC" -L "$LDPC" -M "$M" \
            >/dev/null 2>&1 || echo "[probe] set_radio -M $M failed" >&2
        sleep "$DWELL"
        if [ "$SECS" -gt 0 ]; then
            elapsed=$((elapsed + DWELL))
            [ "$elapsed" -ge "$SECS" ] && { echo "[probe] done (${elapsed}s)"; exit 0; }
        fi
    done
done
