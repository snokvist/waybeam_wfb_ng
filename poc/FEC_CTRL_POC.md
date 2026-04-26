# fec_controller POC (C, on-device)

Minimal proof-of-concept C port of the Python `fec_controller`, designed
to run on-device on SigmaStar Infinity6E (star6e, e.g. 192.168.1.13).

It:

1. Subscribes to waybeam_venc's sidecar (default `127.0.0.1:6666`), parses
   per-frame metadata, and extracts `frame_size_bytes` from the optional
   `ENC_INFO` trailer.
2. Feeds frame sizes through EWMA + bounded-headroom sizing to pick an
   optimal `(k, n)` for one-frame-≈-one-FEC-block operation.
3. Asymmetric gating: fast up (Δk ≥ 2 after 1.0 s cooldown), slow down
   (candidate must hold below `current.k` for `--k-down-dwell` seconds,
   default 8.0 s — pure anti-bounce on content jitter).
4. Polls wfb_tx radio params (`CMD_GET_RADIO`) every 1 s to learn current
   MCS / bandwidth / GI, computes a PHY-rate estimate, and asserts a safe
   video bitrate via `/api/v1/set?video0.bitrate=N`:

   ```
   safe_kbps = phy_mbps * 1000 * (k / n) * safety_margin
   target    = clamp(safe_kbps, --bitrate-min, --bitrate-max)
   ```

   The controller tracks `last_written_kbps` internally — there is no
   HTTP READ. If something else changes `video0.bitrate` externally, the
   next 1 Hz tick re-asserts `target` and the EWMA absorbs the resulting
   transient through normal gating.
5. Emits `CMD_SET_FEC (k, n)` to wfb_tx on every committed update.

The controller is **purely reactive**: it never touches MCS itself. If
MCS, BW, GI, VHT mode, or NSS change externally (operator, other daemon,
hardware re-init), the next radio poll detects it, arms the long settle
window so the EWMA can re-converge to the new frame-size distribution,
and resumes normal sizing. An MCS-DOWN additionally arms a brief
parity boost while the EWMA catches up.

Single thread, single `poll()` loop. No libs beyond libc.

## Requirements on the device

- `waybeam_venc` running with:
  - `outgoing.sidecarPort = 6666`
  - `ENC_INFO` trailer present in sidecar frames
    (i.e. venc built with encoder-feedback telemetry)
- `wfb_tx` started with a control port set to **8000** (matches
  `--wfb` default).  Use `wfb_tx -C 8000 …` or your wrapper's equivalent.
- `waybeam_venc` HTTP API reachable on `127.0.0.1:80` (default), used
  for the `set?video0.bitrate=N` write only.

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
--safety  0.50
```

Dry run (compute-only, no `set_fec` / `set?video0.bitrate=` writes):

```bash
/tmp/fec_controller -v --dry-run
```

## Logs

Initial subscribe + radio discovery, then on every committed FEC change
or bitrate assertion:

```
[fec t=   0.000] sidecar=127.0.0.1:6666 wfb=127.0.0.1:8000 venc=127.0.0.1:80 mtu=1446 safety=0.50 dry_run=0
[fec t=   0.001] sent SUBSCRIBE
[fec t=   0.502] radio: mcs=3 bw=20 gi=long vht=0 nss=0 phy=26.0 Mbps
[fec t=   2.105] FEC init: k=4 n=7 (avg=4832B hd=1.15 ppf=4 red=0.40 fps=60.2)
[fec t=   2.106] bitrate init -1 -> 7428 kbps (phy=26.0 Mbps, k/n=4/7, safe=7428)
[fec t=   8.443] FEC update: k=6 n=10 (avg=6041B hd=1.18 ppf=5 red=0.40 fps=60.1)
```

External MCS change (operator runs `wfb_tx_cmd set_radio` from outside):

```
[fec t=  42.103] radio: external change mcs 3->1 bw 20->20 gi 0->0 vht 0->0 nss 0->0 (phy=13.0Mbps)
[fec t=  42.103] fec: parity boost armed for 3.0s (mult=1.30) [external MCS drop]
[fec t=  47.108] FEC update: k=11 n=18 (avg=10524B hd=1.22 ppf=11 red=0.39 fps=59.8)
[fec t=  47.108] bitrate down 7428 -> 3635 kbps (phy=13.0 Mbps, k/n=11/18, safe=3635)
```

## FEC-emit gating

Each `CMD_SET_FEC` re-inits the session key on wfb_tx and causes the
receiver to briefly drop in-flight packets while the new announce
propagates. To keep that cost low, the controller uses these defaults:

| Knob | Default | Role |
|---|---|---|
| `--k-hyst-up`     | 2     | Min Δk to trigger an up-move (single-step wobble ignored). |
| `--cooldown-up`   | 1.0 s | Min seconds between up-moves. |
| `--k-down-dwell`  | 8.0 s | Candidate must hold below `current.k` for this duration. Pure anti-bounce. Any change in pending target (lower or higher) restarts the timer. Longer than `--mcs-settle-s` so an MCS edge-trigger always wins the race. |
| `--startup-grace` | 2.0 s | No emits for the first 2 s — lets the EWMA settle before first commit. |
| `--bitrate-grace` | 2.0 s | After our own bitrate write, suppress non-edge FEC emits for this long while the venc rate-controller converges. |
| `--mcs-settle-s`  | 5.0 s | After a detected (external) MCS change, suppress non-edge FEC emits while the EWMA tracks the new frame-size distribution. Edge-triggered commit fires once when the window expires (allowed in either direction, bypasses the k-down dwell). |

**No `--cooldown-down`.** The dwell is the anti-bounce; adding a
post-commit cooldown would be redundant gating.

## Bitrate: track the link budget (no HTTP READ)

The controller TRACKS the safe link budget, not a fixed user-set value.
Every `--radio-poll-s` seconds (1 s default) it computes:

```
safe   = phy_mbps * 1000 * (k/n) * --safety
target = clamp(safe, --bitrate-min, --bitrate-max)
```

…and writes `video0.bitrate = target` via HTTP whenever
`|target - last_written_kbps|` exceeds `--bitrate-tol * target`
(15% default), in either direction. When MCS climbs the bitrate is
pushed up to use the extra headroom; when MCS drops or the FEC parity
grows, it's clamped back.

The controller does NOT poll venc for the current bitrate. If an
external actor (web UI, curl, sibling daemon) changes `video0.bitrate`,
the next 1 Hz tick still asserts `target`, silently overwriting them.
The EWMA naturally absorbs the resulting frame-size transient — no
explicit detection or special grace window needed, because the existing
k-down dwell + k-up cooldown handle it.

If you want to disable the controller's bitrate writes (e.g. you're
hand-tuning), pass `--dry-run`.

Defaults: `--bitrate-min=1000`, `--bitrate-max=0` (unlimited, cap only
by `safe`). The log tags moves as `bitrate up …`, `bitrate down …`, or
`bitrate init …` (the very first assertion).

> `--bitrate-desired` is kept as a deprecated alias for `--bitrate-max`
> so older scripts still work; it prints a deprecation notice.

## External MCS-change reactivity

The radio poll runs every 1 s. After each successful `CMD_GET_RADIO`,
the controller compares the new snapshot against the previous one. Any
change in `mcs / bandwidth / short_gi / vht_mode / vht_nss` is treated
as external (the controller never initiates) and triggers:

1. A log line: `radio: external change mcs A->B bw … (phy=X Mbps)`.
2. `controller_arm_settle(--mcs-settle-s)` — suppresses non-edge FEC
   emits while the EWMA tracks the new frame-size distribution.
3. If `mcs_changed && new_mcs < prev_mcs`: `controller_arm_boost()` —
   inflates parity by `--boost-mult` for `--boost-s` seconds. Force-emit
   is edge-triggered (once on entry, once on expiry).
4. When the settle window expires, an edge-triggered FEC commit fires
   once in whichever direction the new steady-state EWMA suggests
   (bypasses the k-down dwell — this is the only path where k can drop
   immediately).

This preserves the exact "extra parity while EWMA catches up after a
phy-rate drop" semantic that the (now-removed) RSSI-driven scaler used
to provide, just driven by observation rather than self-action.

## Heartbeat

Every 5 s the controller logs one `hb:` line (always on, not behind
`-v`):

```
[fec t=  35.012] hb: k=15 n=25 avg=14.9kB fps=59.9 mcs=4 phy=39.0Mbps br=14625kbps upd=9 BOOST
```

`BOOST` is shown only while the parity-boost window is active.

## Out of scope

- RSSI ingest / RSSI-driven MCS scaling. Removed; the controller is
  purely reactive to externally-set MCS/bitrate now.
- Persisting state across restarts.
- Multiple streams.
- TLS / auth on the venc HTTP API.

## Known gotchas

- The POC requires the `ENC_INFO` trailer — without it, it has no
  per-frame byte counts and will never emit `CMD_SET_FEC`. Verify with
  `rtp_timing_probe` (`frame_size_bytes` column must not be `-`).
- `phy_mbps()` covers the common cases (HT/VHT, NSS 1..N). It is a
  rate-table approximation, not an airtime calculator — good enough for
  a safety-margin budget, not for precise scheduling.
- On first run, the headroom is at the `headroom_min` floor (1.05)
  until the rolling window fills. `k` may briefly undershoot during the
  first ~2.5 s of streaming.
- External venc.bitrate changes are silently overwritten on the next
  1 Hz tick. If you need the controller to respect them, stop it
  (`--dry-run` keeps the FEC sizing without writing bitrate).
