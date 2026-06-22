# Design note: Adaptive-n RS+peek (loss-driven redundancy)

> **SUPERSEDED (2026-06-22, `refactor/fec-minimal-mcs-driven`).** Adaptive-n
> and the post-MCS-drop parity boost were **removed**. On device they churned
> n=7↔8 on a clean link (the rx_ant `recov` EWMA never sits exactly at zero),
> producing a steady-state FEC/bitrate oscillation. The FEC controller is now
> purely frame-size driven (k tracks the frame, `n = curve(k)`, emit only on
> real change) and the **MCS selector is the sole loss-response loop**. This
> note is retained for the rationale/derivation only — it does not describe
> shipping behavior. See the "FEC gating design" section in `CLAUDE.md`.

Status (historical): **IMPLEMENTED** (2026-06-18), `fec.loss_adapt` default **ON**
(curve is the floor → identical to the static path on a clean link).
Host-bench validated; pending on-hardware tuning. Branch:
`claude/swfec-wfb-ng-patches-v9xvvq`.

Landed in `vehicle/link_controller.c` (control law in `controller_update`,
config + settings rows + telemetry) with a host unit test at
`tests/test_fec_loss_adapt.c` (regression lock, attack, ceiling, decay,
staleness, airtime rail — run by `make -C vehicle test`). The control law
ended up *simpler* than the spec's §3 sketch: `last_lost_ratio` /
`last_recov_ratio` / `last_rx_ant_us` are already function-local in the main
loop (computed for the MCS path), so no `LossSnapshot` struct was needed —
they pass straight into `controller_update`. Gains/ceiling below are
bench-tuning starting points, not final.

This note came out of an swfec (sliding-window RLNC FEC) evaluation. The
conclusion there was that swfec would *replace* RS+peek rather than extend
it, would delete (not simplify) the adaptive loop, and only wins at latency
budgets this low-latency RF link doesn't have (see the head-to-head below in
"Why not swfec"). The evaluation surfaced a concrete, low-risk gap in the
*current* RS loop that is worth fixing on its own: **redundancy `n` does not
respond to measured channel loss.** This note specifies closing that gap.

---

## 1. Current behaviour (the gap)

`fec_compute()` (`vehicle/link_controller.c`) sets protection in three steps:

1. **`k` tracks frame geometry** — `k = ceil(avg_frame_size × headroom / MTU)`.
   `headroom` is the rolling max/avg ratio (1.05–1.40) from `compute_headroom()`.
   This adapts to the *encoder*, not the *radio*.
2. **`r = interpolate_redundancy(k)`** — the redundancy fraction is a **static
   lookup curve keyed only on `k`** (`REDUNDANCY_CURVE`). Then
   `n = ceil(k / (1 − r))`, clamped to `[fec.min_n, fec.max_n]`.
3. **Transient boost** — the only channel-reactive term is the open-loop,
   time-limited parity bump armed on an **MCS-down event**
   (`controller_arm_boost`, `fec.boost_s`, `fec.boost_mult`).

So in steady state **redundancy is a function of block size plus a brief
MCS-correlated bump. It never responds to actual packet loss.** Two failure
modes follow:

- **Under-protection without an MCS event.** Interference, multipath, a fade,
  antenna misalignment, a co-channel burst — none necessarily trip an
  MCS-down. The link drops packets, `r` stays where the curve put it, blocks
  fail (loss > n−k), and peek drops the frame cleanly… but it *drops*. FEC is
  blind to the one thing it exists to fight.
- **Over-protection when clean.** On a solid link the curve-mandated parity is
  airtime tax that can't ratchet below the curve, forcing MCS/bitrate lower
  than necessary.

Crucially, **the loss signal already exists on the vehicle.** The MCS loop
already ingests rx_ant `-Y` stats (the GS `wfb_rx` push: `lost`,
`fec_recovered`, `all`, …) and maintains EWMAs in the per-link scoring path:

- `smoothed_lost_ratio`  — residual (post-FEC) loss ratio, EWMA
  (`cfg->mcs.loss_ewma_alpha`).
- `smoothed_recov_ratio` — FEC-recovered fraction, EWMA.
- `per_milli`            — per-mille PER used by the MCS demote/probe rules.

The historical "loss-rate feedback is out of scope" rule (CLAUDE.md) kept this
signal from driving **FEC**; MCS already uses it. Adaptive-n therefore adds a
**second consumer of an existing, already-smoothed signal** — no new wire
format, no new EWMA, no new uplink. That is what makes this low-risk.

---

## 2. Goal / non-goals

**Goal.** Make the RS redundancy fraction respond to measured residual loss,
preserving every property of the current delivery path:

- `k` still tracks frame size (unchanged).
- peek still closes each frame's block on the RTP M-bit (unchanged).
- Frames still deliver/drop cleanly on frame boundaries, in sequence.
- No codec change, no wire-format change, no measurable CPU change.

**Non-goals.**

- Not swfec; not sliding-window FEC; not dual-stream FEC.
- No new feedback wire (reuse the rx_ant path the MCS loop already consumes).
- Not a replacement for MCS demotion — parity and MCS stay distinct responses
  (§5 defines how they coordinate).

---

## 3. Signal

Reuse the MCS scoring EWMAs. Two terms, different roles:

| Term | Source | Role |
|---|---|---|
| `loss_ewma` = `smoothed_lost_ratio` | residual post-FEC loss | **hard** term — frames are *already failing*; react strongly |
| `recov_ewma` = `smoothed_recov_ratio` | FEC-recovered fraction | **soft** / leading-indicator term — FEC working but stressed; react gently before residual loss appears |

Using residual (post-FEC) loss as the hard term is deliberate: it avoids
double-counting drops the current FEC already recovered. `recov_ewma` is the
early-warning term — rising recovery means the block is near its correction
limit before any frame is actually lost.

**Plumbing.** These EWMAs are updated in the rx_ant handler; the FEC update
fires on sidecar frame events. They are different events on the same poll loop,
so publish the latest values into a small shared snapshot the FEC path reads:

```c
struct LossSnapshot {
    float    loss_ewma;     /* smoothed_lost_ratio  */
    float    recov_ewma;    /* smoothed_recov_ratio */
    uint64_t updated_us;    /* for staleness gating */
};
```

Single-threaded controller → a plain struct updated in the rx_ant branch and
read in the sidecar branch. **Staleness gate:** if `now - updated_us >
loss_stale_s` (no recent rx_ant), treat both terms as 0 (fail toward the
static curve, never toward phantom protection on a dead feedback path).

---

## 4. Control law

Replace the static `r = interpolate_redundancy(k)` with a loss-biased
effective redundancy, computed inside `fec_compute()` (or just before it in
`controller_update()` so the controller owns the timing/hysteresis):

```
r_base = interpolate_redundancy(k)            // current curve = the FLOOR
r_loss = g_lost  * loss_ewma  + g_recov * recov_ewma
r_eff  = clamp(r_base + r_loss, r_base, r_ceiling)
n      = ceil(k / (1 - r_eff))                // then clamp to [min_n, max_n]
```

**AIMD asymmetry** (mirrors the existing FEC philosophy — under-protection ≫
over-protection):

- **Attack (parity up): fast.** When `r_loss` rises, apply immediately — no
  cooldown. Losing frames now is the expensive failure.
- **Decay (parity down): slow.** When `r_loss` falls, bleed `r_eff` back toward
  `r_base` on a decay timer (`loss_decay_s`, default ~2 s, matching the FEC
  decrease cooldown). Prevents flapping on noisy loss samples.

Implement decay as a one-pole toward `r_base` on the loss component only, so
`r_base(k)` movements (frame-size driven) still take effect immediately while
the *loss bias* is what's slow to release.

**Reuse the existing oscillation detector.** The FEC loop already counts
updates per window and lengthens the decrease cooldown on thrash (>4 updates
in 5 s → ×3). Route parity-driven `n` changes through the same accounting so a
flapping link can't oscillate parity.

The post-MCS-drop **boost is subsumed**: it becomes a fast-start *seed* for
`r_loss` (or is removed). Today it guesses "MCS dropped, assume loss for
`boost_s`, bump by `boost_mult`." With Adaptive-n we bump by what we are
*actually* losing and hold it exactly as long as the loss persists.

---

## 5. Coordination with MCS (the one real interaction)

Both loops now consume the same loss signal, so define priority explicitly to
stop them fighting over the same packets:

1. **Parity is the fast, cheap, local first responder.** It reacts sub-second
   on the vehicle without changing modulation. Let it absorb transient and
   moderate loss.
2. **MCS demote remains the structural response** to sustained loss / RSSI
   floor / fade, exactly as today.
3. **Do not let parity decay during MCS settle/grace.** While the MCS settle
   window or `bitrate_up_lock` is active (post-MCS change), freeze the *decay*
   of `r_eff` (attack still allowed). The link is mid-transition; releasing
   parity into an unsettled modulation is exactly when you'd drop frames.
4. **Airtime guard.** Higher `n` costs airtime; at high MCS a demote can be
   cheaper than piling on parity. Cap `r_ceiling` and add a hard airtime rail
   (see §7) so parity can't crowd out the headroom the MCS loop needs to
   operate.

Net ordering: *loss appears → parity rises immediately → if loss is sustained,
MCS demotes → after the demote settles, parity decays back toward the curve at
the new, more robust modulation.*

---

## 6. Redundancy granularity (RS's one weak axis)

RS parity is integer `n−k` on small blocks: on a 3-packet P-frame the smallest
step is n−k 1→2, i.e. 33%→50% redundancy — you cannot express 40%. This is the
single axis where swfec's continuous overhead knob beats RS, and it bites
hardest on tiny high-fps P-frames.

**Mitigation (fits the existing design, optional, separate flag):** when fps is
high and frames are tiny, let an RS block span `N` frames — peek still closes
the block on a *chosen* frame's M-bit (every Nth frame boundary), so delivery
stays frame-aligned but `k` is large enough that the loss term has fine-grained
parity to work with. This is a follow-on, not required for v1; v1 ships
per-frame blocks and accepts coarse granularity on tiny frames.

---

## 7. Config (all under `cfg->fec`, opt-in, default off)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `fec.loss_adapt` | bool | `false` | master enable; off = today's behaviour exactly |
| `fec.loss_adapt_gain` | float | TBD (≈3.0) | `g_lost` — residual-loss → redundancy gain |
| `fec.loss_adapt_recov_gain` | float | TBD (≈0.5) | `g_recov` — recovered-fraction (early-warning) gain |
| `fec.loss_adapt_ceiling` | float | 0.60 | `r_ceiling` — hard cap on effective redundancy |
| `fec.loss_decay_s` | float | 2.0 | slow-release time constant for the loss bias |
| `fec.loss_stale_s` | float | 2.0 | no rx_ant within this → loss terms forced to 0 |
| `fec.airtime_max_pps` | int | reuse payload table cap (~1100) | hard rail: refuse `n` that pushes `n×fps` over budget |

Gains start conservative and are tuned on the bench (§9). All reuse the
existing `OFF_FEC(...)` settings-table machinery (`§ fec.*` rows) so they're
live-tunable over the WCMD/settings path like every other knob.

**Hard rails (independent of gains):** `[fec.min_n, fec.max_n]` clamps stay;
`r_ceiling` caps redundancy; `airtime_max_pps` caps on-air packet rate so a
loss spike can never drive `n` into airtime collapse. Disabled in `--dry-run`
(no writes), same as the rest of FEC.

---

## 8. Telemetry

Surface the decomposition so the WebUI/logs show *why* `n` moved:

- `LOG_FEC` update line: add `r_base`, `r_loss`, `r_eff`, `loss_ewma`,
  `recov_ewma` alongside the existing `k=… n=… hd=… red=…`.
- `/api/v1` FEC JSON (`append_fec_json`): add `loss_adapt` (bool),
  `r_base`, `r_eff`, `loss_ewma`, `recov_ewma`, and `n_from_loss`
  (= `n - n_at_r_base`) so an operator can see parity attributable to the
  channel vs. the curve.

This makes the A/B in §9 measurable without a packet capture.

---

## 9. Testing & validation

- **Wire format: unchanged.** No `shared/*.h` edit, no SESSION/contract bump,
  no new command. So **no `make test` protocol-vector churn and no
  `tests/protocols/_proto/` bump** — a deliberate consequence of reusing the
  existing rx_ant signal. (`make test` still runs as the regression gate.)
- **Host controller-logic test** (new, if a host-testable seam exists): feed a
  synthetic loss EWMA series + frame-size series into `controller_update`/
  `fec_compute` and assert: (a) `loss_ewma=0` reproduces today's `n` exactly
  (regression lock on the static path); (b) rising loss raises `n` monotonically
  up to `r_ceiling`; (c) attack is immediate, decay respects `loss_decay_s`;
  (d) staleness forces terms to 0.
- **Bench A/B** (192.168.1.x rig + dev-host `wfb_rx_native`):
  - Baseline = `fec.loss_adapt=false` (static curve).
  - Inject controlled loss (attenuator sweep / interferer / `tc netem` on the
    GS RX path). Compare **delivered-frame ratio**, **mean+p99 frame latency**,
    and **airtime (pps / on-air bytes)** between static and adaptive.
  - Confirm the §5 ordering on a real fade: parity rises first, MCS demotes on
    sustained loss, parity decays after settle.
  - Watch for oscillation under bursty loss (oscillation detector engaged).
- **Regression guard:** with `loss_adapt=false`, byte-for-byte identical `n`
  decisions vs. current main on a recorded sidecar+rx_ant trace.

---

## 9a. Host-bench results (2026-06-18)

`tests/bench_fec_loss_adapt.c` (`make -C vehicle bench`) drives the real
`controller_update()` through scripted FPV timelines on a Gilbert-Elliott burst
channel with delayed closed-loop feedback (6-frame ≈ 100 ms RTT). Same
channel + frame sizes fed to both modes; airtime rail off to isolate the law.
Default gains (3.0 / 0.5 / ceiling 0.60 / decay 2.0 s).

| scenario | static deliv% | adaptive deliv% | drops | airtime |
|---|---|---|---|---|
| clean-cruise        | 100.00 | 100.00 | 0→0   | +1.1% |
| obstacle-flyby      | 98.87  | 99.01  | 8→7   | +7.1% |
| range-edge-fade     | 98.44  | 98.87  | 11→8  | +8.5% |
| sharp-turn-nulls    | 99.43  | 99.58  | 4→3   | +8.7% |
| **foliage-sustained** | 98.44 | **99.43** | **11→4** | +9.0% |
| aggregate           | 99.04  | 99.38  | +0.34 pp | +6.9% |

Takeaways:
- **Clean link is free** (delivery identical; the +1.1% is noise off the
  model's 0.02 stress floor — true 0% loss gives 0 overhead).
- **Biggest win is sustained loss** (foliage: drops −64%), which is precisely
  the design target — loss with no MCS-down event, the static curve's blind
  spot.
- The loop biases hardest on **small/fragile blocks**: in the trace, during the
  fade peak `k=26` (large block from the 8 s k-down dwell) absorbs bursts with
  no bias; once `k` shrinks to 9 the bias jumps to the ceiling (redundancy
  0.61 vs the curve's ~0.32). Block FEC is already decent on big blocks, so
  adaptive shaves the failure tail rather than transforming it.
- Tuning levers for hardware: `loss_adapt_recov_gain` buys earlier pre-emption
  (currently the hard residual term dominates and only fires *after* a block
  fails); `loss_decay_s` trades airtime-after-event vs. flap resistance.

## 10. Implementation checklist (action order)

1. ✅ Loss signal: reused the existing `last_lost_ratio` / `last_recov_ratio` /
   `last_rx_ant_us` main-loop locals (no `LossSnapshot` struct needed); the
   sidecar FEC branch computes `loss_fresh` from `loss_stale_s`.
2. ✅ `controller_update()`: `r_base → r_loss → r_eff → n` (only ever raises n)
   with fast-attack / slow-decay one-pole; loss-only n-moves now commit via the
   `k_delta == 0` branch (up immediately, down gated by `cooldown_up_s`).
3. ✅ MCS-down boost composes with loss (loss takes the max; boost is the
   fast-start floor). Full subsumption deferred — harmless overlap, revisit
   after bench.
4. ✅ §5 coordination: loss-decay frozen during MCS settle / `bitrate_up_lock`.
5. ✅ Config + settings rows (`fec.loss_adapt*`, `loss_decay_s`, `loss_stale_s`,
   `airtime_max_pps`) with table ranges. (Startup cross-field validation
   warnings not added — ranges enforced by the settings table; revisit if
   bench wants them.)
6. ✅ Telemetry: `LOG_FEC` `rloss=` field + `append_fec_json`
   (`loss_adapt`, `redundancy`, `r_loss_bias`, `loss_ewma`, `recov_ewma`).
7. ✅ Host unit test `tests/test_fec_loss_adapt.c` + `make test` green.
8. ⏳ **Bench A/B**: tune `loss_adapt_gain` / `recov_gain` / `ceiling` /
   `decay_s` / `airtime_max_pps`; record results here (promote Status
   IMPLEMENTED → VERIFIED, or document removal, peek-note style).

Deferred follow-ups (not blocking bench): wire loss-driven n-moves through the
oscillation detector (currently guarded only by `cooldown_up_s` on down-moves);
multi-frame block spanning for fine parity granularity on tiny high-fps frames
(§6); decide whether to fully delete the MCS-down boost once loss adaptation is
trusted.

Default stays **off** through the entire rollout; flip on per-rig for A/B.

---

## Why not swfec (for the record)

For a low-latency RF video link with a tiny jitter buffer, swfec's structural
wins (lowest clean-link latency, burst resilience per unit overhead, a
continuous adaptation knob) only materialise at a **multi-frame deadline**. Tighten
the deadline to control jitter and swfec's window shrinks toward one frame —
losing the burst-resilience edge, still needing fps to size the deadline, and
converging toward "peek with RLNC math" at higher GF(2⁸) CPU on the Infinity6E
encoder and the GS decoder. swfec also breaks frame-aligned delivery: recovery
latency is window-spread and bimodal, and drop boundaries smear across frames,
versus RS+peek's deterministic, frame-quantized, in-sequence deliver/drop.

Adaptive-n keeps RS+peek's deterministic delivery and attacks the only axis
swfec was clearly winning — coarse parity granularity — via §6, without a codec
swap. It is the higher-leverage, lower-risk change. swfec stays a future A/B
option *iff* the product can later tolerate a multi-frame deadline.
