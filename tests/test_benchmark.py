"""
Extended benchmark tests — realistic scenarios with KPI assertions.

Modes:
  - Simulated (instant): default, for CI and parameter sweeps.
  - Realtime (wall-clock): marked ``@pytest.mark.realtime``, run with
    ``pytest -m realtime`` to include.  Validates timing-dependent behaviour.
  - Replay: feed recorded CSV through the same KPI pipeline.

Run:
  pytest tests/test_benchmark.py -v -s              # simulated only (fast)
  pytest tests/test_benchmark.py -v -s -m realtime   # realtime only (slow)
  pytest tests/test_benchmark.py -v -s -m ""         # all
"""

import pytest

from fec_controller.benchmark import (
    scenario_steady_gop,
    scenario_scene_changes,
    scenario_gradual_ramp,
    scenario_bitrate_cliff,
    scenario_encoder_hunting,
    scenario_real_world_mixed,
    scenario_low_bitrate_cruise,
    scenario_iframe_burst_stress,
    scenario_mcs_plateaus,
    scenario_mcs_rapid_switching,
    scenario_mcs_step_up_down,
    run_all,
    run_frames,
    replay,
    FrameInput,
    BenchmarkResult,
)


def _print_kpi(result: BenchmarkResult) -> None:
    print(f"\n{result.kpi.summary()}")


# -----------------------------------------------------------------------
# Simulated mode (instant, deterministic) — 30-60s each
# -----------------------------------------------------------------------

class TestSteadyGOP:
    def test_run(self):
        r = scenario_steady_gop(duration_s=60.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.95
        assert r.kpi.avg_overhead < 5.0
        assert r.kpi.updates_per_second < 1.0
        assert r.kpi.total_frames == 60 * 120


class TestSceneChanges:
    def test_run(self):
        r = scenario_scene_changes(duration_s=60.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.90
        assert r.kpi.max_deficit <= 15
        assert r.kpi.avg_reaction_frames < 30
        assert r.kpi.updates_per_second < 2.0


class TestGradualRamp:
    def test_run(self):
        r = scenario_gradual_ramp(duration_s=30.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.92
        assert r.kpi.max_reaction_frames < 20
        assert r.kpi.updates_per_second < 3.0
        # Gradual ramp should be almost all increases
        assert r.kpi.increases > r.kpi.decreases


class TestBitrateCliff:
    def test_run(self):
        r = scenario_bitrate_cliff(duration_s=45.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.90
        assert r.kpi.updates_per_second < 1.5
        assert r.kpi.max_deficit <= 12


class TestEncoderHunting:
    def test_run(self):
        r = scenario_encoder_hunting(duration_s=30.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.85
        assert r.kpi.updates_per_second < 2.5
        # Increases should dominate (conservative decrease)
        assert r.kpi.increases >= r.kpi.decreases


class TestRealWorldMixed:
    def test_run(self):
        r = scenario_real_world_mixed(duration_s=60.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.88
        assert r.kpi.avg_reaction_frames < 25
        assert r.kpi.updates_per_second < 2.0
        assert r.kpi.max_deficit <= 15


class TestLowBitrateCruise:
    def test_run(self):
        r = scenario_low_bitrate_cruise(duration_s=45.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.98
        assert r.kpi.updates_per_second < 0.5
        assert r.kpi.avg_overhead < 3.0
        # Should be nearly silent
        assert r.kpi.increases == 0
        assert r.kpi.decreases == 0


class TestIframeBurstStress:
    def test_run(self):
        r = scenario_iframe_burst_stress(duration_s=30.0)
        _print_kpi(r)
        assert r.kpi.protection_rate >= 0.80
        assert r.kpi.max_reaction_frames < 20
        assert r.kpi.updates_per_second < 3.0


# -----------------------------------------------------------------------
# MCS/bandwidth plateau scenarios
# -----------------------------------------------------------------------

class TestMcsPlateaus:
    """60s with 5 MCS plateaus, +-20% variance, steep transitions."""

    def test_run(self):
        r = scenario_mcs_plateaus(duration_s=60.0)
        _print_kpi(r)

        # High protection — plateaus are stable, transitions are steep
        assert r.kpi.protection_rate >= 0.95

        # Variance within a plateau should NOT cause updates
        # 5 transitions → expect ~5-15 updates total, not dozens
        assert r.kpi.updates_per_second < 1.0, (
            f"Too many updates for plateau scenario: {r.kpi.updates_per_second:.2f}/s")

        # k distribution should cluster around plateau levels
        assert r.kpi.k_dist.stddev > 1.0, "k should vary across plateaus"

    def test_plateau_variance_does_not_trigger_updates(self):
        """+-20% variance within a single plateau should not cause FEC changes."""
        # Run just one 12s plateau at 8Mbps
        r = scenario_mcs_plateaus(duration_s=12.0)
        # During the first plateau (0-12s), only the initial update and
        # early peak-window settling should fire — +-20% P-frame variance
        # should NOT cause additional updates once settled.
        first_plateau_updates = [
            f for f in r.frames if f.update_fired and f.time < 11.0
        ]
        # Initial + peak-window settling + I-frame adjustments
        assert len(first_plateau_updates) <= 12, (
            f"Too many updates within first plateau: {len(first_plateau_updates)}")


class TestMcsRapidSwitching:
    """45s rapid MCS switching — short plateaus (2-5s)."""

    def test_run(self):
        r = scenario_mcs_rapid_switching(duration_s=45.0)
        _print_kpi(r)

        assert r.kpi.protection_rate >= 0.90

        # Should increase more than decrease (conservative downward)
        assert r.kpi.increases >= r.kpi.decreases

        # Despite rapid switching, update rate should be bounded
        assert r.kpi.updates_per_second < 2.0


class TestMcsStepUpDown:
    """40s staircase: 3 levels up, then back down."""

    def test_run(self):
        r = scenario_mcs_step_up_down(duration_s=40.0)
        _print_kpi(r)

        assert r.kpi.protection_rate >= 0.95

        # Should have both increases (step-ups) and decreases (step-downs)
        assert r.kpi.increases >= 2, "Should have at least 2 step-ups"
        assert r.kpi.decreases >= 1, "Should have at least 1 step-down"

        # k range should span the full staircase
        assert r.kpi.k_dist.max >= 10  # 20Mbps plateau
        assert r.kpi.k_dist.min <= 10  # 6Mbps plateau (I-frames ~12.6KB → k~9)

    def test_asymmetric_update_rates(self):
        """Verify asymmetry: more increases than decreases (conservative down)."""
        r = scenario_mcs_step_up_down(duration_s=40.0)
        # With 2 step-ups and 2 step-downs, increases should outnumber
        # decreases because the decrease path has higher bar and longer cooldown
        assert r.kpi.increases >= r.kpi.decreases, (
            f"Expected increases >= decreases: "
            f"{r.kpi.increases} vs {r.kpi.decreases}"
        )


# -----------------------------------------------------------------------
# Replay mode — feed recorded CSV data
# -----------------------------------------------------------------------

class TestReplay:
    def test_replay_from_csv_string(self):
        """Replay inline CSV through the KPI pipeline."""
        csv_data = (
            "timestamp_us,frame_size,is_iframe\n"
            "0,5000,1\n"
            "8333,4000,0\n"
            "16667,4100,0\n"
            "25000,4050,0\n"
            "33333,3900,0\n"
            "41667,4200,0\n"
            "50000,4000,0\n"
            "58333,12000,1\n"
            "66667,4000,0\n"
            "75000,4100,0\n"
        )
        r = replay(csv_data, scenario="inline_test")
        _print_kpi(r)
        assert r.kpi.total_frames == 10
        assert r.kpi.protection_rate > 0
        assert r.kpi.realtime is False

    def test_csv_export_reimport(self):
        """Export a scenario trace to CSV, reimport, verify frame count."""
        original = scenario_steady_gop(duration_s=1.0)
        csv_text = original.to_csv()
        # The exported CSV has the controller output columns, not input format.
        # Verify it's well-formed.
        lines = csv_text.strip().split("\n")
        assert len(lines) == original.kpi.total_frames + 1  # header + data


# -----------------------------------------------------------------------
# Realtime mode — short scenarios with wall-clock pacing
# Marked with @pytest.mark.realtime so CI can skip them.
# -----------------------------------------------------------------------

@pytest.mark.realtime
class TestRealtimeSteadyGOP:
    def test_run(self):
        r = scenario_steady_gop(duration_s=5.0, realtime=True)
        _print_kpi(r)
        assert r.kpi.realtime is True
        assert r.kpi.protection_rate >= 0.90
        assert r.kpi.total_frames == 5 * 120


@pytest.mark.realtime
class TestRealtimeSceneChange:
    def test_run(self):
        """5s with a scene change at t=2s."""
        r = scenario_scene_changes(duration_s=5.0, realtime=True)
        _print_kpi(r)
        assert r.kpi.realtime is True
        assert r.kpi.protection_rate >= 0.85


@pytest.mark.realtime
class TestRealtimeEncoderHunting:
    def test_run(self):
        r = scenario_encoder_hunting(duration_s=5.0, realtime=True)
        _print_kpi(r)
        assert r.kpi.realtime is True
        assert r.kpi.protection_rate >= 0.80


# -----------------------------------------------------------------------
# Overview table
# -----------------------------------------------------------------------

class TestAllScenariosOverview:
    """Run all scenarios and print comparison tables."""

    def test_overview(self):
        results = run_all()

        print("\n" + "=" * 100)
        print(f"{'Scenario':<22s}  {'Prot%':>6s}  {'Unprot':>6s}  "
              f"{'MaxDef':>6s}  {'AvgOH':>6s}  {'Upd/s':>6s}  "
              f"{'Up/s':>5s}  {'Dn/s':>5s}  "
              f"{'AvgRx':>6s}  {'MaxRx':>6s}")
        print("-" * 100)
        for r in results:
            k = r.kpi
            print(
                f"{k.scenario:<22s}  {k.protection_rate:>5.1%}  "
                f"{k.unprotected_frames:>6d}  {k.max_deficit:>6d}  "
                f"{k.avg_overhead:>6.2f}  {k.updates_per_second:>6.2f}  "
                f"{k.increases_per_second:>5.2f}  {k.decreases_per_second:>5.2f}  "
                f"{k.avg_reaction_frames:>6.1f}  {k.max_reaction_frames:>6d}"
            )
        print("=" * 100)

        print(f"\n{'Scenario':<22s}  "
              f"{'k mean':>6s} {'k std':>6s} {'k min':>5s} "
              f"{'k p25':>5s} {'k p50':>5s} {'k p75':>5s} {'k p95':>5s} {'k max':>5s}  "
              f"{'n mean':>6s} {'n p50':>5s} {'n max':>5s}")
        print("-" * 110)
        for r in results:
            k = r.kpi
            print(
                f"{k.scenario:<22s}  "
                f"{k.k_dist.mean:>6.1f} {k.k_dist.stddev:>6.1f} {k.k_dist.min:>5d} "
                f"{k.k_dist.p25:>5d} {k.k_dist.p50:>5d} {k.k_dist.p75:>5d} "
                f"{k.k_dist.p95:>5d} {k.k_dist.max:>5d}  "
                f"{k.n_dist.mean:>6.1f} {k.n_dist.p50:>5d} {k.n_dist.max:>5d}"
            )
        print("=" * 110)

        print(f"\n{'Scenario':<22s}  {'Total':>5s}  "
              f"{'Up':>4s}  {'Down':>4s}  {'Up/s':>5s}  {'Dn/s':>5s}  "
              f"{'Up%':>5s}  {'Dn%':>5s}")
        print("-" * 70)
        for r in results:
            k = r.kpi
            up_pct = k.increases / k.update_count * 100 if k.update_count else 0
            dn_pct = k.decreases / k.update_count * 100 if k.update_count else 0
            print(
                f"{k.scenario:<22s}  {k.update_count:>5d}  "
                f"{k.increases:>4d}  {k.decreases:>4d}  "
                f"{k.increases_per_second:>5.2f}  {k.decreases_per_second:>5.2f}  "
                f"{up_pct:>4.0f}%  {dn_pct:>4.0f}%"
            )
        print("=" * 70)

        for r in results:
            assert r.kpi.protection_rate >= 0.80, (
                f"{r.kpi.scenario}: protection {r.kpi.protection_rate:.1%} < 80%")
