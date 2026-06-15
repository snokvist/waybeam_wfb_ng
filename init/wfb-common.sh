# wfb-common.sh — shared helpers for the Waybeam wfb init scripts.
#
# Sourced (never executed) by the per-side startup scripts:
#   vehicle/init/S99wfb        (air, SigmaStar Infinity6E)
#   ground/init/S46gs_supervisor (ground, RK3566 / x86)
#
# Install: copy to /etc/wfb-common.sh on both images alongside the mega binary
# (wfb-air / wfb-gs). The scripts source it by absolute path and fall back to
# inline definitions if it is absent, so an image without it still boots.
#
# Keep this POSIX sh (BusyBox ash) — no bashisms.

# init's inherited PATH is minimal; the scripts shell out to iw/ip/json_cli/
# start-stop-daemon/pidof. Pin a full one.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

WFB_LOG_FILE="${WFB_LOG_FILE:-/tmp/wfb.log}"

# log MSG — timestamp-free tee to the shared log + stdout (init console).
log() { echo "[wfb] $*" | tee -a "$WFB_LOG_FILE"; }

# wfb_detect_mega <air|gs> — set $WFB_MEGA to the multi-call binary name
# (wfb-air / wfb-gs) when it is on PATH, else empty. The caller builds its own
# per-side launch tokens from $WFB_MEGA so the rest of the script is identical
# whether it runs the mega binary or the standalone tools.
wfb_detect_mega() {
    if command -v "wfb-$1" >/dev/null 2>&1; then
        WFB_MEGA="wfb-$1"
    else
        WFB_MEGA=""
    fi
}

# wfb_seed_key <drone|gs> <keyfile> — first-boot bring-up key. If <keyfile> is
# absent and the mega binary is present, derive a deterministic shared key from
# the built-in "Waybeam" passphrase via `keygen-ensure` (bit-identical to
# `wfb_keygen Waybeam`). This is an INSECURE shared default — replace it for
# production. No-op when the key already exists (never overwrites). In standalone
# (non-mega) mode the key must be provisioned out of band, as before.
# wfb_seed_key <drone|gs> <keyfile> [seed]
wfb_seed_key() {
    _role="$1"; _kf="$2"; _seed="$3"
    [ -f "$_kf" ] && return 0
    if [ -n "$WFB_MEGA" ]; then
        log "key: $_kf missing — seeding shared bring-up key (role=$_role, INSECURE default)"
        if [ -n "$_seed" ]; then
            "$WFB_MEGA" keygen-ensure --role "$_role" --seed "$_seed" "$_kf"
        else
            "$WFB_MEGA" keygen-ensure --role "$_role" "$_kf"
        fi
    else
        log "key: $_kf missing and no mega binary — provide $_kf (wfb tools will abort otherwise)"
    fi
}

# wfb_ensure_venc_shm [ring] — make the video encoder publish into the SHM ring
# the local wfb_tx consumes from (air side only; the GS has no encoder).
#
# Only writes + signals the encoder when the value is actually wrong, so a
# correctly-configured encoder is never needlessly reinited — important on the
# SigmaStar, where a spurious pipeline reinit can wedge the SoC. No-op (logged)
# when json_cli or the config file is absent.
#
# Device-verified on .13 (ssc338q/Star6E, 2026-06-15): SIGHUP applies the new
# `.outgoing.server` via a clean venc fork+exec respawn (the SIGHUP-respawn
# handoff) whose fresh process re-reads /etc/waybeam.json — pid changes, video
# resumes, no SoC wedge. So SIGHUP is correct here, but it is a RESPAWN, which
# is exactly why this stays only-if-changed: we never respawn a correctly
# configured (and fragile) encoder for nothing.
wfb_ensure_venc_shm() {
    _ring="${1:-local_shm}"
    _want="shm://$_ring"
    _cfg="/etc/waybeam.json"
    if ! command -v json_cli >/dev/null 2>&1; then
        log "venc shm: json_cli not found — leaving encoder config untouched"
        return 0
    fi
    if [ ! -f "$_cfg" ]; then
        log "venc shm: $_cfg absent — leaving encoder config untouched"
        return 0
    fi
    _cur=$(json_cli --raw -g .outgoing.server -i "$_cfg" 2>/dev/null)
    if [ "$_cur" = "$_want" ]; then
        log "venc shm: encoder already publishing to $_want"
        return 0
    fi
    log "venc shm: rewiring encoder outgoing.server '$_cur' -> '$_want'"
    if ! json_cli -s .outgoing.server "\"$_want\"" -i "$_cfg" 2>/dev/null; then
        log "venc shm: json_cli set failed — encoder left as-is"
        return 0
    fi
    _vpid=$(pidof waybeam 2>/dev/null)
    if [ -n "$_vpid" ]; then
        log "venc shm: SIGHUP waybeam ($_vpid) to apply"
        kill -HUP $_vpid 2>/dev/null
    else
        log "venc shm: waybeam not running — new value applies on its next start"
    fi
}
