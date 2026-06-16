#!/bin/bash
# ht40_host_test.sh — HT40 live-switch + budget verification, GS host half.
#
# Proven on the dev x86 GS (dual RTL88x2, live wfb-gs) 2026-06-16:
#   iw HT40+ both cards -> width 40 MHz; SET_RADIO bandwidth=40 -> tx bw=40 (no crash);
#   native dry-run link_controller phy_mbps 13.0 -> 27.3 (2.10x) at MCS1.
# ALWAYS reverts to HT20 on exit (trap), so it is safe to Ctrl-C.
#
# This is the GS-side half of the gs+air combo test in ../requirements.md §3.
# For the full kbps-doubling readout you need a live vehicle downlink so the FEC
# controller commits k/n (probe + sidecar + rx_ant stats) — see §2.3.
#
# Prereqs:
#   - a running wfb-gs supervisor with a tx tunnel that has a control port (-C)
#   - native controller built:  make -C ../../.. -C vehicle host   (link_controller.host)
#   - iw needs root: run under sudo or with passwordless sudo -n available
#
# Edit these for your host:
CARDS="${CARDS:-wlx40a5ef2f229b wlx40a5ef2f2308}"   # monitor interfaces to retune
CHAN="${CHAN:-161}"                                  # channel (ch161 -> HT40+)
HTPLUS="${HTPLUS:-HT40+}"                            # HT40+ or HT40- (channel-dependent)
TUNNEL="${TUNNEL:-uplink}"                           # tx tunnel name in the supervisor
SUP_BASE="${SUP_BASE:-http://127.0.0.1/api/v1/tunnels}"
LC="${LC:-./build/link_controller.host}"            # run from waybeam_wfb_ng/vehicle
WFB_CTRL="${WFB_CTRL:-127.0.0.1:8000}"              # tx control endpoint for the dry-run LC
APIPORT="${APIPORT:-8799}"

set -u
SUP="$SUP_BASE/$TUNNEL"
LCPID=""
reverted=0
revert() {
  [ "$reverted" = 1 ] && return; reverted=1
  echo "--- REVERT -> HT20 ---"
  curl -s --max-time 4 "$SUP/control?cmd=set_radio&bandwidth=20" >/dev/null 2>&1
  for c in $CARDS; do sudo -n iw dev "$c" set channel "$CHAN" 2>/dev/null; done
  [ -n "$LCPID" ] && kill "$LCPID" 2>/dev/null
  sleep 1
  curl -s --max-time 4 "$SUP" 2>/dev/null | python3 -c 'import sys,json;d=json.load(sys.stdin);print("post-revert tx: bw=%s state=%s"%(d["radio"]["bw"],d["state"]))' 2>/dev/null
  for c in $CARDS; do printf '  %s: ' "$c"; iw dev "$c" info 2>/dev/null | grep -i width; done
}
trap revert EXIT INT TERM

read_phy() {  # start dry-run LC, read radio.phy_mbps/bw from /status, kill
  $LC --dry-run --wfb "$WFB_CTRL" --no-mcs --no-log \
      --api-port "$APIPORT" --stats 127.0.0.1:59999 >/tmp/lc_ht40.log 2>&1 &
  LCPID=$!
  local v="" i
  for i in $(seq 1 10); do
    sleep 0.5
    v=$(curl -s --max-time 2 "http://127.0.0.1:$APIPORT/status" 2>/dev/null \
        | python3 -c 'import sys,json;d=json.load(sys.stdin);r=d.get("radio",{});print("phy_mbps=%s bw=%s"%(r.get("phy_mbps"),r.get("bw")))' 2>/dev/null)
    case "$v" in phy_mbps=None*|"") : ;; *) break;; esac
  done
  kill "$LCPID" 2>/dev/null; LCPID=""; sleep 0.4
  echo "$v"
}

echo "=== BASELINE (HT20) ==="
for c in $CARDS; do printf '%s: ' "$c"; iw dev "$c" info | grep -i width; done
curl -s --max-time 4 "$SUP" | python3 -c 'import sys,json;d=json.load(sys.stdin);print("tx radio: bw=%s mcs=%s"%(d["radio"]["bw"],d["radio"]["mcs"]))'
echo "controller @HT20: $(read_phy)"

echo
echo "=== SWITCH -> $HTPLUS (iw both cards first, then SET_RADIO bandwidth=40) ==="
for c in $CARDS; do printf 'iw %s %s %s: ' "$c" "$CHAN" "$HTPLUS"; sudo -n iw dev "$c" set channel "$CHAN" "$HTPLUS" 2>&1 && echo ok; done
curl -s --max-time 4 "$SUP/control?cmd=set_radio&bandwidth=40" >/dev/null; sleep 1
for c in $CARDS; do printf '%s: ' "$c"; iw dev "$c" info | grep -i width; done
curl -s --max-time 4 "$SUP" | python3 -c 'import sys,json;d=json.load(sys.stdin);print("tx radio: bw=%s mcs=%s state=%s"%(d["radio"]["bw"],d["radio"]["mcs"],d["state"]))'
echo "controller @$HTPLUS: $(read_phy)"

echo
echo "Expect phy_mbps to ~2.1x (e.g. 13.0 -> 27.3 at MCS1). Revert follows on exit."
