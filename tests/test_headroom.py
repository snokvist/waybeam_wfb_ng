"""Tests for HeadroomTracker."""

import pytest

from fec_controller.headroom import HeadroomTracker


class TestHeadroomTracker:

    def _make_tracker(self, **kwargs):
        self._time = 0.0
        kwargs.setdefault("time_fn", lambda: self._time)
        return HeadroomTracker(**kwargs)

    def _advance(self, dt: float):
        self._time += dt

    def test_initial_headroom_is_floor(self):
        tracker = self._make_tracker()
        assert tracker.headroom == 1.05

    def test_uniform_frames_headroom_near_floor(self):
        tracker = self._make_tracker()
        for i in range(100):
            tracker.update(5000.0)
            self._advance(1 / 120)
        # All frames same size -> max/avg ratio ~= 1.0, * margin 1.05 = 1.05
        assert tracker.headroom == pytest.approx(1.05, abs=0.02)

    def test_i_frame_spike_increases_headroom(self):
        tracker = self._make_tracker()
        # Feed mostly P-frames with occasional I-frame
        for i in range(60):
            size = 5000 if i % 30 != 0 else 8000
            tracker.update(float(size))
            self._advance(1 / 60)
        assert tracker.headroom > 1.10

    def test_headroom_capped_at_ceiling(self):
        tracker = self._make_tracker(ceiling=1.40)
        # Feed extreme variance
        for i in range(100):
            size = 1000 if i % 2 == 0 else 50000
            tracker.update(float(size))
            self._advance(1 / 120)
        assert tracker.headroom <= 1.40

    def test_old_samples_expire(self):
        tracker = self._make_tracker(window_s=2.5)
        # Feed a big spike
        tracker.update(20000.0)
        self._advance(0.01)
        # Feed normal frames for 3 seconds (past the window)
        for _ in range(360):
            tracker.update(5000.0)
            self._advance(1 / 120)
        # Spike should have expired
        assert tracker.headroom < 1.15

    def test_margin_applied(self):
        tracker = self._make_tracker(margin=1.10, floor=1.0)
        for i in range(100):
            tracker.update(5000.0)
            self._advance(1 / 120)
        # Uniform frames: max/avg ~1.0, * margin 1.10 = 1.10
        assert tracker.headroom == pytest.approx(1.10, abs=0.02)

    def test_zero_frame_size_returns_floor(self):
        tracker = self._make_tracker()
        tracker.update(0.0)
        assert tracker.headroom == 1.05
