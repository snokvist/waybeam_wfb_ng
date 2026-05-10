# SHM Ring: Setup Guide for SigmaStar SoC

Zero-copy RTP packet transfer from venc to wfb_tx via POSIX shared memory,
eliminating UDP sendmsg/recvmsg kernel overhead on the video path.

**Target**: SSC30KQ / SSC338Q (SigmaStar Infinity6E), armv7l, OpenIPC Linux.

## Architecture

```
 waybeam_venc                   wfb_tx (patched)
 ┌──────────┐                  ┌──────────────┐
 │ H.265    │   /dev/shm/      │ FEC encode + │
 │ encoder  │──► venc_wfb ────►│ WiFi inject  │──► wlan0
 │ + RTP    │   (shared mem)   │              │
 └──────────┘                  └──────────────┘
     512 slots × 1412 bytes
     Lock-free SPSC ring, futex wake
```

No UDP sockets on the video path. Audio continues over UDP to localhost.

---

## Step 1: Build the patched wfb_tx

On the build host (x86_64 Linux):

```bash
cd wfb
./build_wfb_tx.sh
```

This will:
1. Download and cross-compile libsodium (static)
2. Clone wfb-ng from GitHub
3. Apply the SHM input patch (`-H` flag)
4. Cross-compile SHM diagnostic tools (`shm_ring_stats`, `shm_consumer_test`)
5. Produce all binaries in `wfb/build/`

Requires: the star6e toolchain at `../../toolchain/toolchain.sigmastar-infinity6e/`,
git, curl, autotools. First run downloads ~5 MB (libsodium) + ~2 MB (wfb-ng).

## Step 2: Deploy binaries to the SoC

```bash
# One-command deploy (builds if needed, then scp to device):
cd wfb
./build_wfb_tx.sh --deploy

# Or with a custom host:
DEPLOY_HOST=10.0.0.1 ./build_wfb_tx.sh --deploy

# Manual deploy:
scp -O wfb/build/{wfb_tx,wfb_keygen,shm_ring_stats,shm_consumer_test} \
    root@192.168.1.10:/usr/bin/
```

Note: `scp -O` is required for the SigmaStar dropbear SSH server.

## Step 3: Generate wfb-ng keys (first time only)

On the SoC:

```bash
cd /etc
wfb_keygen
# Creates gs.key (ground station) and drone.key (vehicle/drone)
```

Copy `gs.key` to your ground station (wfb_rx side).
Keep `drone.key` on the vehicle.

## Step 4: Configure venc for SHM output

Edit `/etc/venc.json` on the device (venc always loads from this path,
there is no `-c` flag). Back up first: `cp /etc/venc.json /etc/venc.json.bak`

Change the `outgoing` section:

```json
{
  "outgoing": {
    "enabled": true,
    "server": "shm://venc_wfb",
    "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "audioPort": 0
  }
}
```

Key settings:

| Field | Value | Notes |
|-------|-------|-------|
| `server` | `"shm://venc_wfb"` | The `venc_wfb` part becomes the SHM ring name (`/dev/shm/venc_wfb`) |
| `streamMode` | `"rtp"` | Required — SHM carries RTP packets |
| `maxPayloadSize` | `1400` | Each ring slot holds this + 12 bytes (RTP header) |
| `audioPort` | `0` | Disables audio (no UDP socket to share). Set to `5601` for audio over UDP to localhost |

### Audio in SHM mode

- `audioPort: 0` — audio disabled entirely (recommended if not needed)
- `audioPort: 5601` — audio sent via UDP to `127.0.0.1:5601`. You must run a
  separate `wfb_tx` instance for the audio stream (standard UDP input mode)

## Step 5: Start venc

```bash
# Start the encoder — it creates /dev/shm/venc_wfb automatically
venc &
```

Verify the ring was created:

```bash
ls -la /dev/shm/venc_wfb
# -rw------- 1 root root 725184 ... venc_wfb

shm_ring_stats venc_wfb
# slots=512 data_size=1412 total=725184
# write_idx=512 read_idx=0 lag=512
# ring FULL
```

The ring fills up immediately (FULL is expected without a consumer).
Once wfb_tx starts consuming, the lag drops to near zero.

## Step 6: Start wfb_tx with SHM input

```bash
# Typical usage — adjust -k/-n (FEC), -p (radio port), -B/-M (radio params)
wfb_tx -H venc_wfb \
       -K /etc/drone.key \
       -k 8 -n 12 \
       -p 0 \
       -B 20 -M 3 \
       -r 0.5 \
       wlan0
```

| Flag | Purpose |
|------|---------|
| `-H venc_wfb` | Read from SHM ring instead of UDP (the patch adds this) |
| `-K /etc/drone.key` | Encryption key |
| `-k 8 -n 12` | FEC: 8 data + 4 recovery packets |
| `-p 0` | Radio port (channel ID) |
| `-B 20 -M 3` | Bandwidth 20 MHz, MCS index 3 |
| `-r 0.5` | Partial-block parity ratio (matches `(n-k)/k=4/8=0.5`); see "Runtime tuning" below |
| `wlan0` | WiFi interface in monitor mode |

wfb_tx attaches to the ring, reads RTP packets via `venc_ring_read()`, and
injects them over WiFi. The control channel (FEC/radio commands via UDP)
continues working normally. If the SHM ring isn't available yet wfb_tx will
log `SHM ring 'venc_wfb' not available, waiting for producer...` and retry
every 500 ms instead of aborting — so start order is no longer strict.

Additional flags available from this patch (see "Runtime tuning"
below):

| Flag | Default | Purpose |
|------|---------|---------|
| `-T MS` | `0` | FEC safety-net timeout in ms (`0` = disabled). Closes the open block with FEC_ONLY padding when no input arrives for that long. Frame boundaries are not detected — `-T` is a coarse timeout, not a per-frame close. |
| `-x` | off | Plaintext data fragments (video only — session still signed) |
| `-Y host:port` | off | Push tx_stats JSON to a UDP target every `-l` interval |

`-T` is also runtime-tunable: `wfb_tx_cmd <port> set_fec -T <ms>`
rewrites it without restarting wfb_tx, and the binary `CMD_SET_FEC`
wire format carries the same field so `fec_controller` can scale the
timeout with the current FPS.

## Step 7: Verify

### Check ring is being consumed

```bash
shm_ring_stats venc_wfb
# write_idx=5678 read_idx=5670 lag=8
# ring ok
```

A small `lag` (< 50) means wfb_tx is keeping up. `lag=512` (ring FULL) means
wfb_tx can't keep up or isn't running.

### Measure throughput

```bash
# Run for 5 seconds (stop wfb_tx first to avoid contention)
shm_consumer_test venc_wfb 5
#   3383 pkt/s  23.8 Mbit/s
#   2593 pkt/s  20.8 Mbit/s
# === SHM Ring Consumer Results ===
# Duration:   2.5 s
# Packets:    7198 (2910 pkt/s)
# Data:       6.5 MB (22.0 Mbit/s)
# Avg pkt:    945 bytes
# Ring w_idx: 7218  r_idx: 7198  (lag: 20)
```

(Example output at 20 Mbps CBR, 720p120)

Note: `shm_consumer_test` competes with wfb_tx for packets. Stop wfb_tx
first if you want accurate throughput numbers, or use it only for quick
checks.

### Monitor wfb_tx output

wfb_tx logs statistics every second (configurable via `-l`):

```
SHM ring attached: 'venc_wfb' (512 slots x 1412 bytes)
SHM input mode: reading from ring 'venc_wfb'
```

## Startup order

Start order is **no longer strict** — wfb_tx retries attach every 500 ms
until the ring exists, and automatically reattaches if venc restarts (see
"Crashed producer" in Troubleshooting). Any order works; the sleep is
optional.

Recommended init script:

```bash
#!/bin/sh

# 1. Set up WiFi monitor mode
ip link set wlan0 down
iw dev wlan0 set type monitor
ip link set wlan0 up
iw dev wlan0 set channel 149 HT20

# 2. Start venc (creates the SHM ring)
# Config must already have server: "shm://venc_wfb" in /etc/venc.json
venc &

# 3. Start wfb_tx — attaches immediately if venc is up, else retries
wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 -B 20 -M 3 -r 0.5 wlan0 &
```

## Switching back to UDP mode

To revert to standard UDP output (no SHM), restore the original config:

```bash
cp /etc/venc.json.bak /etc/venc.json
# Or edit /etc/venc.json and change outgoing.server back to:
#   "server": "udp://192.168.2.20:5600"
```

Then restart venc and run wfb_tx with `-u` instead of `-H`:

```bash
killall venc; sleep 1; venc &
wfb_tx -u 5600 -K /etc/drone.key -k 8 -n 12 -p 0 wlan0
```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `SHM ring '...' not available, waiting for producer` | venc not running or wrong name | Start venc; wfb_tx will attach automatically once the ring appears |
| `ring FULL` (lag=512) | wfb_tx not consuming fast enough | Check wfb_tx is running; reduce bitrate or FEC overhead |
| High lag but not full | Momentary burst; should recover | Normal during keyframes; monitor over time |
| `shm_ring_stats` shows `write_idx=0` | venc not encoding | Check venc logs; verify sensor/ISP init |
| `SHM producer stalled` | venc crashed without cleanup (watchdog kicked in at 3 s frozen write_idx) | wfb_tx auto-detaches; restart venc, wfb_tx will reattach |
| `SHM header drift: ... Producer restarted; detaching` | venc was rebuilt onto the same `/dev/shm` inode (older venc that uses `O_TRUNC`, or manual `rm /dev/shm/<name>` + restart) — wfb_tx caught the new epoch before any corrupt read | Auto-recovers via re-attach to the new ring. With current venc (uses `shm_unlink` + `O_EXCL` since v0.9.2), restarts produce a fresh inode and the watchdog branch evicts instead — this branch stays silent |
| Audio not working | `audioPort=0` disables audio in SHM mode | Set `audioPort: 5601` and run separate `wfb_tx -u 5601` for audio |
| Video comes up but is black on ground | AEAD-mode mismatch (only one side has `-x`) | Check RX logs for `packets dropped (AEAD-mode mismatch, check -x flag on both sides)` — set or clear `-x` to match |
| `Invalid fragment_idx: N` at startup | Pre-session data packets (RX hasn't received session packet yet) — fixed in current build | None needed; the guard is automatic. Log should stop within 1 s |
| Segfault when running `wfb_tx` with no args | Upstream wfb-ng bug (not SHM patch) | Always pass a wlan interface. The unpatched binary has the same crash — `RawSocketTransmitter` constructor segfaults on empty wlans vector before the usage text prints |

## Ring parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Slot count | 512 | Power of 2, ~340 ms buffer at 30 fps/50 pkts per frame |
| Slot data size | maxPayloadSize + 12 | 1412 bytes default (1400 payload + 12 RTP header) |
| Total SHM size | ~724 KB | `sizeof(header) + 512 × stride` |
| Synchronization | Lock-free SPSC | `__atomic` acquire/release on indices |
| Consumer wake | Linux futex | Zero syscalls in steady state; futex only when ring empty |

---

# Runtime tuning: cleartext mode, FEC timeout, stats push

These features layer on top of the base SHM input patch and work for
both SHM input (`-H`) and UDP input (`-u`).

## FEC safety-net timeout (`-T`, runtime via `set_fec`)

The `-T <ms>` flag arms a fallback that closes the currently-open FEC
block when no input has arrived for `<ms>` milliseconds.  It exists
solely to bound the worst-case decode latency on streams that pause or
have wildly variable frame periods — under steady video the block
naturally closes when `fec_k` data fragments accumulate.

- `-T 0` (default if omitted): timeout disabled.  Blocks close only
  when full or when `CMD_SET_FEC` resets the session.
- `-T <ms>`: positive value enables the timeout.  16 ms is a sensible
  60 fps default; `fec_controller` scales it to the current FPS via
  `CMD_SET_FEC`.

The same value is exposed at runtime through the binary control
protocol — `cmd_set_fec` carries a `uint16_t fec_timeout_ms` (network
byte order).  Pass `WFB_FEC_TIMEOUT_KEEP` (`0xFFFF`) to update k/n
without touching the running timeout; pass any other value to rewrite
it.  The CLI shortcut is `wfb_tx_cmd <port> set_fec -k K -n N -T MS`
(omit `-T` to keep current).  `wfb_tx_cmd <port> get_fec` reports the
running `fec_timeout_ms`.

**Reserved value:** wire value `0xFFFF` (= 65535 ms) is the
keep-current sentinel.  Explicit timeouts are limited to **0..65534
ms** — 65 seconds is far beyond any sensible video FEC bound, so the
ceiling has no practical effect.  `wfb_tx_cmd set_fec -T 65535` is
rejected; `WfbTxControl.send_fec(..., fec_timeout_ms=65535)` is
clamped to 65534 to prevent accidental sentinel collisions.

> **Tip:** at launch (`wfb_tx -T <ms>`) the upstream CLI uses `atoi`
> and accepts any positive integer, so a launch-time value above
> 65534 will be reported by `get_fec` clamped to 65535.  Round-tripping
> that value through `set_fec` would land on the sentinel and silently
> "keep current" — which preserves the launch value, so the result is
> the same.  Set sub-65 s timeouts and this never matters.


## Cleartext data mode (`-x`)

**Default: off (AEAD on).** Opt-in for one-way broadcast workloads
(consumer FPV video) where per-fragment ChaCha20-Poly1305 AEAD is
overhead without meaningful security benefit.

**Wire format change:**

- AEAD mode: `wblock_hdr_t` → AEAD(wpacket_hdr_t + payload) + 16 B tag
- Plain mode: `wblock_hdr_t` → wpacket_hdr_t + payload (no tag)

Both sides must match or packets are silently dropped. The
`count_p_mode_mismatch` stat on RX surfaces misconfigured deploys.

### What `-x` keeps

- **Session packet authentication.** fec_k, fec_n, epoch, channel_id are
  still protected by the pre-shared `tx.key`/`rx.key` keypair. An
  attacker can't inject a fake session packet to reconfigure RX.
- **Replay dedup.** RX's `count_p_uniq` set filters by
  `(block_idx, fragment_idx)` nonce, so exact replays are still
  rejected.

### What `-x` removes

- **Per-fragment authentication.** Anyone on the channel can inject
  fragments that RX accepts (subject to FEC decode succeeding).
- **Per-fragment integrity.** A single-bit flip that slips past FEC
  reaches the video decoder. For H.264/H.265 this is usually a glitch,
  not a crash.
- **Per-fragment confidentiality.** Video content visible to anyone in
  RF range.

### When `-x` makes sense

- One-way broadcast (video from drone to ground)
- Consumer/hobbyist use where opsec isn't load-bearing
- CPU/airtime matters (saves ~4 pp wfb_cpu + 16 B × pkt/s on wire)

### When to keep AEAD on

- **Bidirectional tunnel** (RC control, telemetry) — injected control
  packets are an actual attack vector
- **Commercial deploys** with compliance requirements
- Any scenario where integrity of the stream matters

### Usage

```bash
# TX side (vehicle)
wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 -r 0.5 -x wlan0

# RX side (ground station)
sudo wfb_rx -K /etc/gs.key -i 0 -p 0 -x wlxHHHHHHHHHHHH
```

No wfb_tx_cmd for this — switching mid-session would invalidate RX's
session-key state. Choose at startup.

### Verifying

TX startup logs:

```
Startup: AEAD=off (DATA_PLAIN), k=8, n=12, fec_timeout=16, channel_id=0x0000cf
Note: RX must run with -x too; DATA_PLAIN packets are silently dropped in AEAD mode.
```

RX startup logs:

```
Startup: AEAD=off (expects DATA_PLAIN), channel_id=0x0000cf, rx_mode=local
Note: TX must run with -x too; AEAD DATA packets are silently dropped in plain mode.
```

If modes disagree, RX log will include per-interval:

```
N packets dropped (AEAD-mode mismatch, check -x flag on both sides)
```

## UDP stats push (`-Y host:port`)

Optional: emit one JSON datagram per `-l` interval to a fixed UDP target.
Lets external consumers (e.g. `fec_controller`, status WebUI, metrics
exporter) subscribe to live tx stats without attaching to wfb_tx's stdout
— so wfb_tx can be supervised independently and multiple consumers can
read the same stream.

### Usage

```bash
# Push to fec_controller on the same host
wfb_tx -K /etc/drone.key -H venc_wfb -k 8 -n 12 -p 0 -B 20 -M 3 \
       -C 8000 -l 1000 -Y 127.0.0.1:5800 wlan0
```

Cadence is the existing `-l log_interval` (milliseconds) — the JSON
emit happens at the same point as the human-readable `PKT` line, so
both share one clock. The flag is additive: stdout output is unchanged.

Init failures (bad host/port, socket() failure) log once on stderr and
leave the feature disabled — wfb_tx continues normally. Send errors are
silently ignored to avoid log spam if the consumer flaps.

### Schema

One datagram per emit, single-line UTF-8 JSON, terminated with `\n` for
line-oriented tools:

```json
{"ts_ms":1714115012345,"type":"tx_stats","ver":1,"seq":42,"interval_ms":1000,
 "tx":{"pkts_in":12345,"bytes_in":456789,
       "pkts_out":12340,"bytes_out":456000,
       "pkts_drop":3,"pkts_trunc":2,"fec_timeouts":1,
       "fec_k":12,"fec_n":20},
 "radio":{"mcs":3,"bw":20,"short_gi":0,"stbc":0,"ldpc":0,
          "vht_mode":0,"vht_nss":0}}
```

| Field | Meaning |
|---|---|
| `ts_ms` | Emit timestamp (`get_time_ms()`, monotonic-ish ms). |
| `seq` | Per-process monotonic counter, starts at 0. Lets consumers detect dropped UDP datagrams (gap > 1) and producer restarts (counter resets). Optional / additive: older wfb_tx emits no `seq` field. |
| `interval_ms` | The `-l` value at emit time. |
| `tx.pkts_in` / `bytes_in` | Incoming UDP/SHM packets received this interval. |
| `tx.pkts_out` / `bytes_out` | Successfully injected (includes FEC parity). |
| `tx.pkts_drop` | Dropped due to rxq overflow or injection timeout. |
| `tx.pkts_trunc` | Injected packets that were truncated. |
| `tx.fec_timeouts` | Empty packets sent to close FEC blocks on `-T fec_timeout`. |
| `tx.fec_k` / `fec_n` | Current FEC sizes (tracks `wfb_tx_cmd set_fec` writes). |
| `radio.*` | Current radiotap state. `short_gi`/`ldpc`/`vht_mode` are 0/1. |

`fec_k` / `fec_n` and the entire `radio` block let consumers track
external `wfb_tx_cmd set_fec` / `set_radio` calls without polling
`CMD_GET_RADIO` separately.

### Debug

**On-device** (busybox `nc` does NOT support `-u` UDP-listen mode — use
`socat`, which is preinstalled on OpenIPC):

```bash
socat -u UDP-RECV:5800,reuseaddr -
```

**From a host on the same LAN** (point `-Y` at the host IP, then use
real `nc`):

```bash
# On vehicle:
wfb_tx ... -Y 192.168.1.5:5800 ...
# On host (192.168.1.5):
nc -ul 5800 | jq -c .
```

### Versioning

Increment `ver` on incompatible changes. Adding fields without removing
or renaming existing ones is a non-breaking change (consumers ignore
unknown keys). Renames or semantics changes bump the version.

### Gotcha — `wfb_tx_cmd set_radio` is whole-struct, not partial

`wfb_tx_cmd ... set_radio` always sends a complete CMD_SET_RADIO struct.
Any flag you don't pass on the CLI is sent at the **wfb_tx_cmd
hardcoded default**, NOT preserved from wfb_tx's current state. Defaults
are: `mcs=1 bw=20 short_gi=0 stbc=0 ldpc=0 vht_mode=0 vht_nss=1`.

So `wfb_tx_cmd 8000 set_radio -M 7` doesn't only "change MCS to 7" —
it also resets `stbc/ldpc/vht_*` to their defaults, even if you started
wfb_tx with `-S 1 -L 1`. To preserve, pass everything explicitly:

```bash
# Right: full restate
wfb_tx_cmd 8000 set_radio -M 7 -B 20 -G long -S 1 -L 1
```

Two related footguns:

- **Positional args are silently ignored.** `wfb_tx_cmd 8000 set_radio
  mcs_index 7` parses as zero flags + two positional args (which
  getopt drops), then sends a CMD_SET_RADIO with all defaults
  (mcs=1, stbc=0, ldpc=0, ...). wfb_tx applies it; the operator sees
  `Radiotap updated with ... mcs_index=1` (NOT 7) but might miss the
  detail. Always use `-M N` (capital M) for mcs.
- **The `-Y` stats stream + fec_controller correctly reflect the
  applied values** (verified on hardware). If your subscriber sees
  unexpected `radio.*` after a `wfb_tx_cmd set_radio`, double-check
  the command — wfb_tx's "Radiotap updated with ..." stderr line is
  the source of truth for what actually got applied.

## UDP stats push for wfb_rx (`-Y host:port`)

Same flag, mirror semantics. Emits one JSON datagram per `-l` interval
with per-antenna RSSI/SNR plus the same aggregate counters that the
human-readable `PKT` line carries. Lets ground-side consumers
(fec_controller via uplink, status WebUI, RSSI overlays) subscribe
without bridging through `ground_rssi_forwarder.py`.

### Usage

```bash
# Ground (x86 host with WiFi adapter in monitor mode)
sudo wfb_rx -K /etc/drone.key -i 207 -p 0 -l 1000 \
            -Y 127.0.0.1:5801 wlx40a5ef2f229b
```

### Schema (`type: rx_ant`, ver 1)

One datagram per emit, single line, newline-terminated. The `ant`
array carries one entry per `(freq, mcs, bw, antenna_id)` key — so
multi-antenna RX diversity surfaces naturally as multiple entries.

```json
{"ts_ms":26279557,"type":"rx_ant","ver":1,"seq":2,"interval_ms":1000,
 "ant":[
   {"freq":5745,"mcs":5,"bw":20,"id":"1","pkts":2286,
    "rssi":{"min":-28,"avg":-27,"max":-26},
    "snr":{"min":26,"avg":31,"max":35}},
   {"freq":5745,"mcs":5,"bw":20,"id":"0","pkts":2286,
    "rssi":{"min":-38,"avg":-37,"max":-36},
    "snr":{"min":26,"avg":32,"max":38}}],
 "pkt":{"all":2308,"bytes":2953783,"dec_err":21,"session":1,"data":2286,
        "uniq":2286,"fec_recovered":49,"lost":0,"bad":0,
        "outgoing":1252,"outgoing_bytes":1699183}}
```

| Field | Meaning |
|---|---|
| `ts_ms` | Emit timestamp (`get_time_ms()`). |
| `seq` | Per-process monotonic counter (same semantics as wfb_tx). |
| `interval_ms` | The `-l` value at emit time. |
| `ant[].freq` / `mcs` / `bw` | Channel + modulation per the radiotap header. |
| `ant[].id` | Hex string of `(sin_addr<<32) \| (wlan_idx<<8) \| ant_index`. Same packing the existing IPC `RX_ANT %PRIx64` uses. |
| `ant[].pkts` | Packets seen on this antenna this interval. |
| `ant[].rssi` / `snr` | min / avg / max in dBm and dB respectively. |
| `pkt.all` / `bytes` | Total RF packets received this interval. |
| `pkt.dec_err` | AEAD/decryption failures (off-channel chatter, mismatched -x, ...). |
| `pkt.session` / `data` | Session-key vs data packet classification. |
| `pkt.uniq` | Unique frames after FEC dedup. |
| `pkt.fec_recovered` | Data packets recovered by FEC. |
| `pkt.lost` | Data packets unrecoverable. |
| `pkt.outgoing` / `outgoing_bytes` | Forwarded to `-c HOST:PORT`. |

`ant` may be empty for the first datagram or two while the receiver
warms up (no packets yet). `pkt.dec_err` may be high in the first
second when the receiver picks up pre-existing off-channel traffic
before its first session-key sync.

### Backward compat

Same rules as wfb_tx: omitting `-Y` keeps the stdout-only behavior;
the `seq` field is additive on `ver:1`; consumers ignore unknown
fields. `ground_rssi_forwarder.py` is **deprecated** — for new
deployments, point a JSON-aware consumer at `wfb_rx -Y` directly.

### Debug

```bash
# On the host running wfb_rx:
nc -ul 5801 | jq -c '{seq, ant_count: (.ant | length), pkt_lost: .pkt.lost}'
```

## Quick reference — recommended FPV config

For a typical 25 Mbps H.265 FPV video stream (vehicle → ground, one-way):

```bash
# Vehicle
wfb_tx -K /etc/drone.key -M 5 -B 20 -k 8 -n 12 -P 1 -Q -S 1 -L 1 \
       -C 8000 -H venc_wfb -R 2097152 -s 2097152 -l 1000 -i 0 -p 0 \
       -T 16 -x wlan0

# Ground
sudo wfb_rx -K /etc/gs.key -i 0 -p 0 -x wlxHHHHHHHHHHHH

# Runtime tuning on vehicle (examples)
wfb_tx_cmd 8000 get_fec                      # k=8, n=12, fec_timeout_ms=16
wfb_tx_cmd 8000 set_fec -k 10 -n 15          # change FEC live (timeout untouched)
wfb_tx_cmd 8000 set_fec -k 10 -n 15 -T 33    # also drop timeout to 30 fps
```
