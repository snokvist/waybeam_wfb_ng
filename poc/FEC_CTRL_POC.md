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

   If `video0.bitrate` (queried from the venc HTTP API) exceeds that
   budget, the POC calls `/api/v1/set?video0.bitrate=N` to clamp it.
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

## Out of scope

- RSSI-driven MCS scaler (future: read per-link `wfb_rx` stats or driver
  signal/noise, then choose a higher/lower MCS via `CMD_SET_RADIO`).
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
