# waybeam_wfb_ng

## What this is

Adaptive FEC + MCS controller, WCMD command channel, CSA orchestration,
and zero-copy SHM video path for wfb-ng broadcast.  Two daemons:

- `vehicle/link_controller` — runs on the SigmaStar Infinity6E vehicle.
  Subscribes to the venc RTP sidecar, sizes FEC k/n, picks MCS, applies
  WCMD frames coming up the uplink, and runs CSA on demand.
- `ground/gs_supervisor` — runs on the GS host (x86 or aarch64).
  Forks/manages `wfb_rx` + `wfb_tx`, exposes a REST API + WebUI for
  lifecycle control, channel scan, CSA orchestration, and WCMD emit
  toward the vehicle.

The Python implementation under `archive/python/` (fec_controller,
mcs_selector) is the historical predecessor.  It's no longer deployed.

## Architecture

- **Video path**: venc → SHM ring (zero-copy) → wfb_tx (patched `-H`) → radio
- **Control path**:
  - vehicle: venc sidecar (UDP) → link_controller → wfb_tx CMD_SET_FEC + venc HTTP
  - ground: gs_supervisor → uplink wfb_tx (WCMD on udp_in_port) → vehicle link_controller
- **CSA**: ground arms, sends 5 csa_commit JSON frames @ 20 ms cadence
  → vehicle csa_feed schedules iw set channel → both sides hop in lockstep
- **Stateless FEC sizing**: k from EWMA × bounded headroom; the peek
  per-frame FEC close (RTP M-bit) aligns each frame's final block so
  block loss doesn't contaminate adjacent frames. Single close mode —
  the **gate** (only closes blocks ≥ k/2 full — per-frame isolation
  traded for airtime). The earlier opt-in `--peek-short-tail`
  (proportional parity) and the NAL-aware idr/refpred profiles were
  removed (PR #76); `--peek-profile` now accepts only `off|close`. The
  design rationale survives in `docs/design/peek-proportional-parity.md`.

## Repository layout

```
ground/                  GS-side daemon
  gs_supervisor.c        forks wfb_rx/wfb_tx, REST API, embedded WebUI
  webui/gs.html          tabs: Tunnels / GS Control / Vehicle Control
  config/                example.json + host_x86.json
  scripts/               wfb_rx_to_backpack.py, ground_rssi_forwarder.py
  Makefile               make / make cross / make webui / make deploy

vehicle/                 vehicle-side daemon (single binary)
  link_controller.c      adaptive FEC + MCS + WCMD proxy + CSA + WebUI
  csa/                   CSA receiver library (csa.{h,c}) + agents
  webui/index.html       link_controller status WebUI
  init/S99wfb            OpenIPC init script
  Makefile               make (cross armv7l) / make host / make webui

shared/                  cross-side wire-format headers (single source of truth)
  wcmd_proto.h           WCMD command channel (16-byte req/resp)
  rtp_sidecar.h          venc per-frame metadata wire format

multicall/               busybox-style mega-binary dispatcher (opt-in)
  wfb_multicall.{cpp,h}  generic main(): route applet by argv[0]/subcommand
                         (per-side tables: ground/gs_applets.cpp,
                          vehicle/air_applets.cpp). See docs/design/mega-binary.md.

wfb-ng/                  patched wfb-ng fork (build artefacts only)
  shm-input.patch        SHM input + -Y stats push for wfb_tx/wfb_rx
  peek.patch             per-frame FEC close (peek-profile off|close)
  mega.mk                shared mega-binary object rules (incl. by both Makefiles)
  build-armv7.sh         cross-build for Infinity6E
  build-aarch64.sh       cross-build for ground aarch64
  build-openwrt.sh       cross-build for OpenWRT MIPS

probes/                  host-native dev tools
  rtp_timing_probe.c     subscribe to venc sidecar, dump frame timings
  Makefile

docs/                    design + protocol specs
  protocols/             wcmd-proxy, shm-input, rtp-sidecar
  design/                gs-supervisor, variable-payload sizing analysis

tests/                   default = wire-format conformance
  protocols/             58 tests, frozen Python parsers vendored under _proto/

archive/                 superseded code (kept in-tree for reference)
  python/                fec_controller/ + mcs_selector/ + 328 legacy tests
  c-poc/                 standalone C variants subsumed by link_controller
  init-old/              old GS init scripts replaced by gs_supervisor

FOLLOWUPS.md             deferred polish/nits across both daemons
Makefile                 top-level umbrella (make all / test / clean)
```

## Build & test

```bash
# Default: host-build everything (no cross toolchain needed)
make

# Cross-build the vehicle binary (toolchain symlink at ./toolchain)
make vehicle

# Cross-build the GS supervisor for aarch64 ground stations
make -C ground cross CROSS_CC=aarch64-linux-gnu-gcc

# Run wire-format conformance tests (default)
make test

# Run the legacy Python controller-logic tests
make test-archive
```

### Mega binaries (single-file deployment, opt-in)

One multi-call binary per side instead of the separate
`wfb_rx`/`wfb_tx`/`tx_cmd`/`keygen` + daemon set — `wfb-gs` (ground) and
`wfb-air` (vehicle) dispatch applets by `argv[0]`/subcommand. The supervisor
still fork/execs, re-execing `/proc/self/exe <applet>`. Default builds are
unchanged (mega is `-DWFB_MULTICALL`, opt-in). Prep the patched wfb-ng tree +
cross libs once (`./wfb-ng/build-aarch64.sh` / `build-armv7.sh`), then:

```bash
make mega-ground     # wfb-gs (host x86; aarch64 via CC/CXX override)
make mega-vehicle    # wfb-air (cross armv7; finds cross libs under wfb-ng/build)
make mega            # both
```

Validated live on hardware (GS x86 + RTL88x2, vehicle Infinity6E). On the
vehicle, `init/S99wfb` auto-routes to `wfb-air` when it is on PATH, else falls
back to the standalone binaries. See @docs/design/mega-binary.md.

## Target platforms

| Daemon | Target | Toolchain |
|---|---|---|
| `vehicle/link_controller` | SigmaStar Infinity6E (armv7l) | `arm-openipc-linux-gnueabihf-gcc` (vendored at `./toolchain/`, symlinked from `../waybeam_venc/`) |
| `ground/gs_supervisor` | x86_64 host (default), or aarch64 ground stations | host `cc` or any `aarch64-*-gcc` |
| `wfb_tx` / `wfb_rx` (patched fork) | armv7l + aarch64 + OpenWRT MIPS | scripts under `wfb-ng/` |

## FEC gating design

Asymmetric gating — fast increase, slow decrease (mirror of TCP AIMD):
- **Sizing**: k from EWMA × bounded headroom (1.05–1.40). I-frames
  exceeding k×MTU span multiple blocks; the peek per-frame FEC close
  (M-bit) closes each frame's final block via the gate mode. Close
  cost/mode: see the peek note in the Architecture section above and
  `docs/design/peek-proportional-parity.md`.
- **Increase**: hysteresis=1, cooldown=0.1 s
- **Decrease**: hysteresis=3, cooldown=2.0 s
- **Oscillation detector**: >4 updates in 5 s → decrease cooldown × 3
- Under-protection (lost frames) is far worse than over-protection
  (wasted bandwidth); airtime efficiency on P-frames beats
  "one frame = one block" guarantee.

## WCMD redundancy

Every WCMD from `gs_supervisor /api/v1/cmd` goes out as 3 redundant
copies sharing the same seq.  The vehicle's `link_controller` keeps a
500 ms per-key dedup window, so a single FEC-block loss on the uplink
no longer drops the command but duplicates don't double-apply.  The
`coalesced_burst` counter in `/cmd/status` reports redundancy hits.

## Rules

- Wire protocol must match `shared/wcmd_proto.h` and
  `shared/rtp_sidecar.h` (network byte order, exact struct sizes).
  Any change there → run `make test` and bump the vendored copies in
  `tests/protocols/_proto/` to match.
- The vehicle's `link_controller` is the single source of truth for
  vehicle-side adaptive logic.  Don't deploy `archive/python/` modules
  alongside it (both would fight over the wfb_tx control socket and
  venc HTTP API).
- The ground's `gs_supervisor` is the single supervisor for `wfb_rx`
  and `wfb_tx` on the GS host; manual `wfb-ng.sh` / `wbmode` was
  retired (kept under `archive/init-old/` for reference).
- Loss-rate feedback and dual-stream FEC are out of scope.
