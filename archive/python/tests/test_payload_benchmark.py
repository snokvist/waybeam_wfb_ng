"""Smoke + sanity tests for the variable-payload benchmark harness."""

from fec_controller.encoder_sim import SizeProfile
from fec_controller.payload_benchmark import (
    BenchmarkConfig,
    LinkBudgetProfile,
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


def test_link_budget_profile_empty_uses_fallback():
    """Empty schedule -> every query returns the fallback."""
    p = LinkBudgetProfile()
    assert p.value_at(0.0, 1500.0) == 1500.0
    assert p.value_at(10.0, 1500.0) == 1500.0


def test_link_budget_profile_step_function():
    """Budget steps take effect at their scheduled time and persist."""
    p = LinkBudgetProfile(events=[(0.0, 3000.0), (2.0, 600.0), (4.0, 3000.0)])
    assert p.value_at(-0.1, 1500.0) == 1500.0  # before any event -> fallback
    assert p.value_at(0.0, 1500.0) == 3000.0
    assert p.value_at(1.9, 1500.0) == 3000.0
    assert p.value_at(2.0, 1500.0) == 600.0
    assert p.value_at(3.5, 1500.0) == 600.0
    assert p.value_at(4.0, 1500.0) == 3000.0
    assert p.value_at(10.0, 1500.0) == 3000.0


def test_variable_responds_to_mid_stream_budget_drop():
    """When pps_budget collapses mid-stream (MCS drop), variable should
    grow payload to stay within the new packet budget.

    Scenario tuned so the pre-drop raw payload is above min_payload
    (otherwise the floor hides the sizer's response).
    """
    cfg = BenchmarkConfig(
        fps=60,
        frames=360,   # 6 s
        fec_k=16,
        profile=SizeProfile(base=14_000, i_mult=1.0, jitter_sigma=0.02),
        pps_budget=3000.0,
        # Budget drops to 600 pps at t=2s, recovers at t=4s.
        budget_profile=LinkBudgetProfile(events=[
            (0.0, 3000.0),
            (2.0, 600.0),
            (4.0, 3000.0),
        ]),
        fixed_payload=1500,
        mtu_override=3000,
    )
    from fec_controller.payload_benchmark import _run_variable
    stats = _run_variable(cfg)
    pre_drop = [r["payload"] for r in stats.per_frame if r["time_s"] < 1.9]
    during = [r["payload"] for r in stats.per_frame if 2.8 < r["time_s"] < 3.9]
    assert len(pre_drop) > 30 and len(during) > 30
    avg_pre = sum(pre_drop) / len(pre_drop)
    avg_during = sum(during) / len(during)
    # budget_pf drops 50 -> 10, so target_pf: min(50,16)=16 -> min(10,16)=10
    # with S_ref ~= 14000, raw goes 875 -> 1400
    assert avg_during > avg_pre * 1.3, (
        f"payload did not grow enough under tighter budget "
        f"(pre={avg_pre:.0f}, during={avg_during:.0f})"
    )


def test_variable_recovers_after_budget_restore():
    """When the budget comes back after a dip, payload should fall back
    toward its pre-dip value once hysteresis releases."""
    cfg = BenchmarkConfig(
        fps=60,
        frames=480,   # 8 s, enough for post-recovery settling
        fec_k=16,
        profile=SizeProfile(base=14_000, i_mult=1.0, jitter_sigma=0.02),
        pps_budget=3000.0,
        budget_profile=LinkBudgetProfile(events=[
            (0.0, 3000.0),
            (2.0, 600.0),
            (4.0, 3000.0),
        ]),
        fixed_payload=1500,
        mtu_override=3000,
    )
    from fec_controller.payload_benchmark import _run_variable
    stats = _run_variable(cfg)
    during = [r["payload"] for r in stats.per_frame if 2.8 < r["time_s"] < 3.9]
    post = [r["payload"] for r in stats.per_frame if r["time_s"] > 6.0]
    avg_during = sum(during) / len(during)
    avg_post = sum(post) / len(post)
    assert avg_post < avg_during * 0.9, (
        f"payload did not recover (during={avg_during:.0f}, post={avg_post:.0f})"
    )


def test_format_report_renders():
    r = compare_policies(BenchmarkConfig(frames=60))
    text = format_report(r)
    assert "Variable-payload benchmark" in text
    assert "total source packets" in text
    assert "total wire packets" in text
    assert "one-block hit rate" in text
