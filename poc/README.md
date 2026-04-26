# waybeam_wfb_ng POC tools

End-to-end proof-of-concept stack for adaptive FEC on a wfb-ng video
link. Everything in this directory is **on-device** code (SigmaStar
Infinity6E / OpenIPC Linux, armv7l) plus a few host-native helpers.

The hot loop (vehicle), with the optional MCS controller wired in:

```
 waybeam_venc                wfb_tx (patched)            radio
 ┌──────────┐               ┌──────────────┐           ┌──────┐
 │ encoder  │── /dev/shm ──►│ FEC encode + │── inject ►│ wlan │──► (downlink)
 │ + sidecar│  (zero copy)  │ WiFi inject  │           └──────┘
 └────┬─────┘               └──┬─────┬─────┘                ▲
      │ frame metadata         │     │ -Y JSON tx_stats     │
      ▼ (UDP loopback)         │     ▼ (UDP loopback)       │
 ┌────────────────────────────────────┐                     │
 │ fec_controller                     │── HTTP API on :8765 │
 │   ── EWMA + headroom → k/n         │      /params /set   │
 │   ── 3-layer anti-bounce           │      /status /events│
 │   ── safe-bitrate assertion ───────┼─► venc HTTP         │
 └─────────────────┬──────────────────┘                     │
                   │ set_fec via wfb_tx CMD port (8000)     │
                   ▼                                        │
              wfb_tx control                                │
                   ▲                                        │
                   │ set_radio (mcs_index only)             │
 ┌─────────────────┴──────────────────┐                     │
 │ mcs_selector (optional)            │── HTTP API on :8766 │
 │   ── EWMA RSSI + loss penalty      │      /params /set   │
 │   ── 3-bucket FSM + deadband       │      /status /events│
 │   ── failsafe + recovery streak    │                     │
 └─────────────────▲──────────────────┘                     │
                   │ rx_ant JSON via UDP (e.g. :6600)       │
                   │                                        │
                   └────── ground host wfb_rx -Y ───────────┘
```

The two controllers are **independent and complementary**: fec_controller
sizes FEC k/n for whatever MCS the radio happens to be at; mcs_selector
reacts to ground-side RSSI/loss to pick that MCS. They share only the
wfb_tx control port (8000) and use different commands (`set_fec` vs
`set_radio`), so they don't collide.

Wfb_tx and wfb_rx are **patched** (`shm-input.patch`) but otherwise
upstream-compatible — invocations without `-H`/`-Y` behave identically
to stock wfb-ng.

---

## Inventory

| Artifact | Where | Arch | What it does |
|---|---|---|---|
| `link_controller` | `build/link_controller` | ARM dyn (71 KB) | **Recommended.** Merged FEC + MCS controller with embedded WebUI. REST/SSE/HTML on :8765. Both subsystems in one binary, dual-loop poll(), shared wfb_tx control socket. See [Tool reference](#link_controller--merged-fec--mcs--webui). |
| `fec_controller` | `build/fec_controller` | ARM dyn (34 KB) | Legacy single-purpose FEC controller. Superseded by `link_controller`; kept for rollback / A/B comparison. REST/SSE API on :8765. |
| `mcs_selector` | `build/mcs_selector` | ARM dyn (34 KB) | Legacy single-purpose MCS controller. Superseded by `link_controller`; kept for rollback. REST/SSE API on :8766. |
| `wfb_tx` | `build/wfb_tx` | ARM static (543 KB) | wfb-ng tx with `-H` (SHM input), `-Y host:port` (UDP stats push), `-x` aux flag. |
| `wfb_tx_cmd` | `build/wfb_tx_cmd` | ARM static (318 KB) | Runtime control: `set_fec`, `set_radio`, `set_mbit`, `get_radio`. |
| `wfb_keygen` | `build/wfb_keygen` | ARM static (423 KB) | Key generator (drone/gs keys). |
| `wfb_rx_native` | `build/wfb_rx_native` | x86_64 dyn (67 KB) | wfb-ng rx with `-Y host:port` (per-antenna JSON stats push). For ground host. |
| `shm_ring_stats` | `build/shm_ring_stats` | ARM dyn (10 KB) | Snapshot of the SHM ring state. |
| `shm_consumer_test` | `build/shm_consumer_test` | ARM dyn (10 KB) | Throughput tester for the SHM ring. |
| `rtp_timing_probe.c` | source only | host build | Host-native RTP + sidecar timing diagnostic. |
| `fec_controller.py` | source only | host/embedded | Older Python fec_controller. Superseded by the C version; kept for reference. |
| `ground_rssi_forwarder.py` | source only | host | **DEPRECATED.** Bridged wfb_rx stdout → UDP. Use `wfb_rx -Y` instead. |

Deeper docs:
- `SHM_HOWTO.md` — full SHM-ring protocol, wfb_tx `-H/-b/-r/-x/-Y` flag reference, on-device test recipes.
- `FEC_CTRL_POC.md` — fec_controller design (anti-bounce, dwells, deadband, settle windows).
- `SIDECAR_INFO.md` — RTP sidecar wire protocol (consumed by both `fec_controller` and `rtp_timing_probe`).

---

## Build

### Prerequisites

- `../toolchain/toolchain.sigmastar-infinity6e/` — OpenIPC star6e cross-toolchain
- Host packages (for `wfb_rx_native` + `rtp_timing_probe`): `g++`, `gcc`, `libsodium-dev`, `libpcap-dev`

### One-shot — build everything wfb-ng-related

```bash
cd poc
./build_wfb_tx.sh             # wfb_tx, wfb_tx_cmd, wfb_keygen, shm_ring_stats,
                              # shm_consumer_test, wfb_rx_native (all of them)
./build_wfb_tx.sh --clean     # wipe build/
./build_wfb_tx.sh --deploy    # also scp to DEPLOY_HOST=192.168.1.10
```

### fec_controller (separate Makefile)

```bash
cd poc
make -f Makefile.fec_controller          # cross → build/fec_controller (ARM)
make -f Makefile.fec_controller host     # native → build/fec_controller.host
make -f Makefile.fec_controller deploy   # scp build/fec_controller → 192.168.1.13:/tmp
make -f Makefile.fec_controller clean
```

Set `DEPLOY_HOST=<ip>` to override the default vehicle IP.

### mcs_selector (separate Makefile)

Same pattern as fec_controller:

```bash
cd poc
make -f Makefile.mcs_selector            # cross → build/mcs_selector (ARM)
make -f Makefile.mcs_selector host       # native → build/mcs_selector.host
make -f Makefile.mcs_selector deploy     # scp build/mcs_selector → 192.168.1.13:/tmp
make -f Makefile.mcs_selector clean
```

### link_controller (merged binary, recommended)

Combines `fec_controller` + `mcs_selector` into one binary plus an
embedded HTML WebUI. The Makefile auto-runs `xxd -i webui/index.html`
into `webui_assets.h` on every build.

```bash
cd poc
make -f Makefile.link_controller         # cross → build/link_controller (ARM)
make -f Makefile.link_controller webui   # only regen embedded WebUI assets
make -f Makefile.link_controller host    # native → build/link_controller.host
make -f Makefile.link_controller deploy  # scp build/link_controller → 192.168.1.13:/tmp
make -f Makefile.link_controller clean
```

`xxd` (from `vim-common`) is required on the host. The embedded WebUI
adds ~12 KB to the stripped binary (~71 KB total ARM).

### Re-applying the patch after upstream wfb-ng changes

`shm-input.patch` is the source of truth for everything modified in
wfb-ng. To regenerate it after editing `build/wfb-ng/src/*.cpp`:

```bash
cd poc/build/wfb-ng
git diff > ../../shm-input.patch
```

Then `./build_wfb_tx.sh --clean && ./build_wfb_tx.sh` re-applies cleanly.

---

## Quick start — full vehicle bring-up

This is the canonical setup on a fresh device (assuming venc is already
running and configured for sidecar on port 5602):

```bash
# 1. Cross-build (on host)
cd poc
./build_wfb_tx.sh
make -f Makefile.fec_controller

# 2. Deploy to vehicle (192.168.1.13)
scp -O build/wfb_tx build/wfb_tx_cmd build/wfb_keygen \
    build/shm_ring_stats build/shm_consumer_test \
    root@192.168.1.13:/usr/bin/
scp -O build/fec_controller root@192.168.1.13:/usr/bin/

# 3. Install init script (modeled on the vehicle's existing S95venc):
ssh root@192.168.1.13 'cat > /etc/init.d/S96fec_controller' << 'EOF'
#!/bin/sh
DAEMON='fec_controller'
DAEMON_PATH='/usr/bin/fec_controller'
DAEMON_ARGS='--sidecar 127.0.0.1:5602 --wfb-stats-port 5500 --safety 0.5'
LOGFILE='/tmp/fec_controller.log'
case "$1" in
    start)   pidof "$DAEMON" >/dev/null && exit 0
             "$DAEMON_PATH" $DAEMON_ARGS > "$LOGFILE" 2>&1 & ;;
    stop)    killall "$DAEMON" 2>/dev/null; sleep 1 ;;
    restart) $0 stop; $0 start ;;
    status)  pidof "$DAEMON" >/dev/null && echo running || echo stopped ;;
    *)       echo "Usage: $0 {start|stop|restart|status}"; exit 1 ;;
esac
EOF
ssh root@192.168.1.13 'chmod 755 /etc/init.d/S96fec_controller'

# 4. Start the link (assuming a wfb_tx wrapper script already exists,
#    or run inline — note the -Y flag enables the stats stream that
#    fec_controller subscribes to)
ssh root@192.168.1.13 'wfb_tx -K /etc/drone.key -M 2 -B 20 -k 6 -n 10 \
    -P 1 -Q -S 1 -L 1 -C 8000 -H local_shm -R 2097152 -s 2097152 \
    -l 500 -Y 127.0.0.1:5500 -i 207 -p 0 -x wlan0 &'

# 5. Start fec_controller via the init script
ssh root@192.168.1.13 '/etc/init.d/S96fec_controller start'

# 6. Verify
ssh root@192.168.1.13 'curl -s http://127.0.0.1:8765/status'
```

### Optional — also enable adaptive MCS (`mcs_selector`)

```bash
# 1. Cross-build and deploy
make -f Makefile.mcs_selector
scp -O build/mcs_selector root@192.168.1.13:/usr/bin/

# 2. Run alongside fec_controller — the ground host needs to forward
#    its wfb_rx -Y stream to the vehicle (here, port 6600):
#    Ground:  sudo wfb_rx_native -K /etc/drone.key -i 207 -p 0 -l 200 -x \
#                                 -Y 192.168.1.13:6600 wlxXXXXXX wlxYYYYYY
ssh root@192.168.1.13 'mcs_selector --stats 127.0.0.1:6600 \
                                    --wfb 127.0.0.1:8000 \
                                    --api-port 8766 -v &'

# 3. Verify both controllers
ssh root@192.168.1.13 'curl -s http://127.0.0.1:8765/status   # fec_controller
                       curl -s http://127.0.0.1:8766/status'  # mcs_selector
```

---

## Tool reference

### `link_controller` — merged FEC + MCS + WebUI

**One-liner**: combines `fec_controller` and `mcs_selector` into one
binary plus an embedded HTML WebUI. Both control loops live in a single
poll() loop, share the wfb_tx control socket, and feed a unified
REST/SSE/HTML endpoint on port 8765. When the MCS subsystem commits a
SET_RADIO it explicitly arms the FEC subsystem's `mcs_settle_s` window
in-process — replacing the older `tx_stats`-detect-then-arm round-trip
that the standalone pair used.

```bash
# Default — both subsystems on, polling-mode FEC, 8765 REST + WebUI
link_controller --stats 127.0.0.1:6600

# Production invocation on the vehicle
link_controller --stats 127.0.0.1:6600 \
                --wfb-stats-port 5601 \
                --sidecar 127.0.0.1:5602 \
                --api-port 8765 -v

# MCS-only (skip FEC)
link_controller --no-fec --stats 127.0.0.1:6600

# FEC-only (skip MCS) — equivalent to legacy fec_controller
link_controller --no-mcs --sidecar 127.0.0.1:5602

# Compute but don't send any wfb_tx commands
link_controller --dry-run -v
```

**Key flags**:

| Flag | Default | Purpose |
|---|---|---|
| `--wfb HOST:PORT` | `127.0.0.1:8000` | shared wfb_tx control socket |
| `--api-port N` | `8765` | REST + WebUI bind (0 disables) |
| `--no-fec` / `--no-mcs` | both on | disable a subsystem |
| `--sidecar HOST:PORT` | `127.0.0.1:6666` | venc sidecar (FEC) |
| `--wfb-stats-port N` | `0` (poll mode) | UDP listener for `wfb_tx -Y` (FEC) |
| `--stats HOST:PORT` | `127.0.0.1:5801` | UDP listener for `wfb_rx -Y` rx_ant (MCS) |
| `--range low\|med\|high` | `med` (1,2,3) | bucket→mcs preset |
| `--dry-run` | off | suppress all wfb_tx writes |

See `link_controller --help` for the full list (53 hot tunables; the
WebUI Tune tab generates a labeled input for every one of them).

**HTTP API**:

```bash
# Browser → embedded WebUI (Live / Tune / Timeline tabs)
open http://192.168.1.13:8765/

# curl (no HTML accept) → plain-text help
curl -s http://192.168.1.13:8765/

# Same help, always plain text
curl -s http://192.168.1.13:8765/help

# All 53 tunables (dotted keys: fec.* / mcs.* / common.*)
curl -s http://192.168.1.13:8765/params

# Tunable schema (type/lo/hi/help) — for forms / validators
curl -s http://192.168.1.13:8765/schema

# Combined status snapshot
curl -s http://192.168.1.13:8765/status

# Per-subsystem subset
curl -s http://192.168.1.13:8765/fec/status
curl -s http://192.168.1.13:8765/mcs/status

# Live tuning (multi-namespace atomic write)
curl -s "http://192.168.1.13:8765/set?fec.mtu=1400&mcs.rssi_thresh_low=-65"

# SSE — every log line as JSON, with subsys field
curl -sN http://192.168.1.13:8765/events
# data: {"t_s":42.123,"subsys":"mcs","msg":"decision: down bucket=1 ..."}

# Health
curl -s http://192.168.1.13:8765/health
```

**WebUI**:

- **Live** — current FEC k/n, MCS bucket, effective RSSI, loss/recov %,
  bitrate, plus inline SVG sparklines for last 60 antenna RSSI samples
  per antenna.
- **Tune** — auto-generated form from `/schema`, grouped by subsystem.
  Per-row Apply button + flash animation on accept/reject. Init-once
  pattern so the form doesn't reset on the 1 Hz status refresh.
- **Timeline** — scrolling SSE log, color-coded by subsystem
  (`fec`/`mcs`/`common`), filter chips per subsystem, XSS-safe
  (`textContent` only).

No external assets — all CSS+JS inline in `webui/index.html`, embedded
into the binary at build time via `xxd -i`.

**Coordination**:

- MCS commit → in-process `controller_arm_settle()` on FEC. No race,
  no tx_stats round-trip required.
- Both subsystems share `RadioState`; MCS writes `from_self=true` so
  the FEC "external change" log doesn't fire on self-induced changes.
- Single `wfb_get_radio()` and shared `g_req_id` (single-threaded so
  no atomicity needed).
- The legacy `mcs_settle_s` config name is kept verbatim — it's a FEC
  subsystem tunable that controls how long FEC ignores its own outputs
  after an MCS-driven radio change.

**Limits**: 8 concurrent HTTP clients, 4 of which can be SSE
subscribers (`503 sse: too many subscribers` past that). 16 KB JSON
response cap (overflow returns `500 json overflow`).

**Migration from `fec_controller` + `mcs_selector`**:

```bash
# Before:
fec_controller --sidecar 127.0.0.1:5602 --wfb-stats-port 5601
mcs_selector --stats 127.0.0.1:6600 --api-port 8766

# After (same behavior, single process):
link_controller --sidecar 127.0.0.1:5602 \
                --wfb-stats-port 5601 \
                --stats 127.0.0.1:6600 \
                --api-port 8765
```

Tunable names map mechanically: every `fec_controller` tunable becomes
`fec.<name>` and every `mcs_selector` tunable becomes `mcs.<name>`.
The two old binaries remain in tree for rollback / A/B comparison —
they coexist on the vehicle (different REST ports) but only one of
them should be writing to `wfb_tx` at a time per CMD ID.

---

### `fec_controller` — adaptive FEC + bitrate controller

**One-liner**: subscribes to `wfb_tx -Y` JSON stats + venc sidecar
per-frame metadata, sizes FEC k/n by EWMA + bounded headroom + 3 layers
of anti-bounce, asserts safe video bitrate via venc HTTP, and exposes a
loopback REST + SSE API for tuning + log streaming.

```bash
# Default invocation (defaults: --sidecar 127.0.0.1:6666, --api-port 8765)
fec_controller

# Production invocation on this device
fec_controller --sidecar 127.0.0.1:5602 --wfb-stats-port 5500 --safety 0.5

# Disable HTTP API
fec_controller --api-port 0

# Compute but don't write set_fec / bitrate (observe-only)
fec_controller --dry-run -v
```

**Key flags** (see `fec_controller --help` for the full list):

| Flag | Default | Purpose |
|---|---|---|
| `--sidecar HOST:PORT` | `127.0.0.1:6666` | venc sidecar source |
| `--wfb HOST:PORT` | `127.0.0.1:8000` | wfb_tx control port (`-C`) |
| `--venc HOST:PORT` | `127.0.0.1:80` | venc HTTP API |
| `--wfb-stats-port N` | `0` (disabled) | bind UDP listener for `wfb_tx -Y` stream; 0 falls back to `CMD_GET_RADIO` polling |
| `--api-port N` | `8765` | HTTP API bind (0 disables) |
| `--safety F` | `0.5` | fraction of PHY rate usable: `target = phy * k/n * safety` |
| `--ppf-deadband F` | `0.15` | ±F·MTU slack at ppf bucket edges |
| `--k-down-dwell F` | `8.0` | k-down anti-bounce |
| `--k-up-dwell F` | `2.0` | k-up anti-spike (0 = legacy fast-up) |
| `--dry-run` | off | compute but don't write set_fec / bitrate |

**HTTP API** (loopback only):

```bash
# Help
curl -s http://127.0.0.1:8765/

# Snapshot of current config (26 hot-tunable fields)
curl -s http://127.0.0.1:8765/params

# Live state (controller, radio, last writes)
curl -s http://127.0.0.1:8765/status

# Live tuning
curl -s "http://127.0.0.1:8765/set?safety_margin=0.6&k_up_dwell_s=4.0"

# Bool fields take 0/1 or true/false
curl -s "http://127.0.0.1:8765/set?dry_run=true"

# Stream every log line as JSON SSE events
curl -sN http://127.0.0.1:8765/events | grep --line-buffered '^data:'

# Just the message field, line by line
curl -sN http://127.0.0.1:8765/events \
  | sed -n 's/^data: //p' | jq -r .msg

# Health check
curl -s http://127.0.0.1:8765/health
```

**SSE response shape** — `data: {"t_s":12.345,"msg":"FEC update: k=20 n=29 (...)"}\n\n`. A `:ping\n\n` heartbeat fires every 15 s.

**Limits**: 8 concurrent HTTP clients, 4 of which can be SSE subscribers; 5th `/events` returns `503 sse: too many subscribers`.

See `FEC_CTRL_POC.md` for the design rationale (asymmetric gating, dwell semantics, parity boost, etc.).

---

### `mcs_selector` — adaptive MCS controller

**One-liner**: subscribes to `wfb_rx -Y` rx_ant JSON datagrams (forwarded
from the ground), computes effective RSSI (smoothed antenna RSSI minus
loss penalty), walks an asymmetric 3-bucket FSM (fast-down / slow-up
with deadband, dwell hysteresis, per-direction cooldown, and oscillation
backoff), and writes `CMD_SET_RADIO` to wfb_tx — varying `mcs_index` in
isolation (every other radiotap field is preserved from a startup
`CMD_GET_RADIO`). Runs alongside `fec_controller` without coordination.

```bash
# Default invocation (defaults: --stats 127.0.0.1:5801, --wfb 127.0.0.1:8000,
#                               --api-port 8766)
mcs_selector

# Production invocation on this device — listen on the port the ground
# wfb_rx is pushing -Y to, write to local wfb_tx control port:
mcs_selector --stats 127.0.0.1:6600 --wfb 127.0.0.1:8000

# Pick a starting range preset (sets the three mcs_bucket_* tunables)
mcs_selector --range high      # buckets 0/1/2 → MCS 2/3/4 (bench, strong RSSI)
mcs_selector --range med       # buckets 0/1/2 → MCS 1/2/3 (default)
mcs_selector --range low       # buckets 0/1/2 → MCS 0/1/2 (long-range)

# Compute decisions but never send CMD_SET_RADIO
mcs_selector --dry-run -v

# Force range[0] mcs at startup (off by default — observe-then-act)
mcs_selector --start-low

# Disable HTTP API
mcs_selector --api-port 0
```

**Key flags** (see `mcs_selector --help` for the full list):

| Flag | Default | Purpose |
|---|---|---|
| `--stats HOST:PORT` | `127.0.0.1:5801` | UDP listener for `wfb_rx -Y` JSON |
| `--wfb HOST:PORT` | `127.0.0.1:8000` | wfb_tx control port (`-C`) |
| `--api-port N` | `8766` | HTTP API bind (0 disables) |
| `--range low\|med\|high` | `med` | preset bucket→mcs mapping |
| `--rssi-low F` | `-70` | lower RSSI threshold (dBm); below = bucket 0 |
| `--rssi-high F` | `-50` | upper RSSI threshold (dBm); above = bucket 2 |
| `--deadband F` | `2.0` | symmetric crossing deadband (dB) |
| `--up-consec N` | `3` | consecutive samples needed to commit UP |
| `--down-consec N` | `1` | consecutive samples needed to commit DOWN |
| `--up-cooldown F` | `3.0` | min seconds between UP commits |
| `--down-cooldown F` | `0.2` | min seconds between DOWN commits |
| `--failsafe F` | `0.5` | watchdog gap (s) before forcing bucket 0 |
| `--loss-lost-pct F` | `0.5` | dB penalty per % of post-FEC `lost` |
| `--loss-recov-pct F` | `0.0` | dB penalty per % of `fec_recovered` (0 = ignore; FEC working ≠ bad link) |
| `--start-low` | off | force range[0] mcs at startup |
| `--dry-run` | off | compute but don't send CMD_SET_RADIO |

**HTTP API** (loopback only, mirrors fec_controller's API on a different port):

```bash
# Help
curl -s http://127.0.0.1:8766/

# Snapshot of current config (~28 hot-tunable fields)
curl -s http://127.0.0.1:8766/params

# Live state — selector + radio + last score
curl -s http://127.0.0.1:8766/status

# Live tuning — switch range preset by editing the three buckets together
curl -s "http://127.0.0.1:8766/set?mcs_bucket_0=2&mcs_bucket_1=3&mcs_bucket_2=4"

# Or change a single threshold
curl -s "http://127.0.0.1:8766/set?rssi_thresh_high=-40"

# Stream every log line as JSON SSE
curl -sN http://127.0.0.1:8766/events | sed -n 's/^data: //p' | jq -r .msg

# Health check
curl -s http://127.0.0.1:8766/health
```

**Operator log lines you'll see**:

```
[mcs t=  0.089] decision: init bucket=2 mcs=3 eff=-27.0 smooth=-27.0 raw=-27.0 lost=0.00% recov=10.0% pen=0.0dB
[mcs t=  5.009] hb: bucket=2 mcs=3 eff=-26.9 lost=0.00% recov=9.4% pen=0.0dB commits=1
[mcs t= 13.289] realign: bucket=2 want_mcs=2 wfb_mcs=3
[mcs t= 56.079] decision: down bucket=1 mcs=2 eff=-55.0 smooth=-55.0 raw=-90.0 lost=0.00% recov=9.5% pen=0.0dB
[mcs t= 60.903] failsafe: no rx_ant for 0.55s (>0.50s) — forcing bucket 0
[mcs t= 89.939] failsafe: recovered after 3 good samples
[mcs t=  5.005] hb: waiting for rx_ant on udp:5801 (no datagram yet, 5.0s since boot)
```

`hb:` is heartbeat (every 5 s). `realign:` fires when the bucket→mcs
mapping changes (operator edited a `mcs_bucket_X` tunable, or wfb_tx
got an external SET) — not a bucket transition, just a refresh. The
divergence indicator ` WFB_MCS=N!` is appended to heartbeat when
`sel.current_mcs != radio.mcs` (e.g., during a SET-failure window).

**Coexistence with `fec_controller`**:

- mcs_selector listens on its own UDP port (default 5801; production
  example uses 6600) — fec_controller listens on a different port
  (default disabled, production uses 5500). They don't share the input
  stream.
- Both write to wfb_tx control port (8000) but use different command
  IDs: `set_radio` (mcs_selector) vs `set_fec` (fec_controller). No
  collision on the wire.
- When mcs_selector commits a new MCS, fec_controller sees the change
  on its next `tx_stats` datagram and arms its `mcs_settle_s` window
  (default 5 s) to absorb the EWMA transient. Verified live: the two
  cooperate without thrashing.

**Topology** — typical wiring for the `--stats 127.0.0.1:6600` example:

```
GROUND:   wfb_rx -Y 192.168.1.13:6600 (pushes rx_ant JSON to vehicle)
              │
              ▼ (UDP over WiFi/ethernet)
VEHICLE:  mcs_selector --stats 127.0.0.1:6600 (binds INADDR_ANY, host
                                               argument is just a label)
```

The host part of `--stats` is informational; the listener actually binds
`INADDR_ANY:port` so any source IP can deliver datagrams.

**Failsafe semantics** (verified live):

1. If no rx_ant datagram arrives for `failsafe_timeout_s` (default 0.5 s),
   the FSM commits to bucket 0 (lowest MCS in the active range) and
   freezes.
2. The trip log fires exactly once; subsequent watchdog ticks early-exit
   silently.
3. If the failsafe `set_radio` itself fails (wfb_tx unreachable),
   `in_failsafe` STAYS true and the realign block on the next
   datagram retries the SET.
4. Recovery requires `failsafe_recovery_consecutive` (default 3)
   consecutive samples with `eff_rssi >= rssi_thresh_low + deadband`.

---

### `wfb_tx` — patched wfb-ng tx (`-H`, `-Y`)

Stock wfb-ng tx plus three additions:

- **`-H {local_shm|udp}`** — input source. `local_shm` reads from
  `/dev/shm/venc_wfb` (zero-copy from venc); `udp` is the stock path.
- **`-b N`** — burst threshold for SHM mode.
- **`-r F`** — parity ratio override (overrides `-k -n` math).
- **`-x`** — disable session announce on FEC change (saves overhead).
- **`-Y host:port`** — push one JSON `tx_stats` datagram per `-l`
  interval to a UDP target. Newline-terminated. **`fec_controller`
  consumes this when run with `--wfb-stats-port`.**

Production invocation (this is what the vehicle runs at boot):

```bash
wfb_tx -K /etc/drone.key \
       -M 2 -B 20 -k 6 -n 10 -P 1 -Q -S 1 -L 1 \
       -C 8000 \
       -H local_shm -R 2097152 -s 2097152 \
       -l 500 \
       -Y 127.0.0.1:5500 \
       -i 207 -p 0 -x wlan0
```

Schema (`-Y` JSON, all integer fields, one datagram per `-l` interval ms):

```json
{
  "ts_ms": 1234567890,
  "type": "tx_stats",
  "ver": 1,
  "seq": 42,                        // monotonic; gap = drop, restart = backwards
  "interval_ms": 500,
  "tx": {
    "pkts_in": 1234, "bytes_in": 567890,
    "pkts_out": 1300, "bytes_out": 600000,
    "pkts_drop": 0, "pkts_trunc": 0, "fec_timeouts": 0,
    "fec_k": 6, "fec_n": 10
  },
  "radio": {
    "mcs": 2, "bw": 20, "short_gi": 0,
    "stbc": 1, "ldpc": 1, "vht_mode": 0, "vht_nss": 1
  }
}
```

**Inspect the stream live** (busybox `nc` doesn't do `-u`; use socat):

```bash
socat -u UDP-RECV:5500,reuseaddr -
```

---

### `wfb_tx_cmd` — runtime control client

Talks to the wfb_tx control port (default 8000). All commands take
`-flag value` style (positional args are silently ignored).

```bash
# Read radio params
wfb_tx_cmd 8000 get_radio

# Change FEC sizing live
wfb_tx_cmd 8000 set_fec -k 8 -n 12

# Change MCS index (note: -M, NOT positional)
wfb_tx_cmd 8000 set_radio -M 5

# Change bitrate hint
wfb_tx_cmd 8000 set_mbit -B 20
```

**Gotcha:** `wfb_tx_cmd 8000 set_radio mcs_index 7` does NOT work —
positional args are ignored by getopt. Always use `-M N`.

`fec_controller` issues `set_fec` automatically when run normally; if
you also call `wfb_tx_cmd set_fec` externally, the controller sees the
change in the next stats datagram and logs "external change observed."

---

### `wfb_keygen` — key generator

Generates a `drone.key` + `gs.key` pair for wfb-ng:

```bash
wfb_keygen
# produces drone.key + gs.key in cwd
```

Copy `drone.key` to the vehicle (`/etc/drone.key`), `gs.key` to the
ground station.

---

### `wfb_rx_native` — patched wfb_rx with `-Y` (ground side)

Stock wfb_rx plus `-Y host:port`, identical pattern to `wfb_tx -Y`.
Built x86_64-native (the typical use case is a ground laptop). The
`fec_controller` does not consume this directly — it's primarily for
human inspection / external dashboards / replacing the deprecated
`ground_rssi_forwarder.py`.

```bash
# Ground host — receive video from drone, push per-antenna JSON to localhost
sudo ./build/wfb_rx_native \
     -K /etc/drone.key -i 207 -p 0 -l 1000 \
     -Y 127.0.0.1:5801 \
     wlx40a5ef2f229b
```

Schema (`-Y` JSON, one datagram per `-l` interval ms):

```json
{
  "ts_ms": 1234567890,
  "type": "rx_ant",
  "ver": 1,
  "seq": 42,
  "interval_ms": 1000,
  "ant": [
    {
      "freq": 5180, "mcs": 2, "bw": 20, "id": 0,
      "pkts": 2310,
      "rssi": {"min": -78, "avg": -65, "max": -52},
      "snr":  {"min":   8, "avg":  21, "max":  34}
    }
  ],
  "pkt": {
    "all": 2310, "bytes": 3145728, "dec_err": 0,
    "session": 0, "data": 2300, "uniq": 2300,
    "fec_recovered": 48, "lost": 0, "bad": 0,
    "outgoing": 2300, "outgoing_bytes": 3145728
  }
}
```

Inspect live:

```bash
socat -u UDP-RECV:5801,reuseaddr - | jq -c .
```

---

### `shm_ring_stats` — SHM ring snapshot

Diagnostic for the venc → wfb_tx SHM ring. Run on the vehicle:

```bash
shm_ring_stats
```

Reports producer/consumer indices, slot capacity, and free count. See
`SHM_HOWTO.md` for ring layout details.

---

### `shm_consumer_test` — SHM throughput tester

Standalone consumer that drains the SHM ring without touching the radio
— useful for verifying venc → SHM is healthy in isolation.

```bash
shm_consumer_test
```

Logs frames-per-second + bytes/sec drained. Will starve the real wfb_tx
if both run simultaneously, so stop wfb_tx first.

---

### `rtp_timing_probe.c` — host-native sidecar diagnostic

Reference implementation of the venc sidecar consumer. Subscribes,
measures per-frame latency from sidecar `frame_ready_us` to RTP packet
arrival.

```bash
# Build (host-native)
cc -O2 -Wall -o rtp_timing_probe rtp_timing_probe.c

# Run pointing at vehicle
./rtp_timing_probe --venc 192.168.1.13:6666 --rtp-port 5600
```

See `SIDECAR_INFO.md` for the wire protocol.

---

### `fec_controller.py` — older Python prototype

The Python `fec_controller` that predates `fec_controller.c`. Same job
(adaptive FEC sizing) but uses asyncio instead of `poll()` and depends
on Python 3 + scipy. Kept as reference; the C version is what runs on
the vehicle.

```bash
python3 fec_controller.py --help
```

---

### `ground_rssi_forwarder.py` — DEPRECATED

Used to bridge `wfb_rx` stdout RX_ANT lines into UDP datagrams. Now
superseded by `wfb_rx -Y`. The script still works but its docstring
banner directs you to the replacement.

---

## On-device persistence (vehicle 192.168.1.13)

Currently installed:

| Path | md5 | Notes |
|---|---|---|
| `/usr/bin/wfb_tx` | `2fb2f18a…` | patched, ARM static, 543 KB |
| `/usr/bin/wfb_tx_cmd` | `0a90845a…` | |
| `/usr/bin/fec_controller` | `3dd500a5…` | post-PR #25 integration build, 34 KB |
| `/usr/bin/mcs_selector` | `7f1f1b80…` | post-PR #27 integration build, 34 KB |
| `/etc/init.d/S96fec_controller` | (script) | starts after S95venc on boot |
| `/usr/bin/wfb_tx.bak.20260316` | `…` | pristine pre-patch backup |
| `/usr/bin/fec_controller.bak.20260316-220122` | `f4d5e639…` | last pre-integration build |

To roll back fec_controller:

```bash
ssh root@192.168.1.13 '
  /etc/init.d/S96fec_controller stop
  cp /usr/bin/fec_controller.bak.20260316-220122 /usr/bin/fec_controller
  /etc/init.d/S96fec_controller start
'
```

---

## Troubleshooting

**`fec_controller` log shows "wfb-stats: no datagrams for N.Ns".**
The `wfb_tx -Y` target is wrong, or wfb_tx died. Check that wfb_tx is
running with the same port your `--wfb-stats-port` expects.

**`wfb_tx_cmd 8000 set_radio mcs_index 7` does nothing.**
Positional args are ignored. Use `-M 7`.

**`wfb_tx` ignores `kill -INT`.**
Use `kill -9` in test scripts. Bash `wait` after `kill -INT` will hang.

**Busybox `nc -u` doesn't work on the device.**
Use `socat -u UDP-RECV:PORT,reuseaddr -` instead.

**REST API 503 on `/events`.**
Cap is 4 concurrent SSE subscribers. Disconnect one, or up
`API_MAX_SSE` in `fec_controller.c` and rebuild.

**Bitrate write doesn't fire after `/set?safety_margin=...`.**
The `bitrate_assert` tick is 1 Hz, and re-writes are gated by
`bitrate_tolerance` (default 15 %). Smaller changes get absorbed.

**`shm-input.patch` fails to apply after pulling new wfb-ng.**
Edit `build/wfb-ng/src/{tx,rx}.cpp` to merge the conflict, then
`cd build/wfb-ng && git diff > ../../shm-input.patch` to regenerate.

**`mcs_selector` heartbeat says "waiting for rx_ant on udp:N (no datagram yet)".**
Ground host's `wfb_rx -Y` isn't pointing at this vehicle's port. Confirm
with `socat -u UDP-RECV:6600,reuseaddr -` on the vehicle to see if any
bytes arrive. If not, check the ground-side `-Y target_ip:port` matches
the vehicle's IP and `--stats` port.

**`mcs_selector` heartbeat shows ` WFB_MCS=N!`.**
Selector and wfb_tx are at different MCS values — usually because a
SET_RADIO failed. The realign block on the next datagram retries; if
divergence persists, check that `wfb_tx -C 8000` is reachable.

**`mcs_selector` `recov` is consistently 15-20% on a strong link.**
Normal. With wfb_tx `-k 6 -n 10` (40% parity), the receiver triggers
FEC the moment it has `k` unique fragments, and that kth fragment is
often a parity (sender interleaves data + parity). It does NOT mean
diversity is broken or the link is bad. The default penalty weight on
`fec_recovered` is 0 specifically for this reason — only `lost`
contributes to penalty by default.

**`mcs_selector` wfb_tx changes MCS unexpectedly on restart.**
By default mcs_selector picks an MCS based on current RSSI at boot,
which may differ from whatever wfb_tx was previously set to. If you
want predictable boot behavior, run with `--start-low` (force range[0]
mcs at startup) or pre-set wfb_tx to your desired MCS with a
matching `--range` preset.

---

## Network port reference

| Port | Owner | Direction | Purpose |
|---|---|---|---|
| 5500/udp | `wfb_tx -Y` → fec_controller | producer→consumer | tx_stats JSON (vehicle-local) |
| 5602/udp | venc sidecar → fec_controller | producer→consumer | per-frame metadata |
| 5800/udp | (free, reserved for `wfb_tx -Y` debug) | | |
| 5801/udp | `wfb_rx -Y` → mcs_selector / dashboards | producer→consumer | rx_ant JSON (default mcs_selector listener) |
| 6600/udp | ground `wfb_rx -Y` → vehicle mcs_selector | producer→consumer | rx_ant JSON (production example) |
| 6666/udp | venc sidecar → fec_controller (default) | producer→consumer | per-frame metadata (default port) |
| 8000/udp | `wfb_tx -C` ← wfb_tx_cmd / fec_controller / mcs_selector | request/response | control: set_fec (fec_controller), set_radio (mcs_selector), etc. |
| 8765/tcp | fec_controller HTTP API | server | REST + SSE |
| 8766/tcp | mcs_selector HTTP API | server | REST + SSE |
| 80/tcp | venc HTTP API | server | `/api/v1/set?video0.bitrate=N` |

`8000/udp`, `8765/tcp`, `8766/tcp`, `80/tcp` and the loopback `udp:5500/5602/5801/6600/6666` listeners are all 127.0.0.1 / INADDR_ANY on the
vehicle. The `6600/udp` example above is the cross-host case: a ground
station at any IP pushes rx_ant JSON over the WiFi/ethernet uplink to
the vehicle's `INADDR_ANY:6600`.
