"""Tests for simulation utilities."""

import random
import pytest

from fec_controller.simulation import simulate_stream, print_reference_table


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
        # Seed that might produce extreme Gaussian values
        random.seed(0)
        # Small base makes negative more likely if not guarded
        simulate_stream(fps=60, base_frame_size=1, duration_s=2.0)
        # No assertion error from the assert inside simulate_stream
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
