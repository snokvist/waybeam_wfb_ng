# waybeam_wfb_ng

Adaptive FEC + MCS controller, WCMD command channel, CSA orchestration,
and zero-copy SHM video path for [wfb-ng](https://github.com/svpcom/wfb-ng)
broadcast.  Two daemons:

- **`vehicle/link_controller`** — runs on the SigmaStar Infinity6E vehicle.
  Subscribes to the venc RTP sidecar, sizes FEC k/n, picks MCS, applies
  WCMD frames coming up the uplink, runs CSA on demand.  Exposes a
  status WebUI on port 8765.
- **`ground/gs_supervisor`** — runs on the GS host (x86 or aarch64).
  Forks/manages `wfb_rx` + `wfb_tx`, exposes a REST API + WebUI for
  lifecycle control, channel scan, CSA orchestration, and WCMD emit
  toward the vehicle.  Listens on port 9080.

## System diagram

```
 GROUND STATION (x86 host or aarch64)
 ═══════════════════════════════════════
 ┌──────────────────────────────────────────────────────┐
 │ gs_supervisor                              :9080 ◄── operator
 │   ├─ forks N×wfb_rx (video downlink)                 │
 │   ├─ forks 1×wfb_tx (uplink for WCMD + CSA)          │
 │   ├─ /api/v1/cmd          → 3-burst WCMD over uplink │
 │   │   └─ /request/idr     → alias for ?key=force_idr │
 │   ├─ /api/v1/system/csa   → 5-frame burst + iw hop   │
 │   ├─ /api/v1/system/scan  → channel sweep            │
 │   └─ embedded WebUI       → tunnels / GS / Vehicle   │
 └────┬──────────────────────────────────────┬──────────┘
      │ wfb_rx                                │ wfb_tx
      │ (decoded video to                     │ (WCMD + CSA frames)
      │  127.0.0.1:5600)                      │
      ▼                                       ▼
                            [ over the air ]
                                   ▲
                                   │
 VEHICLE (SigmaStar Infinity6E, armv7l, OpenIPC Linux)
 ════════════════════════════════════════════════════════
 ┌────────────┐   SHM ring   ┌─────────────────────┐
 │ waybeam_   ├─────────────►│ wfb_tx (patched -H) │
 │ venc       │   zero-copy  │ FEC encode + inject │
 │ + RTP      │              └─────────────────────┘
 │ + sidecar  │
 └────┬───────┘
      │ UDP sidecar (rtp_sidecar.h: per-frame size,fps,bitrate,…)
      ▼
 ┌──────────────────────────────────────────────────────┐
 │ link_controller                            :8765 ◄── operator
 │   ├─ FEC subsystem      (EWMA + bounded headroom)    │
 │   ├─ MCS subsystem      (rx_ant scoring + selector)  │
 │   ├─ WCMD dispatcher    (key→HTTP/wfb_cmd/iw)        │
 │   ├─ CSA receiver lib   (csa.h: csa_feed/csa_tick)   │
 │   └─ wfb_cmd writer     (set_fec, set_radio)         │
 └──────────────────────────────────────────────────────┘
```

## Repository layout

```
ground/                  GS-side daemon
  gs_supervisor.c          (3651 LOC)  REST API + WebUI + tunnel supervision
  webui/gs.html            Tunnels / GS Control / Vehicle Control tabs
  config/example.json      reference config + host_x86.json
  scripts/                 wfb_rx_to_backpack.py, ground_rssi_forwarder.py
  Makefile                 make / make cross / make webui / make deploy

vehicle/                 vehicle-side daemon (single binary)
  link_controller.c        (5365 LOC)  FEC + MCS + WCMD + CSA + WebUI
  csa/                     CSA receiver library + agents
  webui/index.html         status WebUI
  init/S99wfb              OpenIPC init script
  Makefile                 make (cross) / make host / make webui / make deploy

shared/                  cross-side wire-format headers (single source of truth)
  wcmd_proto.h             WCMD command channel (16-byte req/resp)
  rtp_sidecar.h            venc per-frame metadata wire format

wfb-ng/                  patched wfb-ng fork (build artefacts)
  shm-input.patch          SHM input + -Y stats push
  build-{armv7,aarch64,openwrt}.sh

probes/                  host-native dev tools
  rtp_timing_probe.c

docs/                    design notes + protocol specs
  protocols/{wcmd-proxy,shm-input,rtp-sidecar}.md
  design/{gs-supervisor,variable-payload}.md

tests/                   default = wire-format conformance (58 tests)
  protocols/               frozen Python parsers vendored under _proto/

archive/                 superseded code (kept in-tree for reference)
  python/                  fec_controller/ + mcs_selector/ + 328 legacy tests
  c-poc/                   standalone C variants subsumed by link_controller
  init-old/                old GS init scripts replaced by gs_supervisor

FOLLOWUPS.md             deferred polish across both daemons
Makefile                 top-level umbrella (make all / test / clean)
```

## Quickstart

### Build everything (host, no toolchain needed)

```bash
make
make test           # 58 wire-format conformance tests
```

### Cross-build for the vehicle (Infinity6E, armv7l)

```bash
# Repo expects the toolchain at ./toolchain/ (typically a symlink into
# a sibling waybeam_venc checkout that vendors the OpenIPC SDK).
make vehicle
```

### Cross-build the ground supervisor for an aarch64 station

```bash
make -C ground cross CROSS_CC=aarch64-linux-gnu-gcc \
                     OUT_CROSS=build/gs_supervisor.aarch64
```

### Deploy

```bash
# Vehicle: scp link_controller and restart wfb-ng init
make -C vehicle deploy DEPLOY_HOST=192.168.1.13
ssh root@192.168.1.13 '/etc/init.d/S99wfb restart'

# Ground: scp gs_supervisor and restart (config under ground/config/)
make -C ground deploy DEPLOY_HOST=192.168.2.20
```

### WebUIs

- Vehicle:  `http://<vehicle-ip>:8765`
- Ground:   `http://<gs-ip>:9080`

## How the pieces fit together

### Video path (vehicle → ground)

`waybeam_venc` encodes H.265, writes RTP packets to a POSIX shared-memory
ring.  Patched `wfb_tx` reads the ring with `-H local_shm` (zero-copy),
applies FEC, and injects on the air.  Ground's `wfb_rx` decodes and
forwards to `127.0.0.1:5600` for the player.

### Control path (vehicle → vehicle, via venc sidecar)

`venc` emits one UDP datagram per encoded frame (size, fps, QP, etc.)
to a sidecar port.  `link_controller` subscribes, sizes FEC k/n via
EWMA + bounded headroom, and writes `CMD_SET_FEC` to `wfb_tx`'s control
port.  No round-trip; loss-rate feedback is not used (see "FEC gating
design" below).

### Control path (ground → vehicle, via uplink)

Operator hits a button in the GS WebUI → `gs_supervisor` emits a 16-byte
WCMD frame (3 redundant copies, same seq) to its uplink `wfb_tx` UDP
input port → over the air → vehicle's `wfb_rx` → `link_controller`'s
WCMD dispatcher demuxes (4-byte "WCMD" magic distinguishes from rx_ant
JSON) → applies via venc HTTP / `wfb_cmd` / `iw`.  Per-key 500 ms seq
dedup window collapses redundant copies to one apply.

### Channel switch (ground orchestrates both)

`POST /api/v1/system/csa` on the GS sends 5 `csa_commit` JSON frames at
20 ms cadence to the vehicle.  Both sides hop with `iw set channel` at
the same `T_switch`.  If no rx traffic arrives within `t_revert_ms`,
both sides revert to the previous channel.

## FEC gating design (vehicle side)

Asymmetric — fast increase, slow decrease (mirror of TCP AIMD):

| Direction | Hysteresis | Cooldown | Notes |
|---|---|---|---|
| Increase | 1 sample | 0.1 s | Under-protection is far worse than over-protection |
| Decrease | 3 samples | 2.0 s | Avoid oscillation under bursty I/P imbalance |
| Oscillation | — | × 3 multiplier | After >4 changes in 5 s |

Sizing: `k = ceil(EWMA_size / MTU) × headroom`, with headroom learned
from observed I/P ratio (1.05–1.40).  I-frames that exceed `k×MTU` span
multiple FEC blocks, but the patched `wfb_tx` honours the RTP M-bit and
closes each frame's final block on the boundary, so single-block loss
never contaminates adjacent frames.

## WCMD redundancy

Every WCMD from `gs_supervisor /api/v1/cmd` goes out as 3 redundant
copies sharing the same seq.  The vehicle keeps a 500 ms per-key dedup
window, so a single FEC-block loss on the uplink no longer drops the
command and duplicates don't double-apply.  The `coalesced_burst` counter
in `/cmd/status` reports how often dedup fired.

## Compatibility / wire formats

`shared/wcmd_proto.h` and `shared/rtp_sidecar.h` are the single sources
of truth for cross-daemon wire formats.  `tests/protocols/` keeps a
vendored Python parser as a regression net — change either header,
update the vendored copy, run `make test`.

## Status

| Component | State |
|---|---|
| `vehicle/link_controller` | **production** on Infinity6E vehicles |
| `ground/gs_supervisor`    | **production** on x86 + aarch64 GS hosts |
| `wfb-ng/shm-input.patch`  | **production** (vendored fork) |
| `archive/python/`         | superseded — reference only, not deployed |
| `archive/c-poc/`          | superseded — algorithmic reference |

## Contributing

- Cross-cutting cleanup: see `FOLLOWUPS.md` for deferred polish items.
- Wire-format changes: bump the C header in `shared/`, the vendored
  Python copy in `tests/protocols/_proto/`, and add a round-trip test.
- New WCMD keys: extend `shared/wcmd_proto.h`, the dispatcher in
  `vehicle/link_controller.c`, the emitter + WebUI in `ground/`.
- License: see existing files.
