# waybeam\_wfb\_ng

Zero-copy video streaming infrastructure and adaptive FEC control for
WiFi broadcast ([wfb-ng](https://github.com/svpcom/wfb-ng)) on
SigmaStar Infinity6E SoCs.

## Architecture

```
 waybeam_venc          /dev/shm/venc_wfb          wfb_tx (patched)
 ┌──────────┐         ┌─────────────────┐        ┌──────────────┐
 │ H.265    ├────────►│ Lock-free SPSC  ├───────►│ FEC encode + │──► wlan0
 │ encoder  │         │ ring (512 slots)│        │ WiFi inject  │
 │ + RTP    │         └─────────────────┘        └──────┬───────┘
 └────┬─────┘                                           │
      │ sidecar (UDP)                          UDP control port
      ▼                                                 ▲
 ┌────────────────────────────────────────────┐         │
 │ fec_controller                             │         │
 │  SUBSCRIBE ──► venc sidecar (port 6666)    │         │
 │  FRAME     ◄── per-frame metadata (52/64B) │         │
 │  FPSEstimator + HeadroomTracker            │         │
 │  FECController (k/n computation)           ├─────────┘
 │  WfbTxControl  ──► "set_fec k n"           │
 └────────────────────────────────────────────┘
```

**Video path** uses shared memory (zero kernel copies).
**Control path** uses UDP — no custom wfb\_tx patches needed for FEC updates.

## Components

### Shared memory ring (`poc/`)

Zero-copy RTP packet transfer from encoder to wfb\_tx via POSIX shared
memory, eliminating UDP sendmsg/recvmsg overhead on the video path.

- `SHM_HOWTO.md` — setup guide, ring parameters, troubleshooting
- `shm-input.patch` — wfb\_tx patch adding `-H` flag for SHM input
- `build_wfb_tx.sh` — cross-compilation build script for Infinity6E

### RTP timing sidecar (`poc/`)

Out-of-band UDP channel for per-frame timing diagnostics.

- `SIDECAR_INFO.md` — wire protocol, message types, overhead
- `rtp_timing_probe.c` — host-native reference probe for frame correlation

### Adaptive FEC controller (`fec_controller/`)

Dynamically adjusts wfb-ng FEC k/n based on real-time video frame
statistics received via the sidecar protocol.

#### How it works

1. Subscribes to the venc sidecar (sends `SUBSCRIBE` every 2s)
2. Receives `FRAME` messages (52B base / 64B with encoder trailer)
3. Derives fps from `frame_ready_us` intervals (EWMA)
4. Tracks frame size variance via `HeadroomTracker` (2.5s rolling window)
5. Computes optimal k from `avg_frame_size * headroom / MTU`
6. Interpolates redundancy from curve: 50% at k=1 down to 25% at k=48
7. Sends `set_fec k n` to wfb\_tx via UDP control port
8. Gates updates with hysteresis (k must change by >= 2) and rate limiting (0.5s min)

#### Key design decisions

- **One frame ~ one FEC block**: k is sized so a full frame fits in one
  block, avoiding latency from partial last blocks stalling on the next
  frame's packets.
- **Headroom is learned**: The max/avg frame size ratio adapts within
  2.5s, clamped to [1.05, 1.40] with a 1.05x safety margin.
- **Small frames get more redundancy**: At k=1, 50% redundancy is cheap
  in absolute bytes. At k=48, 25% is sufficient because Reed-Solomon has
  more symbols to work with.
- **fec\_timeout at half frame period**: Flushes underflowed blocks
  mid-gap between frames rather than colliding with the next burst.

#### Reference table

```
MTU=1446, headroom=1.15

FrameSize    k    n   n-k  Redundancy  Efficiency
      500    1    2     1     50%         50%
     1446    2    4     2     47%         50%
     3000    3    6     3     43%         50%
     5000    4    7     3     40%         57%
     8000    7   11     4     35%         64%
    12000   10   15     5     32%         67%
    20000   16   23     7     30%         70%
    30000   24   34    10     29%         71%
    44000   35   48    13     27%         73%
    60000   48   64    16     25%         75%
```

#### Module structure

```
fec_controller/
  __init__.py        Public API exports
  __main__.py        python -m fec_controller
  protocol.py        Sidecar wire protocol (matches rtp_sidecar.h)
  config.py          ControllerConfig dataclass (all tunables)
  headroom.py        HeadroomTracker (learned I/P variance)
  controller.py      FECController core (EWMA, k/n, gating)
  wfb_control.py     WfbTxControl (UDP "set_fec k n" sender)
  service.py         Async UDP service + FPSEstimator
  cli.py             CLI entry point (run / simulate / table)
  simulation.py      Synthetic stream generator (uses real protocol)
```

#### Usage

```bash
# Run with live sidecar
python -m fec_controller run \
  --wfb-port 8003 \
  --sidecar-host 10.0.0.1 \
  --sidecar-port 6666 \
  --stat-port 5610

# Dry run (log updates, don't send to wfb_tx)
python -m fec_controller run --wfb-port 8003 --dry-run

# Run simulation (builds real sidecar FRAME packets)
python -m fec_controller simulate --fps 120 --base-frame-size 5000

# Print reference table
python -m fec_controller table
```

#### Tests

```bash
python -m pytest tests/ -v
```

85 tests covering wire format roundtrips, byte order verification, fps
estimation, headroom learning, redundancy interpolation, update gating,
wfb\_tx control format, and async UDP integration. All tests use the
real sidecar FRAME protocol — no mock formats.

## What's done

- [x] SHM ring zero-copy video path (venc -> wfb\_tx)
- [x] wfb\_tx patch for `-H` shared memory input
- [x] Cross-compilation build script for Infinity6E
- [x] RTP timing sidecar protocol and reference probe
- [x] Adaptive FEC controller module
  - [x] Wire protocol matching `rtp_sidecar.h` (52B/64B FRAME, SUBSCRIBE)
  - [x] FPS estimation from `frame_ready_us` intervals
  - [x] Learned headroom from actual max/avg frame size ratio
  - [x] Redundancy curve interpolation (k-dependent)
  - [x] Update gating (hysteresis + rate limiting)
  - [x] Async service with sidecar subscription keepalive
  - [x] `set_fec k n` UDP control output to wfb\_tx
  - [x] Simulation using real protocol path
  - [x] 85 unit/integration tests

## Next steps

### Near-term

- **Integration testing on hardware** — Deploy to device and verify
  `CMD_SET_FEC` binary commands are accepted by wfb\_tx's control socket.

- **Packaging** — Add `pyproject.toml` for pip-installable package with
  console\_scripts entry point.

### Future enhancements

- **Packet aggregation on high-MTU links** — When the radio MTU allows
  larger packets, aggregate multiple small RTP packets per FEC symbol to
  reduce FEC overhead. Independent of the adaptive k/n logic.
