# waybeam_wfb_ng POC tools

End-to-end proof-of-concept stack for adaptive FEC on a wfb-ng video
link. Everything in this directory is **on-device** code (SigmaStar
Infinity6E / OpenIPC Linux, armv7l) plus a few host-native helpers.

The hot loop:

```
 waybeam_venc                wfb_tx (patched)             radio
 ┌──────────┐               ┌──────────────┐           ┌──────┐
 │ encoder  │── /dev/shm ──►│ FEC encode + │── inject ►│ wlan │──► …
 │ + sidecar│  (zero copy)  │ WiFi inject  │           └──────┘
 └────┬─────┘               └──┬───────────┘
      │ frame metadata         │ -Y JSON tx_stats (1/interval)
      ▼ (UDP loopback)         ▼ (UDP loopback)
 ┌────────────────────────────────────┐
 │ fec_controller                     │── HTTP API on :8765
 │   ── EWMA + headroom → k/n         │      /params /set /status
 │   ── 3-layer anti-bounce           │      /events (SSE) /health
 │   ── safe-bitrate assertion ───────┼─► venc HTTP /api/v1/set?...
 └────────────────────────────────────┘
```

Wfb_tx and wfb_rx are **patched** (`shm-input.patch`) but otherwise
upstream-compatible — invocations without `-H`/`-Y` behave identically
to stock wfb-ng.

---

## Inventory

| Artifact | Where | Arch | What it does |
|---|---|---|---|
| `fec_controller` | `build/fec_controller` | ARM dyn (34 KB) | Adaptive FEC sizing + REST/SSE API. Single-binary on-device controller. |
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

---

## Tool reference

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

---

## Network port reference

| Port | Owner | Direction | Purpose |
|---|---|---|---|
| 5500/udp | `wfb_tx -Y` → fec_controller | producer→consumer | tx_stats JSON |
| 5602/udp | venc sidecar → fec_controller | producer→consumer | per-frame metadata |
| 5800/udp | (free, reserved for `wfb_tx -Y` debug) | | |
| 5801/udp | `wfb_rx -Y` → external (ground) | producer→consumer | rx_ant JSON |
| 6666/udp | venc sidecar → fec_controller (default) | producer→consumer | per-frame metadata (default port) |
| 8000/udp | `wfb_tx -C` ← wfb_tx_cmd / fec_controller | request/response | control: set_fec, set_radio, etc. |
| 8765/tcp | fec_controller HTTP API | server | REST + SSE |
| 80/tcp | venc HTTP API | server | `/api/v1/set?video0.bitrate=N` |

All ports are 127.0.0.1 (loopback) on the vehicle.
