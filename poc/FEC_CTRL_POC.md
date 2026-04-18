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

On an MCS drop, the controller arms a **parity boost**: for the next
`--boost-s` seconds it inflates the FEC parity (`n-k`) by `--boost-mult`
and force-emits `CMD_SET_FEC` past the normal hysteresis/cooldown, so
redundancy rises temporarily while goodput shrinks. On expiry it forces
one more emission to restore natural parity.

### Heartbeat

Every 5 s the controller logs one `hb:` line (always on, not behind
`-v`):

```
[fec] hb: k=15 n=25 avg=14.9kB fps=59.9 phy=39.0Mbps upd=9 rssi=-77.3 mcs=4 BOOST
```

`BOOST` is shown only while the parity-boost window is active. `rssi` /
`mcs` fields appear only when the scaler is enabled.

## Out of scope

- RSSI transport — the POC reads a raw wfb_rx text stream; the actual
  wire that gets RSSI from the ground station to the vehicle is *your*
  existing wfb-ng setup (see "RSSI source" note below).
- Persisting state across restarts.
- Multiple streams.
- TLS / auth on the venc HTTP API.

### RSSI source note

The scaler needs RSSI measured at the video **receiver** (ground
station). The vehicle has no local access to that data; it must be
shipped in. The POC is agnostic to how — it just tails a file, FIFO, or
stdin. Typical wiring options:

- **SSH pipe from ground**: `ssh root@star6e '/tmp/fec_controller … --rssi-stream -' < <(wfb_rx … 2>/dev/null)`
- **FIFO fed by a separate shipper** that parses ground wfb_rx stdout
  and forwards RX_ANT lines over a wfb-ng uplink wfb_tx channel.
- **File tail** if an existing wfb-ng agent already writes RX_ANT lines
  to a log file on the vehicle.

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
