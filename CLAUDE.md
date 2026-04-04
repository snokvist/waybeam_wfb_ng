# waybeam_wfb_ng

## What this is

Adaptive FEC controller for wfb-ng WiFi broadcast on SigmaStar Infinity6E (armv7l / OpenIPC Linux). Zero-copy video streaming via POSIX shared memory with real-time FEC k/n adaptation.

## Architecture

- **Video path**: venc -> SHM ring (zero-copy) -> wfb_tx (patched `-H`) -> radio
- **Control path**: venc sidecar FRAME msgs (UDP) -> fec_controller -> `set_fec k n` -> wfb_tx control port
- **Design**: One frame ~ one FEC block. Learned headroom. Stateless (no loss feedback).

## Repository layout

```
fec_controller/       Production module (Python, async UDP)
  protocol.py         Sidecar wire protocol (matches rtp_sidecar.h)
  config.py           ControllerConfig (all tunables)
  headroom.py         HeadroomTracker (learned I/P variance)
  controller.py       FECController core (EWMA, k/n, gating)
  wfb_control.py      WfbTxControl (UDP set_fec sender)
  service.py          Async UDP service + FPSEstimator
  cli.py              CLI (run / simulate / table)
  simulation.py       Synthetic stream generator
poc/                  Proof-of-concept artifacts & build tools
  shm-input.patch     wfb_tx SHM input patch
  build_wfb_tx.sh     Cross-compilation script for Infinity6E
  rtp_timing_probe.c  Host-native reference probe
tests/                85 tests (real protocol, no mocks)
```

## Build & test

```bash
# Run tests
python -m pytest tests/ -v

# Run simulation
python -m fec_controller simulate --fps 120 --base-frame-size 5000

# Print reference table
python -m fec_controller table
```

## Target platform

- SigmaStar Infinity6E, armv7l, OpenIPC Linux
- Cross-compiler toolchain at `../toolchain/toolchain.sigmastar-infinity6e/`

## FEC gating design

Asymmetric gating — fast increase, slow decrease (mirror of TCP AIMD):
- **Increase**: hysteresis=1, cooldown=0.1s, peak window (32 frames, >= 1 GOP) as floor
- **Decrease**: hysteresis=3, cooldown=2.0s, EWMA-only (no peak)
- **Oscillation detector**: >4 updates in 5s → decrease cooldown multiplied by 3x
- Under-protection (lost frames) is far worse than over-protection (wasted bandwidth)

## Rules

- All tests use the real sidecar protocol — no mock formats
- Wire protocol must match `rtp_sidecar.h` (network byte order, exact struct sizes)
- fec_controller is the only sidecar consumer (no multiplexer needed)
- Loss-rate feedback and dual-stream FEC are out of scope
