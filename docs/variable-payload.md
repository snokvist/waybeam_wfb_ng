# Variable NAL / Payload Sizing — Design Note

Host-side simulation of an adaptive RTP-payload sizing policy for the
custom wfb-ng inject pipeline. The goal is **not** generic-Internet
packetisation; it is to collapse one encoded frame into one FEC source
block whenever physically possible, at the lowest packet count the
current WLAN link can sustain.

## Ordering

k-first becomes P-first. Current `FECController` computes
`k = ceil(F·h / MTU)` at a fixed MTU. The new path inverts it: given a
desired `fec_k` cap and a measured `pps_budget`, pick `P` so the frame
fits, then derive the actual `k` from `ceil(F / P)`. The existing
asymmetric gating and oscillation detector still apply — to `k` changes
as before, and to `P` changes via the sizer's own hysteresis.

## Inputs

| Name | Source | Notes |
|---|---|---|
| `S_ref` | percentile tracker over recent frame sizes | P95 of a rolling window, not EMA alone — biases toward IDR absorption |
| `fps` | `FPSEstimator` (existing) | derived from `frame_ready_us` intervals |
| `pps_budget` | `mod_aalink` (not in this sim) — stub estimator here | rolling-window estimator with conservative fallback (1500 pps) and freshness TTL |
| `fec_k` | user-configured or derived from radio profile | the preferred cap on packets per frame; not the same as the emitted FEC `k` |
| `min_payload` | config (default 800) | below this, tiny frames get padded into a single slot-sized packet |
| `mtu_override` | config (default 1500) | wall-clock MTU hint from the link layer |
| `max_payload_cap` | hard constant 3900 | absolute ceiling regardless of `mtu_override` |

## Core formula

```
max_payload  = min(mtu_override, 3900)
budget_pf    = floor(pps_budget / max(1, fps))      # packets available per frame
target_pf    = max(1, min(budget_pf, fec_k))        # tighter of link and FEC caps
raw_payload  = ceil(S_ref / target_pf)
payload      = clamp(raw_payload, min_payload, max_payload)

# FEC one-block rule (dominates)
if ceil(S_ref / payload) > fec_k and payload < max_payload:
    payload = min(max_payload, ceil(S_ref / fec_k))

# Hysteresis: reject sub-threshold changes
if prev is not None and abs(payload - prev) / prev < hysteresis:
    payload = prev
```

`packets_per_frame = ceil(S_ref / payload)`; `fits_in_block = packets_per_frame <= fec_k`.

## Why P99 (not P95) and not EMA

`HeadroomTracker` already exists but clamps at 1.40 × avg. For our workload
an IDR frame is typically 5–8 × a P-frame. Using EMA × 1.40 under-sizes P
for IDRs and forces 2-block spills on every GOP boundary. A percentile
tracker over the recent size window catches IDRs directly in `S_ref`.

The right quantile depends on IDR frequency. At 60 fps with a 30-frame GOP
over a 2.5 s window, IDRs are 5/150 = 3.3 % of samples. P95 lands on the
P-frame band; P99 on the IDR band. At 120 fps / 60-frame GOP it's 5/300 =
1.7 %, still inside P99 headroom. So the default is **P99** over a 2.5 s
window (configurable). Outliers beyond P99 are allowed to spill to two
blocks (by design — the prompt accepts this).

Belt-and-suspenders: even when S_ref underestimates (e.g. at startup before
an IDR lands), the sizer's explicit "lift payload for one-block containment"
step catches the next too-large frame and grows P to `ceil(frame / fec_k)`
on the spot.

The existing `HeadroomTracker` is retained for the legacy k-first path;
the sizer uses a sibling `frame_size_percentile.py` so the two policies
can run side-by-side in simulation without interfering.

## Why per-block padding is free for us

wfb-ng RS-encodes each block at the size of the block's largest source
packet. Because P is committed per frame (= per block) and stays constant
within a block, there is no intra-block pad penalty. Inter-block changes
in P cost nothing on the wire.

## Why padding to `min_payload` is worthwhile

Small frames would otherwise generate a single very small packet. Because
every RS recovery symbol is padded up to block-max, a 200-byte source +
a `P=800` recovery symbol is still smaller and carries more protection
than a 200-byte source + a 200-byte recovery symbol when the next block's
P jumps. `min_payload=800` also keeps packet-count fluctuations from
driving pps volatility down into the range where A-MPDU efficiency drops.

## Hysteresis

Relative-change threshold (default 12 %). The prompt suggests
10–15 % or "persist for N frames". The former is cheaper to reason about
and composes cleanly with the existing k-side gating; picked that.

Stronger anti-flap: the decision's `reason` field records `"hysteresis_clamp"`
when a candidate is rejected, so benchmark output can show how often the
threshold fires.

## Encoder & block simulation

Three new pure Python components, all deterministic under a seeded RNG:

- `encoder_sim.EncoderSim` — generates per-frame `(size, type)` pairs
  from a configurable size distribution (base × I-multiplier × jitter,
  plus bitrate-event schedule). Applies the current P to emit a packet
  list for each frame.
- `block_model.FECBlock.emit()` — models one FEC block: input packet
  list, chosen `(k, n)`, outputs `BlockStats(wire_bytes, padding_bytes,
  largest_packet, packets_in_block, spilled_to_next)`.
- `benchmark.compare_policies()` — runs fixed-P (status-quo) and
  variable-P over the same trace, reports: total packets, total wire
  bytes, FEC padding bytes, one-block hit rate, P volatility, k volatility.

The sizer consumes parsed FRAME messages and emits `Decision` records;
the benchmark reads both to compute the comparison tables.

## Acceptance tests (mapped to prompt)

| Criterion | Test |
|---|---|
| Clamp `[min, max]` | `test_sizer_clamps_min`, `test_sizer_clamps_max` |
| Grows with avg frame size | `test_sizer_grows_with_s_ref` |
| Grows with tighter budget | `test_sizer_grows_when_budget_tightens` |
| Prefers single-block fit | `test_sizer_prefers_one_block`, `test_sizer_allows_spill_when_unavoidable` |
| Padding preferred over spill | `test_sizer_pads_rather_than_spills` |
| No flap on small drift | `test_sizer_hysteresis_no_flap` |
| Determinism | `test_sizer_deterministic_under_seed` |

## Out of scope for this slice

- Wire-format changes to the sidecar (`MSG_LINK_BUDGET`, `MSG_SET_PAYLOAD`,
  trailer v2). Documented elsewhere when the time comes to leave the sim.
- Real `mod_aalink` integration. The `LinkBudgetEstimator` takes
  hand-fed samples in tests and sim.
- venc packetiser control-plane. Sim emulates the outcome only.
- wfb_tx behavioural changes. Sim assumes the documented "per-block
  largest-packet padding" rule, which has been confirmed against upstream.

## Non-goals

- Replacing `FECController`. The feature is opt-in via
  `ControllerConfig.enable_variable_payload`; the legacy path stays the
  default until sim numbers justify the migration.
- Optimising for non-WLAN transports.
- Handling per-NAL sizing (below the frame granularity). Frame-level is
  the natural commit boundary and matches the one-block-per-frame rule.
