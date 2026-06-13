#!/bin/bash
# import_vehicle_session.sh — Phase 2: pull a vehicle walkout session over the
# management link (post-walk, at close range — zero flight-time link burden) and
# import its link_controller status.jsonl into wfb.sqlite as a vehicle-uplink
# session, correlated to the GS downlink capture by the vehicle's monotonic
# session filename. See WALKOUT_TELEMETRY_PLAN.md Phase 2.
#
# Usage:
#   import_vehicle_session.sh <vehicle_ip> [seq|latest] [-- extra import args]
# Examples:
#   import_vehicle_session.sh 192.168.1.13              # newest session
#   import_vehicle_session.sh 192.168.1.13 000002       # a specific one
#   import_vehicle_session.sh 192.168.1.13 latest -- --scenario range-walk

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-$HERE/.venv/bin/python}"; [ -x "$PY" ] || PY=python3
DB="${WFB_DB:-$HERE/wfb.sqlite}"
WALKOUT="${VEHICLE_WALKOUT_DIR:-/mnt/mmcblk0p1/walkout}"
SSH="ssh -o ConnectTimeout=10"

IP="${1:?usage: import_vehicle_session.sh <vehicle_ip> [seq|latest] [-- extra args]}"
SEL="${2:-latest}"
shift || true; shift 2>/dev/null || true
[ "${1:-}" = "--" ] && shift   # remaining args pass through to the python importer

# Resolve the session directory name on the vehicle.
if [ "$SEL" = "latest" ]; then
    DIR=$($SSH "root@$IP" "ls -1d $WALKOUT/[0-9][0-9][0-9][0-9][0-9][0-9]_*/ 2>/dev/null | sort | tail -1")
else
    DIR=$($SSH "root@$IP" "ls -1d $WALKOUT/${SEL}_*/ 2>/dev/null | sort | tail -1")
fi
DIR="${DIR%/}"
[ -n "$DIR" ] || { echo "no walkout session matching '$SEL' on $IP:$WALKOUT" >&2; exit 1; }

BASE=$(basename "$DIR")          # e.g. 000002_20260316_211413_855
SEQ="${BASE%%_*}"                # e.g. 000002 — the correlation key
TMP="${TMPDIR:-/tmp}/vehicle_${SEQ}_status.jsonl"

echo "pulling $IP:$DIR/status.jsonl  (session $SEQ) ..."
scp -O -o ConnectTimeout=10 "root@$IP:$DIR/status.jsonl" "$TMP"

"$PY" "$HERE/import_vehicle_session.py" "$TMP" --db "$DB" --vehicle-session "$SEQ" "$@"
echo "imported vehicle session $SEQ into $DB"
