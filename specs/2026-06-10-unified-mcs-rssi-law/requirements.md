# Unified MCS law — PER probe primary + RSSI guard-rails

Status: IMPLEMENTED 2026-06-10 (follows directly from the Phase 4 boundary-probe
bring-up; legacy RSSI-bucket FSM removed in the same change).
Hardening batch from the 2026-06-11 code review applied — see §8.

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
| 4 | Probe V+2 | fresh + post-gate rung[V+2]: PER ≥ `probe_fail_milli` (200‰) → demote; PER ≤ `probe_clean_milli` (20‰) → promote +1 after `promote_dwell_s`. **Fail branch is evaluated first** so an inverted config (clean ≥ fail, warned) can only demote/hold, never promote on a failing rung | the Phase 4 law, unchanged |
| 5 | Hold | — | stale probe / mid-band PER |

- **Promote guard**: promotes are blocked while rule 3 is active, or while
  smoothed RSSI ≤ `rssi_floor_dbm` + 3 dB (hysteresis so promotes don't resume
  on the knife-edge).
- **RSSI slope**: per-sample derivative of the smoothed RSSI series, EWMA'd
  with fixed α=0.3 (no extra knob). **Latency honesty**: this is two cascaded
  first-order filters (rssi_ewma + slope ewma, each τ≈0.28 s at 10 Hz) — a
  real −12 dB/s fade crosses the default −10 dB/s threshold only after
  ~0.9 s (~11 dB of drop). Rule 3 is a *backstop* that genuinely pre-empts
  only fast fades (≳20 dB/s); near-threshold fades are usually caught by
  rule 1 (loss EWMA α=0.5, ~0.2 s) or the floor first.
  dt handling: < 10 ms (burst-drained datagrams after an event-loop stall)
  → sample skipped entirely, baseline untouched; > 2 s → slope reset to 0
  (a fade rate measured before a blackout carries no information about now).
- **Failsafe** (watchdog): no rx_ant for `failsafe_timeout_s` → commit
  `mcs_min`; clears on the next datagram. **Boot-armed** ("GS absent ⇒ max
  robustness"): the watchdog reference is seeded at process start, so a
  vehicle that boots with no GS in range drops to `mcs_min` after
  `failsafe_timeout_s` instead of sitting at the static S99wfb MCS forever.
  The watchdog also syncs the radio cache from the local wfb_tx if a
  failsafe fires before any datagram ever did (otherwise the mcs_min SET
  could not go out). Recovery from any failsafe is NOT a jump back — the
  flag clears on the first datagram, then the law climbs +1 per
  `promote_dwell_s`, each rung gated on a fresh post-gate clean V+2 probe
  reading.

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

CLI values are strict-parsed with the same ranges as the `/set` path
([−120,0] / [0,60] / [−120,0]); a malformed or out-of-range value is a
startup error (a sign typo like `--rssi-floor 85` would otherwise pin
`floor_hit` true and demote to `mcs_min` forever).

Validation warnings (startup + every `/set`):
- `rssi_fade_arm_dbm <= rssi_floor_dbm` (fade rule could never fire before
  the floor rule)
- `probe_clean_milli >= probe_fail_milli` (clean band empty — probe can only
  demote or hold)
- `probe_fail_milli <= demote_per_milli` (probe pre-empt cannot fire before
  the reactive rule)

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
  Note from review: at the default 10 dB/s threshold, rule 3 mostly duplicates
  rules 1/2 (see §2 latency note) — calibration should decide whether to keep
  it, raise the threshold, or derive slope from raw RSSI for real pre-emption.
- Induced-fade dynamic test (operator at bench) — inherited from Phase 4 §8.
- ~~`SD_RECOVERED` decision code is now unreachable~~ — pruned in the §8 batch.

## 8. Hardening batch (2026-06-11 code review)

Full review: 3 independent passes (embedded-C safety, control-law semantics,
integration). Core law verified faithful to §2; all findings were in the
*inputs* to the guard-rails plus one inherited cache desync. Applied:

- **Slope estimator**: burst-drained datagrams (dt < 10 ms, up to 32 queued
  after an event-loop stall) no longer decay the slope or move the baseline;
  gaps > 2 s reset the slope to 0 instead of one decay step (a pre-outage
  −20 dB/s survived as −14 and could spuriously trip the fade rule on
  recovery).
- **NaN/inf rejected** in `parse_strict_double` — `/set?...=nan` previously
  passed the range check (NaN compares false) and silently disarmed the
  guard comparisons while corrupting /status JSON.
- **CLI strict parse + range** for `--rssi-floor/--rssi-fade/--rssi-fade-arm`
  (see §5).
- **Rule-4 fail branch evaluated first** (see §2 table) + two new validation
  warnings (see §5).
- **Live `/set` of `mcs_min`/`mcs_max` re-clamps** the committed MCS on the
  next watchdog tick (≤ failsafe_timeout_s/4); previously a lowered `mcs_max`
  at ceiling hold was never applied until real link damage demoted. The clamp
  commit goes through the normal ordered-drop path (probe retune still leads
  the video SET).
- **Guard flags cleared on failsafe entry** — they are refreshed per-datagram
  and froze at their last value on a dead link, lying in /status.
- **/status renames** (consumers: WebUI updated in the same commit):
  `score.lost_ratio/recov_ratio/diversity_ratio` →
  `smoothed_lost_ratio/smoothed_recov_ratio/smoothed_diversity_ratio` (the
  values were always the EWMA the law evaluates; named honestly now).
  New `mcs.probe.v2_gated` — true when the V+2 rung looks fresh but predates
  the §5.1 commit gate (the law is refusing it).
- `SD_RECOVERED` pruned; stale "mode=1"/"selector_commit_mcs" comments fixed.

Verified: host E2E extended to 24 checks (A–E: original 17 + nan/inf
rejection, live mcs_max re-clamp + restore climb, inverted-threshold
no-promote + recovery, new /status fields), all PASS; 76 wire tests; host +
ARM cross-build clean.

## 9. WCMD radio write-through (review finding B1, separate commit)

`radio_body` was a self-write cache: operator WCMD radio writes
(`WFB_MCS`/`WFB_BANDWIDTH`/`WFB_LDPC`/`WFB_STBC`/`WFB_SHORT_GI`) went
GET→mutate→SET straight to wfb_tx without updating the main loop's cache or
the selector. Consequences (all fixed): the realign never saw the change (it
compares selector vs *cache*, not vs wfb_tx); the probe PHY mirror kept the
old bandwidth, so probe PER measured the wrong PHY; and the next selector
commit rebuilt its SET body from the stale cache, silently reverting the
operator's write.

Fix: a successful WCMD radio SET reports the applied body via
`WcmdState.radio_written{,_body}`; the dispatch site in the main loop adopts
it:

- `radio_body` ← applied body (non-MCS PHY fields now STICK across future
  adaptive commits).
- MCS key: adopted into the selector via `selector_commit` (clamped to
  `[mcs_min,mcs_max]`; out-of-clamp writes get pulled back by the realign on
  the next datagram). The law continues from the operator's rung — manual MCS
  only sticks with `WCMD_KEY_MCS_ENABLED=0`, as before.
- If a deferred MCS-down is in flight, its pending SET body is patched with
  the operator's PHY fields but keeps the adaptive target MCS (adaptive law
  wins the MCS field mid-drop).
- `radio_apply_observation(from_self=true)` arms the FEC settle/boost windows
  so the budget follows the new PHY.

Verified: harness Phase F (F1 probe mirror follows WCMD bandwidth same tick;
F2 adaptive demote commit preserves operator bw=40 — pre-fix it reverted to
20; F3 selector adopts WCMD MCS; F4 probe retunes to adopted V+2; F5 law
re-promotes from the adopted rung). Full harness 29/29.
