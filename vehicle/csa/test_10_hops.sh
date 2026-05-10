#!/bin/bash
# Sequenced 10-hop CSA test to validate consistency.
# Pre-condition: csa_agent is running on both vehicle (5801, wlan0)
# and cpe510 (5802, wlan0mon, --no-revert). link_controller is stopped.
set -u

CPE=${CPE_HOST:-192.168.2.2}
VEH=${VEH_HOST:-192.168.1.13}
HERE="$(cd "$(dirname "$0")" && pwd)"
MUX="$HERE/csa_mux.py"

# (target_chan, target_ht, prev_chan, prev_ht)
HOPS=(
    "149 HT20  161 HT40+"
    "153 HT20  149 HT20"
    "157 HT20  153 HT20"
    "161 HT20  157 HT20"
    "153 HT40+ 161 HT20"
    "161 HT40+ 153 HT40+"
    "149 HT20  161 HT40+"
    "161 HT40+ 149 HT20"
    "157 HT40+ 161 HT40+"
    "161 HT40+ 157 HT40+"
)

PASS=0; FAIL=0
RESULTS=()
i=0
for hop in "${HOPS[@]}"; do
    i=$((i+1))
    read -r tc tht pc pht <<<"$hop"
    echo
    echo "===== HOP $i/10: ch${pc} ${pht} -> ch${tc} ${tht} ====="
    python3 "$MUX" --target-chan "$tc" --target-ht "$tht" \
                   --prev-chan "$pc"   --prev-ht "$pht" \
                   --lead-ms 1000 --t-revert-ms 5000 2>&1 | grep -E "sent|sess|seq=4"

    # Allow up to 3s for switch + verify
    sleep 3

    veh_ch=$(ssh -o ConnectTimeout=2 root@$VEH "iw dev wlan0 info | awk '/channel/{print \$2}'")
    veh_w=$(ssh -o ConnectTimeout=2 root@$VEH "iw dev wlan0 info | awk -F'width: ' '/width:/{split(\$2,a,\" \"); print a[1]}'")
    cpe_ch=$(ssh -o ConnectTimeout=2 root@$CPE "iw dev wlan0mon info | awk '/channel/{print \$2}'")
    cpe_w=$(ssh -o ConnectTimeout=2 root@$CPE "iw dev wlan0mon info | awk -F'width: ' '/width:/{split(\$2,a,\" \"); print a[1]}'")

    expect_w="20"
    case "$tht" in HT40*) expect_w="40";; esac

    veh_ok="FAIL"; cpe_ok="FAIL"
    [ "$veh_ch" = "$tc" ] && [ "$veh_w" = "$expect_w" ] && veh_ok="PASS"
    [ "$cpe_ch" = "$tc" ] && [ "$cpe_w" = "$expect_w" ] && cpe_ok="PASS"

    line=$(printf "hop %2d: target=ch%s w%s | veh=ch%s w%s [%s] cpe=ch%s w%s [%s]" \
        "$i" "$tc" "$expect_w" "$veh_ch" "$veh_w" "$veh_ok" "$cpe_ch" "$cpe_w" "$cpe_ok")
    RESULTS+=("$line")
    echo "$line"
    if [ "$veh_ok" = "PASS" ] && [ "$cpe_ok" = "PASS" ]; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
done

echo
echo "===== SUMMARY ====="
for r in "${RESULTS[@]}"; do echo "$r"; done
echo "passed=$PASS  failed=$FAIL"
