"""
Variable NAL/payload sizing — P-first policy for single-block frame containment.

See docs/variable-payload.md for the full design note. This module is
deliberately pure: one function, one dataclass, no I/O, no time source,
fully deterministic given its inputs.

Policy summary:
    - pick payload so ceil(S_ref / payload) <= target_pf where
      target_pf = min(budget_pf, fec_k)
    - clamp to [min_payload, min(mtu_override, 3900)]
    - if the result still spills beyond fec_k, grow payload up to max
      rather than allow the spill
    - hysteresis: reject candidates that move by less than a relative
      threshold from the previous payload
"""

import math
from dataclasses import dataclass

MAX_PAYLOAD_HARD_CAP = 3900
DEFAULT_MIN_PAYLOAD = 800
DEFAULT_MTU_OVERRIDE = 1500
DEFAULT_HYSTERESIS = 0.12
PPS_BUDGET_FALLBACK = 1500


@dataclass(frozen=True)
class Decision:
    """Result of one sizing decision. All fields are observable telemetry."""
    payload: int
    packets_per_frame: int
    fits_in_block: bool
    s_ref: float
    fps: float
    pps_budget: float
    budget_pf: int
    target_pf: int
    fec_k: int
    raw_payload: int
    max_payload: int
    min_payload: int
    reason: str


def choose_payload_size(
    s_ref: float,
    fps: float,
    pps_budget: float | None,
    fec_k: int,
    min_payload: int = DEFAULT_MIN_PAYLOAD,
    mtu_override: int = DEFAULT_MTU_OVERRIDE,
    *,
    prev: int | None = None,
    hysteresis: float = DEFAULT_HYSTERESIS,
) -> Decision:
    """Compute a payload size for the next frame.

    Args:
        s_ref: reference frame size in bytes (typically a percentile of
            recent frame sizes, not raw EMA — see docs).
        fps: current estimated frame rate, > 0.
        pps_budget: sustainable packets/s on the link, or None to use the
            conservative fallback.
        fec_k: preferred cap on packets per frame (one FEC source block).
        min_payload: floor, default 800.
        mtu_override: wall-clock MTU hint, capped at MAX_PAYLOAD_HARD_CAP.
        prev: previously committed payload, for hysteresis. None on the
            first call.
        hysteresis: relative-change threshold under which a new candidate
            is rejected in favour of `prev`. Default 0.12 (12 %).

    Returns:
        Decision record with the chosen payload and diagnostics.
    """
    if s_ref <= 0:
        raise ValueError(f"s_ref must be positive, got {s_ref}")
    if fps <= 0:
        raise ValueError(f"fps must be positive, got {fps}")
    if fec_k < 1:
        raise ValueError(f"fec_k must be >= 1, got {fec_k}")
    if min_payload < 1:
        raise ValueError(f"min_payload must be >= 1, got {min_payload}")

    max_payload = min(int(mtu_override), MAX_PAYLOAD_HARD_CAP)
    if max_payload < min_payload:
        raise ValueError(
            f"max_payload ({max_payload}) < min_payload ({min_payload})"
        )

    effective_budget = pps_budget if (pps_budget and pps_budget > 0) else PPS_BUDGET_FALLBACK
    budget_pf = max(1, int(effective_budget // fps))
    target_pf = max(1, min(budget_pf, fec_k))

    raw_payload = math.ceil(s_ref / target_pf)
    payload = max(min_payload, min(raw_payload, max_payload))

    reason_parts: list[str] = []
    if raw_payload > max_payload:
        reason_parts.append("raw_above_max")
    if raw_payload < min_payload:
        reason_parts.append("raw_below_min")
    if budget_pf < fec_k:
        reason_parts.append("budget_tighter_than_k")

    # FEC one-block rule: if the current payload still spills beyond fec_k,
    # grow payload up to max rather than allow the spill. This can only
    # trigger when raw_payload < max_payload initially (otherwise we were
    # already at the ceiling).
    packets = math.ceil(s_ref / payload)
    if packets > fec_k and payload < max_payload:
        lifted = math.ceil(s_ref / fec_k)
        payload = min(max_payload, lifted)
        packets = math.ceil(s_ref / payload)
        reason_parts.append("lifted_for_one_block")

    if packets > fec_k:
        # Even at max_payload we can't fit. Accept spill.
        reason_parts.append("spill_unavoidable")

    # Hysteresis — reject sub-threshold changes
    if prev is not None and prev > 0:
        rel = abs(payload - prev) / prev
        if rel < hysteresis:
            payload = prev
            packets = math.ceil(s_ref / payload)
            reason_parts.append("hysteresis_clamp")

    if not reason_parts:
        reason_parts.append("normal")

    return Decision(
        payload=int(payload),
        packets_per_frame=int(packets),
        fits_in_block=packets <= fec_k,
        s_ref=float(s_ref),
        fps=float(fps),
        pps_budget=float(effective_budget),
        budget_pf=int(budget_pf),
        target_pf=int(target_pf),
        fec_k=int(fec_k),
        raw_payload=int(raw_payload),
        max_payload=int(max_payload),
        min_payload=int(min_payload),
        reason="+".join(reason_parts),
    )
