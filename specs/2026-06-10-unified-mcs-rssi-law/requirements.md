# Unified MCS law — PER probe primary + RSSI guard-rails

Status: IMPLEMENTED 2026-06-10 (follows directly from the Phase 4 boundary-probe
bring-up; legacy RSSI-bucket FSM removed in the same change)

## 1. Context

The boundary-probe PER law (`specs/2026-06-10-boundary-probe-phase4/`) was
device-verified through the full §8 checklist (steady-state soak, retune
ordering, CSA-hop survival). It was still opt-in (`mcs.mode=1`), with the
legacy RSSI-bucket FSM as the compiled default — every link_controller restart
silently fell back to the legacy law.

Maintainer direction: make the probe law the only law, and rather than deleting
the RSSI signal, **integrate it into the probe law as guard-rails**. This
subsumes the previously deferred "Phase 3 RSSI fade-rate augment".

## 2. The law

Evaluated once per video rx_ant datagram in `selector_update()`
(vehicle/link_controller.c), first match wins. All demotes step
`current_mcs − 1`, respect `down_cooldown_s`, floor at `mcs_min`.

| # | Rule | Trigger | Why |
|---|---|---|---|
| 1 | Reactive demote | smoothed video PER ≥ `demote_per_milli` (30‰) | real damage signal, fastest |
| 2 | RSSI floor demote | smoothed RSSI ≤ `rssi_floor_dbm` (−85) | deep-fade backstop; works when the probe stream died with the fade |
| 3 | RSSI fade demote | slope ≤ −`rssi_fade_db_per_s` (10) AND RSSI ≤ `rssi_fade_arm_dbm` (−65) | pre-emptive on fast collapse, before PER damage; fades at strong signal ignored |
| 4 | Probe V+2 | fresh + post-gate rung[V+2]: PER ≥ `probe_fail_milli` (200‰) → demote; PER ≤ `probe_clean_milli` (20‰) → promote +1 after `promote_dwell_s` | the Phase 4 law, unchanged |
| 5 | Hold | — | stale probe / mid-band PER |

- **Promote guard**: promotes are blocked while rule 3 is active, or while
  smoothed RSSI ≤ `rssi_floor_dbm` + 3 dB (hysteresis so promotes don't resume
  on the knife-edge).
- **RSSI slope**: per-sample derivative of the smoothed RSSI series, EWMA'd
  with fixed α=0.3 (~0.3 s response at the 10 Hz rx_ant rate; no extra knob).
  dt clamped to [10 ms, 2 s]; longer stats gaps decay the slope instead of
  producing a bogus spike.
- **Failsafe** (watchdog, unchanged): no rx_ant for `failsafe_timeout_s` →
  commit `mcs_min`; clears on the next datagram.

## 3. Probe is mandatory

`mcs.enabled && probe_port == 0` is a **startup error** — without the probe the
law can never promote and would pin video at `mcs_start`. `--no-mcs` gives
FEC-only operation. S99wfb maps `wfbprobe=0` → `--no-mcs` (and drops
`--csa-iface` with it, since the CSA receiver rides the MCS rx_ant listener).

## 4. Removed (legacy bucket FSM)

Config fields + tunables + CLI flags, all uses: `rssi_thresh_low/high`,
`rssi_deadband_db`, `loss_*_penalty_*` (and `effective_rssi`), `up/down_consecutive`,
`up_cooldown_s`, `failsafe_recovery_consecutive`, `oscillation_threshold/backoff`,
`mcs_bucket_0/1/2`, `start_low`, `mcs.mode`, `--range`, `--recover-consec` et al.
Kept: Scorer EWMA machinery (`rssi_ewma_alpha`, `loss_ewma_alpha`,
`rssi_aggregator`), `down_cooldown_s`, `failsafe_timeout_s`,
`oscillation_window_s` (stats-only `changes_in_window`), `mcs_min/max/start`,
all `probe_*` knobs.

Behavioral side effects of the unification:

- The **realign block** (external party touched wfb_tx) now compares
  `radio_body.mcs_index` against `sel.current_mcs` directly — it was dead in
  probe mode before (gated on `current_bucket >= 0`).
- `/status` `mcs` object: bucket/pending/recovery fields gone; new
  `rssi_guard:{floor_active,fade_active}`. `score` object: `effective_rssi`/
  `loss_penalty_db` replaced by `raw_rssi`(kept)/`rssi_slope_db_s`.
- Failsafe recovery is immediate on first datagram (probe-mode semantics);
  the legacy N-good-samples unfreeze is gone with the bucket law.

## 5. New tunables

| Key | Default | CLI |
|---|---|---|
| `mcs.rssi_floor_dbm` | −85.0 | `--rssi-floor` |
| `mcs.rssi_fade_db_per_s` | 10.0 (0 = fade rule off) | `--rssi-fade` |
| `mcs.rssi_fade_arm_dbm` | −65.0 | `--rssi-fade-arm` |

Validation warning when `rssi_fade_arm_dbm <= rssi_floor_dbm` (fade rule could
never fire before the floor rule).

Defaults are bench-safe (bench RSSI ≈ −24 dBm: neither rule arms) and chosen
from typical wfb-ng link budgets; **field-tune `rssi_floor_dbm` against the
real MCS0 sensitivity point** during range testing.

## 6. Verification

- Host build `-Wall -Wextra` clean; wire-format tests unaffected.
- Host E2E (fake wfb_tx ctrl + synthetic rx_ant/probe feeds): cold climb,
  reactive demote + §5.1 gate, fade-ramp demote + promote-block + recovery,
  floor demote to `mcs_min`, probe-mandatory startup error, `--no-mcs` FEC-only.
- Device (§ same rig as Phase 4): cold S99wfb start with NO manual args —
  law active by default, climbs to `mcs_max`, steady; `wfbprobe=0` restart →
  FEC-only boot.

## 7. Deferred

- Field calibration of floor/fade defaults at real range (bench cannot fade).
- Induced-fade dynamic test (operator at bench) — inherited from Phase 4 §8.
- `SD_RECOVERED` decision code is now unreachable (legacy unfreeze emitted it);
  kept in the enum for log-string stability, prune on next touch.
