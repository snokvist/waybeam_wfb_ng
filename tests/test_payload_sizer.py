"""Acceptance tests for the variable-payload sizer.

Each test is named against one bullet in the master prompt's acceptance
criteria. See docs/variable-payload.md for the mapping.
"""

import math

import pytest

from fec_controller.payload_sizer import (
    DEFAULT_HYSTERESIS,
    DEFAULT_MIN_PAYLOAD,
    DEFAULT_MTU_OVERRIDE,
    MAX_PAYLOAD_HARD_CAP,
    PPS_BUDGET_FALLBACK,
    Decision,
    choose_payload_size,
)


# ---------------------------------------------------------------------------
# Clamp behaviour
# ---------------------------------------------------------------------------

def test_sizer_clamps_min():
    """Tiny frames get clamped to min_payload, not below."""
    d = choose_payload_size(s_ref=100, fps=60, pps_budget=3000, fec_k=16)
    assert d.payload == DEFAULT_MIN_PAYLOAD
    assert d.min_payload == DEFAULT_MIN_PAYLOAD
    assert "raw_below_min" in d.reason


def test_sizer_clamps_max():
    """Huge frames get capped at min(mtu_override, 3900)."""
    d = choose_payload_size(
        s_ref=200_000, fps=60, pps_budget=3000, fec_k=16,
        mtu_override=4000,
    )
    assert d.payload == MAX_PAYLOAD_HARD_CAP
    assert d.max_payload == MAX_PAYLOAD_HARD_CAP


def test_sizer_respects_mtu_override_below_hard_cap():
    """mtu_override lower than 3900 becomes the effective cap."""
    d = choose_payload_size(
        s_ref=200_000, fps=60, pps_budget=3000, fec_k=16,
        mtu_override=1500,
    )
    assert d.payload == 1500


# ---------------------------------------------------------------------------
# Monotonicity
# ---------------------------------------------------------------------------

def test_sizer_grows_with_s_ref():
    """Larger frames -> larger payload (all else equal). Inputs chosen so
    the raw formula produces values above min_payload on both sides."""
    small = choose_payload_size(
        s_ref=8000, fps=60, pps_budget=3000, fec_k=8, mtu_override=3000,
    )
    large = choose_payload_size(
        s_ref=20000, fps=60, pps_budget=3000, fec_k=8, mtu_override=3000,
    )
    assert large.payload > small.payload
    assert small.payload >= small.min_payload


def test_sizer_floors_at_min_payload_for_tiny_frames():
    """Far below the floor, payload is pinned at min_payload regardless of
    further decreases in s_ref — this is the correct behaviour, not a bug."""
    d1 = choose_payload_size(s_ref=3000, fps=60, pps_budget=3000, fec_k=16)
    d2 = choose_payload_size(s_ref=1000, fps=60, pps_budget=3000, fec_k=16)
    assert d1.payload == d2.payload == DEFAULT_MIN_PAYLOAD


def test_sizer_grows_when_budget_tightens():
    """Lower pps_budget -> larger payload to keep packet count down."""
    loose = choose_payload_size(s_ref=8000, fps=60, pps_budget=6000, fec_k=16)
    tight = choose_payload_size(s_ref=8000, fps=60, pps_budget=1800, fec_k=16)
    assert tight.payload >= loose.payload
    assert tight.budget_pf <= loose.budget_pf


# ---------------------------------------------------------------------------
# One-block precedence
# ---------------------------------------------------------------------------

def test_sizer_prefers_one_block():
    """When the initial raw P would spill past fec_k, the sizer lifts P."""
    # fec_k=4, s_ref=8000, small mtu -> natural P would be 2000 if the
    # budget allowed; but fec_k=4 should constrain packets to <=4.
    d = choose_payload_size(
        s_ref=8000, fps=60, pps_budget=10000, fec_k=4,
        mtu_override=3000,
    )
    assert d.packets_per_frame <= 4
    assert d.fits_in_block


def test_sizer_pads_rather_than_spills():
    """If the sizer can fit the frame in one block by using more P, it does —
    even if the raw formula would have picked less."""
    d = choose_payload_size(
        s_ref=12000, fps=60, pps_budget=10000, fec_k=4,
        mtu_override=3900,
    )
    # 12000 / 4 = 3000 -> must lift to at least 3000
    assert d.payload >= 3000
    assert d.fits_in_block


def test_sizer_allows_spill_when_unavoidable():
    """If even max_payload can't contain the frame, accept spill."""
    d = choose_payload_size(
        s_ref=40_000, fps=60, pps_budget=10000, fec_k=4,
        mtu_override=3900,
    )
    assert d.payload == 3900
    assert not d.fits_in_block
    assert "spill_unavoidable" in d.reason


# ---------------------------------------------------------------------------
# Hysteresis
# ---------------------------------------------------------------------------

def test_sizer_hysteresis_no_flap():
    """Small relative drift in S_ref should NOT change payload."""
    d1 = choose_payload_size(
        s_ref=8000, fps=60, pps_budget=3000, fec_k=16, prev=None,
    )
    # Drift by 5 % - should clamp back to d1.payload
    d2 = choose_payload_size(
        s_ref=8400, fps=60, pps_budget=3000, fec_k=16, prev=d1.payload,
    )
    assert d2.payload == d1.payload
    assert "hysteresis_clamp" in d2.reason


def test_sizer_hysteresis_crosses_threshold():
    """Large enough drift overrides hysteresis."""
    d1 = choose_payload_size(
        s_ref=4000, fps=60, pps_budget=3000, fec_k=16, prev=None,
    )
    d2 = choose_payload_size(
        s_ref=16000, fps=60, pps_budget=3000, fec_k=16, prev=d1.payload,
    )
    assert d2.payload != d1.payload
    assert "hysteresis_clamp" not in d2.reason


def test_sizer_custom_hysteresis():
    """A higher hysteresis threshold holds through larger drifts."""
    d1 = choose_payload_size(
        s_ref=8000, fps=60, pps_budget=3000, fec_k=16, prev=None,
    )
    d2 = choose_payload_size(
        s_ref=11000, fps=60, pps_budget=3000, fec_k=16,
        prev=d1.payload, hysteresis=0.5,
    )
    assert d2.payload == d1.payload


# ---------------------------------------------------------------------------
# Fallback / edge cases
# ---------------------------------------------------------------------------

def test_sizer_pps_fallback():
    """None pps_budget uses the conservative fallback."""
    d = choose_payload_size(s_ref=6000, fps=60, pps_budget=None, fec_k=16)
    assert d.pps_budget == PPS_BUDGET_FALLBACK
    # With fallback 1500 pps / 60 fps = 25 pf, fec_k=16 constrains further
    assert d.target_pf == 16


def test_sizer_zero_pps_falls_back():
    """pps_budget <= 0 falls through to the fallback (defensive)."""
    d = choose_payload_size(s_ref=6000, fps=60, pps_budget=0, fec_k=16)
    assert d.pps_budget == PPS_BUDGET_FALLBACK


def test_sizer_rejects_invalid_inputs():
    with pytest.raises(ValueError):
        choose_payload_size(s_ref=0, fps=60, pps_budget=3000, fec_k=16)
    with pytest.raises(ValueError):
        choose_payload_size(s_ref=1000, fps=0, pps_budget=3000, fec_k=16)
    with pytest.raises(ValueError):
        choose_payload_size(s_ref=1000, fps=60, pps_budget=3000, fec_k=0)
    with pytest.raises(ValueError):
        choose_payload_size(s_ref=1000, fps=60, pps_budget=3000, fec_k=16,
                            min_payload=0)


def test_sizer_rejects_impossible_range():
    """min_payload > max_payload is a config error."""
    with pytest.raises(ValueError):
        choose_payload_size(
            s_ref=1000, fps=60, pps_budget=3000, fec_k=16,
            min_payload=2000, mtu_override=1500,
        )


# ---------------------------------------------------------------------------
# Determinism / diagnostics
# ---------------------------------------------------------------------------

def test_sizer_deterministic_under_same_inputs():
    """Same inputs -> same output, bit-for-bit."""
    a = choose_payload_size(s_ref=7321, fps=60, pps_budget=2750, fec_k=16)
    b = choose_payload_size(s_ref=7321, fps=60, pps_budget=2750, fec_k=16)
    assert a == b


def test_decision_fields_populated():
    d = choose_payload_size(s_ref=8000, fps=60, pps_budget=3000, fec_k=16)
    assert d.payload > 0
    assert d.packets_per_frame >= 1
    assert d.s_ref == 8000
    assert d.fps == 60
    assert d.fec_k == 16
    assert d.reason
    assert d.packets_per_frame == math.ceil(d.s_ref / d.payload)
