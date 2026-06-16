#!/bin/bash
# HT40 gs+air COMBO test — live link, vehicle link_controller budget doubling.
# Switch-up order: iw HT40+ both ends FIRST, then WCMD wfb_bandwidth=40 (radiotap).
# Switch-down (revert): WCMD wfb_bandwidth=20 FIRST, then iw HT20 both ends.
# Auto-reverts on ANY exit. No process restarts (all live SET_RADIO / iw).
set -u
GS_CARDS="wlx40a5ef2f229b wlx40a5ef2f2308"
VEH=192.168.1.13
CH=161
SUP=http://127.0.0.1            # GS supervisor
SSH="ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no root@$VEH"

veh_status() { timeout 8 $SSH 'curl -s --max-time 4 http://127.0.0.1:8765/status' 2>/dev/null; }
show() { # $1=label  ; reads vehicle /status, prints the budget line
  veh_status | python3 -c '
import sys,json
try: d=json.loads(sys.stdin.read())
except: print("  '"$1"': (no status)"); sys.exit()
r=d.get("radio",{}); w=d.get("wfb",{}); m=d.get("mcs",{}); s=d.get("score",{})
print("  '"$1"': bw=%s mcs=%s phy=%s | safe_kbps=%s set_kbps=%s k/n=%s/%s | rssi=%.1f lost=%.4f recov=%.4f mcs_flap=%s" % (
  r.get("bw"), r.get("mcs"), r.get("phy_mbps"),
  w.get("computed_safe_kbps"), w.get("last_bitrate_kbps"),
  w.get("last_set_fec_k"), w.get("last_set_fec_n"),
  s.get("smoothed_rssi",0), s.get("smoothed_lost_ratio",0), s.get("smoothed_recov_ratio",0),
  (m.get("flap_guard") or {}).get("frozen")))'
}
gs_width() { for c in $GS_CARDS; do printf "    %s: " "$c"; iw dev "$c" info 2>/dev/null | grep -oiE "width: [0-9]+ MHz.*center1: [0-9]+"; done; }
veh_width() { timeout 8 $SSH 'iw dev wlan0 info | grep -oiE "width: [0-9]+ MHz.*center1: [0-9]+"' 2>/dev/null | sed 's/^/    wlan0: /'; }

reverted=0
revert() {
  [ "$reverted" = 1 ] && return; reverted=1
  echo; echo "=== REVERT -> HT20 (WCMD bw=20 first, then iw) ==="
  curl -s --max-time 4 "$SUP/api/v1/cmd?key=wfb_bandwidth&value=20" >/dev/null 2>&1
  sleep 1.5
  timeout 8 $SSH "iw dev wlan0 set channel $CH" 2>/dev/null
  for c in $GS_CARDS; do sudo -n iw dev "$c" set channel $CH 2>/dev/null; done
  sleep 1.5
  echo "  post-revert:"; show "REVERTED"; echo "  widths:"; veh_width; gs_width
}
trap revert EXIT INT TERM

echo "=== 1. BASELINE (HT20) ==="
show "HT20"; echo "  widths:"; veh_width; gs_width

echo; echo "=== 2-3. iw HT40+ (vehicle wlan0 + both GS cards) ==="
timeout 8 $SSH "iw dev wlan0 set channel $CH HT40+" 2>&1 | sed 's/^/  veh: /'
for c in $GS_CARDS; do printf "  gs %s: " "$c"; sudo -n iw dev "$c" set channel $CH HT40+ 2>&1 && echo ok; done
sleep 1; echo "  widths after iw:"; veh_width; gs_width

echo; echo "=== 4. WCMD wfb_bandwidth=40 (radiotap, via GS uplink) ==="
R=$(curl -s --max-time 4 "$SUP/api/v1/cmd?key=wfb_bandwidth&value=40"); echo "  emit resp: $R"

echo; echo "=== 5-6. OBSERVE / SOAK (poll vehicle /status) ==="
for i in 1 2 3 4 5 6 7 8; do sleep 4; show "t+$((i*4))s"; done

echo; echo "(revert via trap follows)"
