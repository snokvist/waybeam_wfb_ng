#!/bin/sh
# /usr/sbin/wfb-ng.sh - procd foreground supervisor for waybeam wfb-ng on CPE510
# Reads active profile from /etc/waybeam/current. For "wfb" profile, runs the
# profile's post-up.sh to create wlan0mon on phy0, then runs wfb_rx and wfb_tx
# in the background and waits. On signal, kills children and tears down wlan0mon.

set -u

PROFILE_DIR="/etc/waybeam"
CURRENT="$PROFILE_DIR/current"
KEY="/etc/drone.key"
IFACE="wlan0mon"
LOG_TAG="[wfb-ng]"

log() { echo "$LOG_TAG $*"; }

active_profile() {
    [ -r "$CURRENT" ] || { echo ""; return; }
    tr -d '[:space:]' < "$CURRENT"
}

cleanup() {
    log "stopping (signal received)"
    [ -n "${PID_RX:-}" ] && kill "$PID_RX" 2>/dev/null
    [ -n "${PID_TX:-}" ] && kill "$PID_TX" 2>/dev/null
    sleep 0.3
    killall -q wfb_rx wfb_tx 2>/dev/null
    iw dev "$IFACE" del 2>/dev/null
    log "stopped"
    exit 0
}

trap cleanup TERM INT HUP

PROFILE="$(active_profile)"
log "profile=${PROFILE:-<empty>}"

if [ "$PROFILE" != "wfb" ]; then
    log "wbmode is not 'wfb' (got '${PROFILE}'); idling"
    # Stay alive so procd doesn't respawn us in a hot loop, but do nothing.
    while :; do sleep 3600; done
fi

POST_UP="$PROFILE_DIR/wfb/post-up.sh"
if [ ! -x "$POST_UP" ]; then
    log "ERROR: $POST_UP not executable; cannot bring up $IFACE"
    exit 1
fi

log "running $POST_UP"
"$POST_UP" || { log "ERROR: post-up.sh failed"; exit 1; }

# Sanity check: iface must exist
if ! iw dev | grep -qw "$IFACE"; then
    log "ERROR: $IFACE not present after post-up.sh"
    exit 1
fi

if [ ! -r "$KEY" ]; then
    log "ERROR: $KEY missing or unreadable"
    exit 1
fi

log "starting wfb_rx (link 207) -> 192.168.2.20:5600, stats -> 127.0.0.1:5801"
wfb_rx -K "$KEY" -c 192.168.2.20 -u 5600 -i 207 -p 0 -l 100 -x \
       -Y 127.0.0.1:5801 "$IFACE" &
PID_RX=$!

log "starting wfb_tx (link 208) <- udp 5801, control 8000"
wfb_tx -K "$KEY" -M 1 -B 20 -k 1 -n 2 -P 1 -Q -S 1 -L 1 -C 8000 \
       -u 5801 -R 2097152 -s 2097152 -l 1000 -i 208 -p 0 "$IFACE" &
PID_TX=$!

log "started rx=$PID_RX tx=$PID_TX on $IFACE"

# Exit if either child dies; procd will respawn us.
while kill -0 "$PID_RX" 2>/dev/null && kill -0 "$PID_TX" 2>/dev/null; do
    sleep 2
done

log "a child exited (rx alive=$(kill -0 $PID_RX 2>/dev/null && echo y || echo n) tx alive=$(kill -0 $PID_TX 2>/dev/null && echo y || echo n))"
cleanup
