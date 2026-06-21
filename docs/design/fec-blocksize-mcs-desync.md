# FEC block-size ↔ MCS/bitrate desync (FPS oscillation root-cause + fix)

Status: **diagnosis + proposed architecture.** The per-frame *desync probe*
(`fec.desync_probe`, shipped in this branch) is the confirmation instrument;
the architecture below is the proposed fix once the probe confirms the cause.

Related: PR #84 (MCS↔bitrate transition grace), PR #85 (Adaptive-n), PR #86
(backpressure wedge watchdog — parked the MCS0 frame-drop chase as
"downstream/decoder"), `docs/design/adaptive-n-rs-peek.md`,
`docs/design/peek-proportional-parity.md`.

---

## 1. Symptom

Decoder-side FPS oscillation, most visible after the link drops to a low MCS
rung (worst case MCS5→MCS0). PR #86's bench saw a *clean* forced 5→0 drop
(ring 0% fill, ground rx clean) and concluded the link/FEC path was clean and
the problem was downstream. That bench never exercised the loss-coupled path —
which is exactly the condition that triggers a real MCS drop in the field.

## 2. Root cause: three signals, three latencies, coupled by *freeze* not *sync*

The controller tracks frame/link state through three signals on three
different latencies:

| Signal | Source | Latency on an MCS-down |
|---|---|---|
| **MCS rung** (link capacity) | controller decision | step, at the `bitrate_lead_s` deadline |
| **Commanded venc bitrate** (setpoint we wrote) | `bitrate_assert` HTTP write | step, **known exactly, instantly** |
| **Measured frame size** (sidecar `frame_size_bytes`) | encoder output | **lags** (rate-control converges ~hundreds of ms) + noisy (I vs P) |

FEC block size `k` is derived **only** from the measured frame size, via a slow
EWMA (`ewma_alpha=0.05`) plus a 2.5 s headroom ring
(`headroom_window_s`, `compute_headroom` `link_controller.c:754`). The
MCS/bitrate loop and the FEC loop are coupled **only by grace windows that
freeze the FEC loop**:

```c
// controller_update(), link_controller.c:998
if (in_grace) return false;   // k/n NOT recomputed for mcs_settle_s (5 s)
```

Freezing is the wrong coupling. On a 5→0 drop:

1. `T=0` `commit_mcs_pre_drop_bitrate` writes the low bitrate, arms the SET_RADIO
   deadline at `T+bitrate_lead_s` (0.5 s) and the up-lock for `lead+settle` (5.5 s).
2. `T+0.5 s` SET_RADIO mcs=0 fires → `radio_apply_observation` →
   `controller_arm_settle(mcs_settle_s=5.0)` → `bitrate_grace_until_us = T+5.5 s`.
3. `T+0.5 … 5.5 s` **`in_grace` → `controller_update` returns false → k/n stay
   pinned at the pre-drop MCS5 values** (k ≈ 14–24) while the frames flowing are
   now MCS0-sized (~1.7 KB ≈ 1–2 packets).

So `k` desyncs from frame size for ~5 s minimum, longer because:

- **Headroom ring lag**: `max/avg` over 2.5 s holds the big pre-drop I-frames →
  `headroom` railed at `headroom_max=1.40` even after the EWMA settles.
- **`k_down_dwell_s=8.0`**: shrinking k is gated by an 8 s dwell
  (`link_controller.c:1013`). A `settle_just_ended` one-shot (`:1000`) *can*
  snap k down at `T+5.5 s` if the ring/EWMA have settled — otherwise k stays
  large up to ~13 s.
- **Flap re-arms**: `controller_arm_settle` is extend-only (`:851`). If MCS
  hunts the MCS0 boundary, every change pushes grace out → **k never recommits
  → permanent large-block coalescing → sustained oscillation.**

## 3. The amplifier: the peek frame-boundary close gate

The peek per-frame FEC close (`wfb-ng/peek.patch:268`) only closes a frame's
block at its M-bit if the block is at least half full:

```c
if (k <= 0 || (int)t->block_fill() * 2 >= k) { /* close + emit parity */ }
```

With `k` pinned at 24 and frames at ~1.3 packets, the gate needs
`block_fill ≥ 12` → **~9 tiny frames coalesce into one FEC block** before it
closes. Under any RF loss, the rx cannot recover/release that block's frames
until enough fragments arrive, so 9 frames flush to the decoder together:
**burst → gap → burst = FPS oscillation.** On a clean link the data fragments
forward immediately, which is why PR #86's clean bench saw nothing.

The gate itself is correct — it exists to stop a P-frame-heavy 60/120 fps stream
from putting `~n` parity packets on air *per frame* (overflowing the TX ring).
The bug is that `k` is allowed to drift far from packets-per-frame, which turns
the gate from "rare fallback" into "steady state."

Worst case the user posed — **24/36 → 2/4**: a 12× block-size desync held for
5–13 s (∞ under flap). Both desync directions exist:

- **k stale-HIGH** (the bug above): coalescing → chunked release.
- **k stale-LOW** (if k were dropped *ahead* of the encoder spool-down): a still-
  large frame spans many tiny blocks → parity explosion + cross-block frame
  fragmentation.

The synchronization anchor that avoids *both* is the encoder-convergence point —
which is exactly what `bitrate_lead_s` already marks.

## 4. Why a bolt-on fix is insufficient

- *Shorten `mcs_settle_s`* — still measurement-lagged, still freezes, just
  briefly; flap still strands k.
- *Let k resize down during grace from measurement* — better, but still rides
  the EWMA + 2.5 s headroom-ring lag; the down-snap is gradual, still partly
  desynced.
- *Make the peek gate always close* — reintroduces the packet explosion the gate
  was built to prevent. Treats the symptom.

## 5. Proposed architecture: feed-forward operating point + bounded trim

**Principle: size the FEC block from the *committed link setpoint* (synchronous,
lag-free), and use the measured frame size only as a bounded correction.**

### 5.1 A single committed `LinkOperatingPoint`

The MCS/bitrate loop already decides `{mcs, bitrate, payload}` atomically. Make
that the one source of truth and derive sizing from it:

```c
typedef struct {
    int      mcs;
    long     bitrate_kbps;   /* what we commanded venc to produce */
    float    fps;            /* current FPS estimate */
    int      payload;        /* MTU/payload in force */
    uint64_t commit_us;
} LinkOp;
```

On every setpoint commit (MCS up/down/floor, bitrate, operator override),
compute the feed-forward target:

```
expected_bytes_per_frame = bitrate_kbps * 1000 / 8 / fps
ppf_ff   = ceil(expected_bytes_per_frame * headroom / mtu)
k_target = clamp(ppf_ff, min_k, max_k)
```

This is **available the instant we command the bitrate, with zero lag** — it is
exactly what the desync probe already computes as `ppf_ff`.

### 5.2 k = feed-forward step + measurement trim

- **On a setpoint change**, `k` snaps toward `k_target` as an explicit step of
  the ordered transition (see 5.3) — not frozen, not EWMA-lagged.
- **Between setpoint changes**, the measured-frame EWMA trims `k` within a small
  band (±1 ppf) under the existing dwell/hysteresis, to absorb encoder
  over/undershoot and scene complexity.

The EWMA stops driving the *step* changes (its lag no longer sits on the
critical path); it only fine-tunes. `n` continues to come from the static
redundancy curve + Adaptive-n on top, unchanged.

### 5.3 Sequence the FEC resize *inside* the ordered MCS transition

The ordered MCS-down state machine (`commit_mcs_change` `:2779`) already has the
right anchor. Add FEC sizing as an explicit step, timed to encoder convergence:

```
MCS-DOWN:
  T=0            pre-drop venc bitrate (already done)
  T=bitrate_lead deadline: SET_RADIO mcs↓  AND  commit k→k_target(new bitrate)
                 ← both land together, after venc output has converged
MCS-UP:
  T=0            SET_RADIO mcs↑ ; hold bitrate for mcs_up_grace_s
  T=mcs_up_grace commit k→k_target(new bitrate)  AND  raise bitrate
```

`k` lands in lockstep with the rung, when frames have actually reached the new
size — neither leading (5-low/fragmentation) nor lagging (5-high/coalescing).

### 5.4 Direction-aware grace

Grace exists to damp *oscillation*, i.e. measurement-driven re-sizing on noise.
A *commanded* resize during a downramp is precisely what we want. So:

- the **feed-forward step** passes through grace (it *is* the transition);
- the **measurement trim** stays gated by grace/dwell/hysteresis.

This mirrors the project's AIMD philosophy (fast-down/slow-up) but applies it to
the FEC loop's *trim*, not to the setpoint step.

### 5.5 Peek gate becomes self-consistent

With `k ≈ ppf` at all times, `block_fill*2 >= k` passes at every frame boundary —
per-frame isolation holds across transitions, and the coalescing path reverts to
the rare fallback it was designed to be. No change to `peek.patch` is required;
if desired, the gate could additionally be made frame-aware (close at the M-bit
when the block already holds ≥1 whole frame and an airtime budget allows), but
that is redundant once k tracks the setpoint.

## 6. Migration / safety

- Feature-flag (`fec.blocksize_feedforward`, default off → identical to today).
- Regression-lock: clean-link, steady-state `k`/`n` byte-identical to the static
  path (unit test, same pattern as `tests/test_fec_loss_adapt.c`).
- Fallback to the measurement path when the feed-forward inputs are unavailable
  (no committed bitrate yet at startup, or `fps` not yet estimated).
- The bench harness (`tests/bench_fec_loss_adapt.c`) gains an MCS-step timeline
  to score frames-per-block across a 5→0 drop with loss.

## 7. Confirming the cause first — the desync probe (this branch)

`fec.desync_probe` (off by default) emits one `FEC DESYNC` line per frame while
in grace or whenever desync is predicted/observed, ~1 Hz otherwise:

```
FEC DESYNC fsz=<bytes> ppf_meas=<pkts this frame> k=<committed> n=<committed>
           ppf_ff=<pkts implied by commanded bitrate> fpb=<frames per FEC block>
           coalesce=<0|1> grace=<0|1> gms=<grace ms left> mcs=<rung>
           br=<commanded kbps> fps=<fps>
```

- `fpb` (frames-per-block) replays the peek gate deterministically on the
  controller — **no TX feedback needed**. `fpb > 1` ⇒ coalescing is happening.
- The hypothesis is **confirmed** if, on an MCS-down, you see `grace=1` with
  `k` ≫ `ppf_meas` (and ≫ `ppf_ff`) and `fpb` jumping to ~5–10 for the duration
  of the grace window, recovering to `fpb=1` only once `k` recommits.
- It is **denied** if `k` tracks `ppf_meas` throughout (`fpb≈1`) yet FPS still
  oscillates — pointing back at the decoder / 120 fps regime per PR #86.

Enable live: `GET /set?fec.desync_probe=1` (or config). The lines are captured by
the SD telemetry logger and can be charted against the decoder FPS trace.
