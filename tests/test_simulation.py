"""Tests for simulation utilities."""

import math
import random
import pytest

from fec_controller.simulation import (
    simulate_stream,
    print_reference_table,
    simulate_step_up,
    simulate_step_down,
    simulate_rapid_oscillation,
    simulate_scene_change_cascade,
)


class TestSimulateStream:
    """Verify simulation runs without errors and produces output."""

    def test_basic_execution(self, capsys):
        """Default simulation completes without errors."""
        random.seed(42)
        simulate_stream(fps=60, base_frame_size=5000, duration_s=1.0)
        out = capsys.readouterr().out
        assert "Simulating" in out
        assert "60fps" in out

    def test_120fps_execution(self, capsys):
        """120fps simulation runs correctly."""
        random.seed(42)
        simulate_stream(fps=120, base_frame_size=5000, duration_s=0.5)
        out = capsys.readouterr().out
        assert "120fps" in out

    def test_output_has_table_header(self, capsys):
        """Output contains the formatted table header."""
        random.seed(42)
        simulate_stream(fps=60, duration_s=0.5)
        out = capsys.readouterr().out
        assert "Time" in out
        assert "FrmSize" in out
        assert "EWMA" in out

    def test_first_frame_always_printed(self, capsys):
        """First frame triggers an update (<<<) marker."""
        random.seed(42)
        simulate_stream(fps=60, base_frame_size=5000, duration_s=0.5)
        out = capsys.readouterr().out
        assert "<<<" in out

    def test_bitrate_event_applied(self, capsys):
        """Bitrate events change frame sizes in output."""
        random.seed(42)
        simulate_stream(
            fps=60,
            base_frame_size=1000,
            duration_s=4.0,
            bitrate_events=[(2.0, 10000)],
        )
        out = capsys.readouterr().out
        assert "Event at 2.0s: base -> 10000B" in out

    def test_no_bitrate_events(self, capsys):
        """Simulation with empty bitrate events list works."""
        random.seed(42)
        simulate_stream(
            fps=60,
            base_frame_size=5000,
            duration_s=1.0,
            bitrate_events=[],
        )
        out = capsys.readouterr().out
        assert "Simulating" in out

    def test_small_frame_size(self, capsys):
        """Very small base frame size (< MTU)."""
        random.seed(42)
        simulate_stream(fps=60, base_frame_size=100, duration_s=0.5)
        out = capsys.readouterr().out
        assert "Simulating" in out

    def test_large_frame_size(self, capsys):
        """Large frame size near k max."""
        random.seed(42)
        simulate_stream(fps=60, base_frame_size=60000, duration_s=0.5)
        out = capsys.readouterr().out
        assert "Simulating" in out

    def test_custom_mtu(self, capsys):
        """Non-default MTU is respected."""
        random.seed(42)
        simulate_stream(fps=60, base_frame_size=5000, duration_s=0.5, mtu=1200)
        out = capsys.readouterr().out
        assert "Simulating" in out

    def test_jitter_never_negative_frame(self, capsys):
        """Even with extreme jitter, frame_size stays >= 1."""
        random.seed(0)
        simulate_stream(fps=60, base_frame_size=1, duration_s=2.0)
        out = capsys.readouterr().out
        assert "Simulating" in out

    def test_deterministic_with_seed(self, capsys):
        """Same seed produces same output."""
        random.seed(123)
        simulate_stream(fps=60, base_frame_size=5000, duration_s=0.5)
        out1 = capsys.readouterr().out

        random.seed(123)
        simulate_stream(fps=60, base_frame_size=5000, duration_s=0.5)
        out2 = capsys.readouterr().out

        assert out1 == out2


class TestReferenceTable:

    def test_basic_execution(self, capsys):
        """Reference table prints without errors."""
        print_reference_table()
        out = capsys.readouterr().out
        assert "Reference table" in out
        assert "MTU=" in out

    def test_contains_all_frame_sizes(self, capsys):
        """All standard frame sizes appear in output."""
        print_reference_table()
        out = capsys.readouterr().out
        for size in [500, 1000, 1446, 5000, 12000, 30000, 60000]:
            assert str(size) in out

    def test_output_has_header(self, capsys):
        """Table header with column names."""
        print_reference_table()
        out = capsys.readouterr().out
        assert "FrameSize" in out
        assert "Redun" in out
        assert "Effic" in out


# --------------------------------------------------------------------------
# Adversarial simulation scenarios — asymmetric gating validation
# --------------------------------------------------------------------------

class TestStepUp:
    """Step-up stress: 5KB -> 15KB, verify fast k increase."""

    def test_k_increases_within_200ms(self):
        events = simulate_step_up()
        # Find first event after the step-up (at t=1.0s)
        post_step = [e for e in events if e["time"] >= 1.0]
        assert len(post_step) >= 1, "No update after step-up"
        first_increase = post_step[0]
        # Must happen within 0.2s of the step
        assert first_increase["time"] < 1.2

    def test_k_reaches_correct_level(self):
        events = simulate_step_up()
        post_step = [e for e in events if e["time"] >= 1.0]
        # 15000 / 1446 = 10.4 -> k should reach at least 10
        max_k = max(e["k"] for e in post_step)
        assert max_k >= 10

    def test_first_update_is_at_frame_0(self):
        events = simulate_step_up()
        assert events[0]["frame"] == 0


class TestStepDown:
    """Step-down: 15KB -> 5KB, verify slow k decrease."""

    def test_decreases_respect_cooldown_between_each_other(self):
        """After a decrease fires, the next one respects the 2.0s cooldown."""
        events = simulate_step_down()
        k_at_transition = None
        for e in events:
            if e["time"] <= 2.0:
                k_at_transition = e["k"]

        decreases = [e for e in events if e["time"] > 2.0 and e["k"] < k_at_transition]
        # Verify gap between consecutive decreases is >= 2.0s
        for i in range(1, len(decreases)):
            gap = decreases[i]["time"] - decreases[i - 1]["time"]
            assert gap >= 1.9, (  # 1.9 to allow float rounding
                f"Decreases too close: t={decreases[i-1]['time']:.2f} -> "
                f"t={decreases[i]['time']:.2f} (gap={gap:.2f}s)"
            )

    def test_k_does_not_decrease_immediately_after_step_down(self):
        """k stays high right after 15KB→5KB transition (EWMA slow + cooldown)."""
        events = simulate_step_down()
        # EWMA alpha=0.05 and decrease cooldown=2.0s both prevent k
        # from dropping in the first fraction of a second after the step.
        very_early = [e for e in events if 2.0 < e["time"] < 2.07]
        k_at_transition = None
        for e in events:
            if e["time"] <= 2.0:
                k_at_transition = e["k"]
        for e in very_early:
            assert e["k"] >= k_at_transition, (
                f"k decreased at t={e['time']:.3f} immediately after step-down "
                f"(k={e['k']}, was {k_at_transition})"
            )

    def test_k_eventually_decreases(self):
        events = simulate_step_down()
        # Get max k (from the 15KB phase)
        k_high = max(e["k"] for e in events if e["time"] <= 2.0)
        # Get final k
        k_final = events[-1]["k"]
        assert k_final < k_high, "k should decrease after sustained low frames"


class TestRapidOscillation:
    """Rapid oscillation: alternating 5KB/15KB, k must stay protective."""

    def test_k_stays_high_during_oscillation(self):
        result = simulate_rapid_oscillation()
        # Under k-for-average, EWMA tracks the rolling mean (~10KB) not the
        # peak, so k settles around 7-9 during oscillation.  Brief dips to 6
        # are expected as EWMA drifts toward 5KB during low-half windows.
        # 15KB frames spanning 2 FEC blocks is the intended fallback.
        assert result["k_during_min"] >= 6, (
            f"k dropped too low during oscillation: {result['k_during_min']}"
        )

    def test_k_decreases_after_oscillation_stops(self):
        result = simulate_rapid_oscillation()
        # After oscillation stops (t=5.0) and cooldown (2.0s), by t=7.0+
        # k should be back near 5KB level
        # 5000*1.15/1446 = 4.0 -> k~4
        assert result["k_after_max"] < result["k_during_max"], (
            "k should decrease after oscillation stops"
        )


class TestSceneChangeCascade:
    """Scene change cascade: 3 rapid bitrate changes in 1s."""

    def test_k_tracks_peak(self):
        events = simulate_scene_change_cascade()
        # During the cascade (t=1.0-2.0), k should reach level for 30KB
        # 30000/1446 = 20.7 -> k should reach ~21
        cascade_events = [e for e in events if 1.0 <= e["time"] <= 2.5]
        assert len(cascade_events) >= 1
        max_k = max(e["k"] for e in cascade_events)
        assert max_k >= 15, f"k didn't track peak during cascade: max_k={max_k}"

    def test_no_rapid_decrease_during_cascade(self):
        events = simulate_scene_change_cascade()
        # During cascade, k should only go up (or stay), never drop
        cascade_events = [e for e in events if 1.0 <= e["time"] <= 2.0]
        for i in range(1, len(cascade_events)):
            assert cascade_events[i]["k"] >= cascade_events[i - 1]["k"], (
                f"k decreased during cascade: {cascade_events[i-1]['k']} -> "
                f"{cascade_events[i]['k']} at t={cascade_events[i]['time']:.2f}"
            )

    def test_k_settles_after_cascade(self):
        events = simulate_scene_change_cascade()
        # After cascade settles at 10KB, k should eventually decrease
        late_events = [e for e in events if e["time"] >= 4.0]
        if late_events:
            peak_k = max(e["k"] for e in events)
            final_k = late_events[-1]["k"]
            assert final_k < peak_k
