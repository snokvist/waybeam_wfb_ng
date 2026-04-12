# Variable-Payload — Adversarial Review Findings & Next Steps

Captured 2026-04-12 after an independent Codex adversarial pass over
PRs #5, #6, #7 and a source-level verification of wfb-ng's FEC padding
rule. The policy math is sound and the upstream FEC-padding assumption
checks out; three real defects were found that need fixing before the
sizer is ever promoted from read-only to authoritative.

## Validated

- **Per-block RS padding confirmed.** Upstream `wfb-ng/src/tx.cpp:376-399`
  keeps `max_packet_size` per in-flight block, rounds it with
  `ZFEX_ROUND_UP_SIMD`, and calls `fec_encode_simd(…, that_size)` at
  `fragment_idx == fec_k`. Public header states "FEC packet size is max
  of all UDP packet sizes in this block" (`src/wifibroadcast.hpp:104-118`).
  Our SHM patch does not alter that path.
- **Variable P in `[800, 3900]` with P constant within a block is fully
  efficient on the wire.** No global-max padding across blocks. No
  session-level MTU knob (upstream only exposes MCS via `-M`). Compile-
  time ceiling is `WIFI_MTU=4045` minus headers; our 3900 cap is safely
  under.

## Defects found

### 1. Hysteresis can revert the one-block lift AND produce payload > max_payload

**Severity: implementation bug. Real.**

`fec_controller/payload_sizer.py:111` applies the one-block lift *before*
`:121` applies the hysteresis clamp. Hysteresis can roll back the lift,
and can also restore a `prev` that is outside the currently valid
`[min_payload, max_payload]` window. Concrete repros:

```
# One-block rule reverted:
s_ref=9001, fps=60, pps_budget=3000, fec_k=8, prev=1040
  raw=1125 → lift to 1126 (fits in 8) → hysteresis clamps back to 1040
  → returns payload=1040, packets_per_frame=9, fits_in_block=False
```

```
# Clamp violated:
s_ref=12000, fps=60, pps_budget=1500, fec_k=8, prev=1600, mtu_override=1500
  raw=1500 (at max) → hysteresis clamps back to prev=1600
  → returns payload=1600 > max_payload=1500
```

**Fix:** reorder so hysteresis is applied first, then re-run the
one-block lift and re-clamp to `[min_payload, max_payload]`. Equivalent:
allow hysteresis to accept the new candidate whenever rejecting it
would exit the valid range or flip `fits_in_block` from true to false.

### 2. `block_model.wire_bytes` over-charges source padding

**Severity: design flaw in the benchmark (not the policy).**

`fec_controller/block_model.py:65` sums `source_padding` into
`wire_bytes`. Upstream wfb-ng sends source packets at **actual size** —
only recovery symbols are padded to block-max. The benchmark therefore
inflates the fixed-P total-wire-bytes number (fixed has more padding
bytes charged because its last packet is smaller on large frames).

**Fix:** `wire_bytes = source_bytes + recovery_bytes` (drop the
`source_padding` term). Keep `source_padding` as a diagnostic field but
do not add it to wire totals. Re-run the benchmarks and walk back the
wire-byte headline numbers accordingly. The **policy** metrics
(one-block hit rate, source-packet count, spill count) are unaffected
and the variable-P wins on those remain valid.

### 3. Sizer's `target_fec_k` diverges from the controller's actual `k`

**Severity: design flaw. Cosmetic today, load-bearing when authoritative.**

`ControllerConfig.target_fec_k` is static (default 8). `FECController`
dynamically picks `k = ceil(F·h / MTU)` per frame from
`config.mtu=1446`. These routinely disagree. With
`target_fec_k=16, s_ref=14000, pps_budget=3000`, sizer picks
`payload=875` planning 16 packets/frame; legacy controller sets wfb_tx
to `k ≈ 10`. `Decision.fits_in_block=True` in the logs refers to a k
that is not on the wire.

**Fix (read-only phase):** sizer reads
`FECController.get_current().k` when available, falls back to
`config.target_fec_k`. Log both values so it is visible when they
disagree.

**Fix (authoritative phase, later):** sizer's `(P, k)` becomes the
single source of truth. `FECController` either consumes the sizer's k
directly or is retired in the payload-variable code path.

## Test gaps worth closing

### P99 windows that miss sparse IDRs

`FrameSizePercentile` uses `idx = ceil(q·n) - 1`. P99 needs > 1 % of the
window in IDRs to catch one. Combinations that miss today:

- 120 fps / 2.5 s window / GOP 120 → 2.5/300 = 0.83 % IDRs → P-frame band
- 60 fps / 2.5 s window / GOP 150 → 1/150 → P-frame band

Either bump default toward `s_ref_quantile = 1.0` (max-over-window) or
provide a `recommended_quantile(fps, window_s, gop) → float` helper +
design-note table. Belt-and-suspenders: the sizer's explicit one-block
lift still catches the next too-large frame regardless of S_ref lag.

### Off-by-one on expiry

`frame_size_percentile.py:64` uses `< cutoff`. `<=` is the more common
rolling-window convention and removes a 1-sample boundary artefact.
Minor.

### Benchmark scenarios biased toward variable-P wins

Every current scenario either differs the payload ceilings (fixed=1500,
variable=3000) or applies a ramp/drop mid-stream. No "fixed-P honestly
wins" case. Add a steady, well-tuned scenario where variable's
`min_payload=800` floor hurts vs fixed at ~1200. Target params:
`fps=60, base≈2200, i_mult≈1.3, fec_k=16, fixed_payload=1500,
mtu_override=1500, pps_budget=12000`.

## Not bugs

- **Service same-frame feedback** (`service.py:173` updates percentile
  with frame N then sizes from it). Benign while read-only; document as
  "when authoritative, switch to next-frame control".
- **`_prev_payload` race** — none. Single-threaded async,
  `_handle_frame` is the only mutator.

## Proposed PRs (ordered by priority)

### PR A — Hysteresis, block-model, k-source fixes

1. Reorder `choose_payload_size` to apply hysteresis first, then the
   one-block lift, then the final clamp. Or disallow hysteresis from
   reverting a `fits_in_block=True` decision or exiting the clamp range.
2. Fix `BlockStats.wire_bytes` to exclude `source_padding`.
3. Plumb `FECController.get_current().k` into the sizer as `fec_k` when
   available; log both `target_fec_k` and `actual_k`.
4. Add repro tests for all three defects using the concrete examples
   above.
5. Re-run `payload-benchmark` CLI and update `docs/variable-payload.md`
   with corrected wire-byte numbers.

### PR B — Test gaps and percentile tuning

6. Add the "fixed-P wins" benchmark scenario to
   `tests/test_payload_benchmark.py`.
7. Fix the `< cutoff` → `<= cutoff` boundary in
   `FrameSizePercentile`.
8. Either bump default `s_ref_quantile` toward 1.0 or add
   `recommended_quantile(fps, window_s, gop)` helper with design-note
   table covering typical FPV configurations.

### PR C — Canonical sidecar spec (cross-repo, coordination only)

9. Promote `waybeam_wfb_ng/poc/SIDECAR_INFO.md` to
   `waybeam-coordination/protocols/venc-sidecar.md`. Pin
   `MSG_LINK_BUDGET`, `MSG_SET_PAYLOAD`, and FRAME trailer v2 wire
   formats before any of venc, mod_aalink, or the service-side sender
   attempt an implementation. Docs-only; no code moves across repos.

## Deferred until after PR A–C land

- Wiring `MSG_LINK_BUDGET` into `mod_aalink` (waybeam-hub).
- Wiring `MSG_SET_PAYLOAD` reception into `waybeam_venc` packetiser.
- Promoting `ControllerConfig.enable_variable_payload` to authoritative
  (requires PR A item 3 plus a representative-stream replay that
  confirms the policy on real frame distributions).
- Pre-existing `cli.py:175` bug (`min_update_interval` not a
  `ControllerConfig` field) — unrelated, blocks `python -m fec_controller
  run` end-to-end but not relevant to the sizer work.

## Source references

- Codex task `task-mnw23sl5-96qi3b` — adversarial review (effort high,
  3m45s).
- Codex task `task-mnw23z3w-7860s2` — wfb-ng padding verification
  (effort medium, 3m45s).
- Upstream wfb-ng: `src/tx.cpp:376-399`, `src/wifibroadcast.hpp:104-118`,
  `src/rx.cpp:690-799`, `src/zfex.h:56-60`.
