"""Smoke + sanity tests for the variable-payload benchmark harness."""

from fec_controller.encoder_sim import SizeProfile
from fec_controller.payload_benchmark import (
    BenchmarkConfig,
    compare_policies,
    format_report,
)


def test_benchmark_runs_to_completion():
    result = compare_policies(BenchmarkConfig(frames=120, fec_k=16))
    assert "fixed" in result and "variable" in result
    assert result["fixed"]["frames"] == 120
    assert result["variable"]["frames"] == 120


def test_benchmark_deterministic():
    a = compare_policies(BenchmarkConfig(frames=120, seed=42))
    b = compare_policies(BenchmarkConfig(frames=120, seed=42))
    assert a == b


def test_variable_beats_fixed_when_mtu_ceiling_differs():
    """Realistic scenario: venc's packetiser is config'd at legacy 1500 B,
    but the radio can carry 3000 B. Variable exploits the wider ceiling;
    fixed cannot.  With large frames, fixed spills, variable doesn't."""
    cfg = BenchmarkConfig(
        frames=300,
        fec_k=8,
        profile=SizeProfile(base=15_000, i_mult=1.1, jitter_sigma=0.02),
        pps_budget=3000.0,
        fixed_payload=1500,
        mtu_override=3000,
    )
    result = compare_policies(cfg)
    # Fixed at 1500 can't contain 15 kB in 8 source packets:
    # ceil(15000/1500) = 10 > 8 -> spill every frame.
    assert result["fixed"]["frames_spilled"] > 0.8 * cfg.frames
    # Variable with room up to 3000 B comfortably fits:
    #   ceil(15000 * 1.1 / 8) = 2063 B payload -> 8 packets, fits.
    assert result["variable"]["frames_spilled"] < 0.1 * cfg.frames
    assert result["variable"]["one_block_hit_rate"] > result["fixed"]["one_block_hit_rate"]


def test_variable_adapts_to_bitrate_ramp():
    """Fixed is tuned for one point; variable follows a ramp. With fixed
    at 1500 B, the post-ramp frames fan out into many source packets;
    variable lifts P toward max_payload to preserve one-block containment."""
    profile = SizeProfile(
        base=3000,
        i_mult=1.0,
        jitter_sigma=0.02,
        gop_interval=30,
        bitrate_events=[(1.5, 20_000)],
    )
    cfg = BenchmarkConfig(
        frames=360,
        fec_k=8,
        profile=profile,
        pps_budget=3000.0,
        fixed_payload=1500,
        mtu_override=3000,
    )
    result = compare_policies(cfg)
    # Variable should meaningfully beat fixed on spill count and
    # one-block rate.  Exact numbers depend on tracker window vs ramp
    # time, but the gap should be substantial (> 15 points).
    assert result["fixed"]["frames_spilled"] > result["variable"]["frames_spilled"]
    gain = (
        result["variable"]["one_block_hit_rate"]
        - result["fixed"]["one_block_hit_rate"]
    )
    assert gain > 0.15, f"one-block rate gain {gain:.2%} < 15 pts"


def test_variable_steady_state_not_strictly_worse():
    """When fixed is already tuned right (no changing conditions), variable
    should not be catastrophically worse — one-block rate within 5 points."""
    cfg = BenchmarkConfig(
        frames=300,
        fec_k=8,
        profile=SizeProfile(base=10_000, i_mult=1.2, jitter_sigma=0.02),
        pps_budget=3000.0,
        fixed_payload=1500,
        mtu_override=1500,
    )
    result = compare_policies(cfg)
    fx = result["fixed"]["one_block_hit_rate"]
    va = result["variable"]["one_block_hit_rate"]
    assert va >= fx - 0.05, f"variable={va:.2%} fixed={fx:.2%}"


def test_format_report_renders():
    r = compare_policies(BenchmarkConfig(frames=60))
    text = format_report(r)
    assert "Variable-payload benchmark" in text
    assert "total source packets" in text
    assert "total wire packets" in text
    assert "one-block hit rate" in text
