# fec_controller POC (C, on-device)

Minimal proof-of-concept C port of the Python `fec_controller`, designed
to run on-device on SigmaStar Infinity6E (star6e, e.g. 192.168.1.13).

It:

1. Subscribes to waybeam_venc's sidecar (default `127.0.0.1:6666`), parses
   per-frame metadata, and extracts `frame_size_bytes` from the optional
   `ENC_INFO` trailer.
2. Feeds frame sizes through EWMA + bounded-headroom sizing to pick an
   optimal `(k, n)` for one-frame-≈-one-FEC-block operation.
3. Asymmetric gating: fast increase (Δk ≥ 1 after 0.1 s), slow decrease
   (Δk ≥ 3 after 2.0 s). Same rules as `fec_controller/controller.py`.
4. Queries wfb_tx radio params (`CMD_GET_RADIO`) to learn current
   MCS / bandwidth / GI, computes a PHY-rate estimate, and derives a
   safe video bitrate:

   ```
   safe_kbps = phy_mbps * 1000 * (k / n) * safety_margin
   ```

   If `video0.bitrate` (queried from the venc HTTP API as
   `GET /api/v1/get?video0.bitrate` — the field name is the query key,
   not a `field=` value) exceeds that budget, the POC calls
   `/api/v1/set?video0.bitrate=N` to clamp it.
5. Emits `CMD_SET_FEC (k, n)` to wfb_tx on every committed update.

Single thread, single `poll()` loop. No libs beyond libc.

## Requirements on the device

- `waybeam_venc` running with:
  - `outgoing.sidecarPort = 6666`
  - `ENC_INFO` trailer present in sidecar frames
    (i.e. venc built with encoder-feedback telemetry)
- `wfb_tx` started with a control port set to **8000** (matches
  `--wfb` default).  Use `wfb_tx -C 8000 …` or your wrapper's equivalent.
- `waybeam_venc` HTTP API reachable on `127.0.0.1:80` (default).

## Build

From `waybeam_wfb_ng/poc/`:

```bash
# Cross-build for star6e
make -f Makefile.fec_controller

# Output
ls build/fec_controller
```

The Makefile uses `../toolchain/toolchain.sigmastar-infinity6e/` — same
toolchain as `build_wfb_tx.sh`.

### Host build (for syntax/debug only)

```bash
make -f Makefile.fec_controller host
./build/fec_controller.host --help
```

## Deploy

```bash
make -f Makefile.fec_controller deploy
# or manually:
scp -O build/fec_controller root@192.168.1.13:/tmp/
```

## Run (on device)

```bash
/tmp/fec_controller -v
```

Default CLI:

```
--sidecar 127.0.0.1:6666
--wfb     127.0.0.1:8000
--venc    127.0.0.1:80
--mtu     1446
--safety  0.60
```

Dry run (compute-only, no set_fec / bitrate writes):

```bash
/tmp/fec_controller -v --dry-run
```

## Logs

Initial subscribe + radio discovery, then on every committed FEC change:

```
[fec] sidecar=127.0.0.1:6666 wfb=127.0.0.1:8000 venc=127.0.0.1:80 mtu=1446 safety=0.60 dry_run=0
[fec] sent SUBSCRIBE
[fec] radio: mcs=3 bw=20 gi=long vht=0 nss=0 phy=26.0 Mbps
[fec] FEC init: k=4 n=7 (avg=4832B hd=1.15 ppf=4 red=0.40 fps=60.2)
[fec] bitrate: cur=8192 safe=8914 (phy=26.0 k/n=4/7)
[fec] FEC update: k=5 n=8 (avg=6041B hd=1.18 ppf=5 red=0.38 fps=60.1)
[fec] clamp bitrate 12000 -> 8914 kbps (phy=26.0 Mbps, k/n=4/7, safety=0.60)
```

## MCS scaler (opt-in)

RSSI-driven MCS ladder that chooses a conservative-ish MCS based on a
wfb_rx-formatted text stream. Off by default — enable with
`--mcs-enable --rssi-stream PATH`.

```
--mcs-enable                # explicit opt-in
--mcs-min N / --mcs-max N   # ladder floor/ceiling (defaults 1..3)
--rssi-stream PATH          # file/FIFO/stdin to tail
--rssi-silence F            # stale threshold (default 1.5 s)
--mcs-climb F               # climb dwell (default 2.0 s)
--mcs-drop F                # drop dwell  (default 0.3 s)
--mcs-cooldown F            # min seconds between moves (default 3.0)
--boost-s F                 # post-drop FEC-parity boost (default 3.0)
--boost-mult F              # parity multiplier during boost (default 1.3)
```

**Expected input format** (mirrors `wfb_rx -x` stdout):

```
34001586    RX_ANT    5745:5:20    1    1082:-30:-28:-28:24:31:34
ts_ms       tag       freq:mcs:bw  ant  pkts:rmin:ravg:rmax:smin:savg:smax
```

We take the `ravg` field, aggregate the **max** across antennas within a
250 ms window, and EWMA it (α=0.3). The ladder (HT20, LGI, single-stream)
has climb and drop thresholds with a ~4 dB hysteresis band, from MCS 0
(−200, unreachable) through MCS 7 (−58 climb / −62 drop).

### Ground client — `ground_rssi_forwarder.py`

The RSSI is measured at the **ground station's** wfb_rx (the receiver of
the video downlink). To get it back to the vehicle, we use a simple
ground→vehicle chain over wfb-ng's existing uplink:

```
 VEHICLE (venc)                      GROUND (laptop)
   │                                    │
   │ video downlink                     │
   └───────────► wfb_rx ──► stdout ──► ground_rssi_forwarder.py
                                          │
                                          │ UDP loopback (--target HOST:PORT)
                                          ▼
                                        wfb_tx -u PORT          (uplink)
                                          │ over the air
   ┌────────────◄ wfb_rx -u PORT  (on vehicle, decapsulates)
   │ UDP loopback
   ▼
 fec_controller --rssi-udp PORT
```

The forwarder reads wfb_rx stdout from one of three sources:

```bash
# Pipe from an already-running wfb_rx:
./wfb_rx -K drone.key -i 207 -x -p 0 wlan0 \
  | python3 poc/ground_rssi_forwarder.py --target 127.0.0.1:5700

# Spawn wfb_rx as a subprocess:
python3 poc/ground_rssi_forwarder.py \
  --spawn './wfb_rx -K drone.key -i 207 -x -p 0 wlan0' \
  --target 127.0.0.1:5700

# Tail a log file (when some other tool already runs wfb_rx):
python3 poc/ground_rssi_forwarder.py \
  --tail /var/log/wfb_rx.log --target 127.0.0.1:5700
```

The forwarder throttles to 4 Hz by default (`--throttle-hz 0` to disable)
and sends each `RX_ANT` line verbatim as one UDP datagram.

### Vehicle-side RSSI ingest

`fec_controller` accepts either a text stream (`--rssi-stream`) or a UDP
listener (`--rssi-udp`) — not both.

```bash
# On vehicle, RSSI arriving via wfb_rx uplink decapsulation:
/tmp/fec_controller --mcs-enable --rssi-udp 5701

# Or the ingest-less shortcut for testing: forward directly over the
# LAN (no radios in the loop):
python3 poc/ground_rssi_forwarder.py \
  --target <vehicle_ip>:5701 < fake_rssi.txt
```

The second form was how the POC was integration-tested end-to-end.

On an MCS drop, the controller arms a **parity boost**: for the next
`--boost-s` seconds it inflates the FEC parity (`n-k`) by `--boost-mult`.
The force-emit is **edge-triggered** — once on boost entry (to commit the
inflated parity) and once on expiry (to restore natural parity). During
the boost window, FEC updates follow the normal hysteresis/cooldown; this
prevents per-frame `CMD_SET_FEC` floods when the EWMA wobbles k by ±1.

**Startup snap-in:** if the hardware MCS is outside the `[mcs_min, mcs_max]`
policy range, the scaler issues an immediate `CMD_SET_RADIO` on first sync
to bring the radio into the allowed band. Logged as
`mcs: snap 5 -> 3 (outside policy [1..3])`.

**RSSI-loss fallback:** if RSSI goes silent for more than
`--rssi-fallback-s` (default 4.0) seconds AFTER an EWMA has been
established, the scaler snaps to `--fallback-mcs` (default = `mcs_min`;
set to `0` for the most robust modulation). A parity boost is armed at
the same time. When RSSI returns, the radio is snapped back to
`mcs_min` and the normal ladder resumes:

```
[fec t=   9.150] rssi: FALLBACK — no RSSI for >4.0s, snapping mcs 5 -> 0
[fec t=   9.150] fec: parity boost armed for 3.0s (mult=1.30) [fallback]
[fec t=   9.152] bitrate down 23276 -> 2869 kbps (phy=6.5 Mbps, …)
[fec t=  14.253] rssi: recovered (rssi=-50.0 dBm), exiting fallback -> mcs=1
[fec t=  14.254] bitrate up 2340 -> 4680 kbps (phy=13.0 Mbps, …)
[fec t=  17.305] mcs: 1 -> 2 (rssi=-50.0 dBm) CLIMB
```

The fallback is gated on `have_ewma` — it only fires once the controller
has actually established an EWMA from real packets, so the subscribe
warm-up period does not count as "RSSI lost".

### Bitrate: track the link budget

The controller **tracks** the safe link budget, not a fixed user-set
value. Every `--bitrate-poll-s` seconds (1 s default) it computes

```
safe   = phy_mbps * 1000 * (k/n) * --safety
target = clamp(safe, --bitrate-min, --bitrate-max)
```

…and re-applies `video0.bitrate = target` whenever the live value drifts
more than `--bitrate-tol` (5% default) from the target, in **either**
direction. When MCS climbs the bitrate is pushed up to use the extra
headroom; when MCS drops or the FEC parity grows, it's clamped back.

Defaults: `--bitrate-min=1000`, `--bitrate-max=0` (unlimited, cap only by
`safe`). The log tags moves as `bitrate up …` or `bitrate down …`.

This also doubles as periodic drift correction for external changes —
whatever the live venc bitrate is (web UI edit, curl from another
terminal), it converges back to the controller's target within one poll.

> `--bitrate-desired` is kept as a deprecated alias for `--bitrate-max`
> so older scripts still work; it prints a deprecation notice.

### Heartbeat

Every 5 s the controller logs one `hb:` line (always on, not behind
`-v`):

```
[fec] hb: k=15 n=25 avg=14.9kB fps=59.9 phy=39.0Mbps upd=9 rssi=-77.3 mcs=4 BOOST
```

`BOOST` is shown only while the parity-boost window is active. `rssi` /
`mcs` fields appear only when the scaler is enabled.

## Out of scope

- Persisting state across restarts.
- Multiple streams.
- TLS / auth on the venc HTTP API.

## Known gotchas

- The POC requires the `ENC_INFO` trailer — without it, it has no
  per-frame byte counts and will never emit `CMD_SET_FEC`. Verify with
  `rtp_timing_probe` (`frame_size_bytes` column must not be `-`).
- `phy_mbps()` covers the common cases (HT/VHT, NSS 1..N). It is a
  rate-table approximation, not an airtime calculator — good enough for a
  safety-margin budget, not for precise scheduling.
- On first run, the headroom is at the `headroom_min` floor (1.05) until
  the rolling window fills. `k` may briefly undershoot during the first
  ~2.5 s of streaming.
