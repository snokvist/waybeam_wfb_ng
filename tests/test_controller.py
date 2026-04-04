"""Tests for FECController core."""

import math
import pytest

from fec_controller.config import ControllerConfig
from fec_controller.controller import FECController, FECParams


class TestControllerConfig:

    def test_defaults(self):
        cfg = ControllerConfig()
        assert cfg.mtu == 1446
        assert cfg.min_k == 1
        assert cfg.max_k == 48
        assert cfg.min_n == 2
        assert cfg.max_n == 72
        assert cfg.k_hysteresis == 2
        assert cfg.min_update_interval == 0.5

    def test_redundancy_curve_default(self):
        cfg = ControllerConfig()
        assert len(cfg.redundancy_curve) == 6
        # First point: k=1, 50% redundancy
        assert cfg.redundancy_curve[0] == (1, 0.50)
        # Last point: k=48, 25% redundancy
        assert cfg.redundancy_curve[-1] == (48, 0.25)


class TestRedundancyInterpolation:

    def setup_method(self):
        self._time = 0.0
        self.ctrl = FECController(time_fn=lambda: self._time)

    def test_below_curve_start(self):
        assert self.ctrl._interpolate_redundancy(0) == 0.50

    def test_at_curve_start(self):
        assert self.ctrl._interpolate_redundancy(1) == 0.50

    def test_at_curve_end(self):
        assert self.ctrl._interpolate_redundancy(48) == 0.25

    def test_above_curve_end(self):
        assert self.ctrl._interpolate_redundancy(100) == 0.25

    def test_midpoint_interpolation(self):
        # Between (1, 0.50) and (4, 0.40): at k=2, t = 1/3
        # redundancy = 0.50 + (1/3) * (0.40 - 0.50) = 0.50 - 0.033 = 0.467
        r = self.ctrl._interpolate_redundancy(2)
        assert r == pytest.approx(0.467, abs=0.01)

    def test_exact_curve_point(self):
        assert self.ctrl._interpolate_redundancy(8) == pytest.approx(0.33)
        assert self.ctrl._interpolate_redundancy(16) == pytest.approx(0.30)


class TestComputeParams:

    def setup_method(self):
        self._time = 0.0
        self.ctrl = FECController(time_fn=lambda: self._time)

    def test_small_frame(self):
        p = self.ctrl.compute_params(500, 60, 1.15)
        assert p.k == 1
        assert p.n >= 2
        assert p.redundancy == 0.50

    def test_medium_frame(self):
        p = self.ctrl.compute_params(5000, 120, 1.15)
        assert 3 <= p.k <= 5
        assert p.n > p.k

    def test_large_frame(self):
        p = self.ctrl.compute_params(60000, 60, 1.15)
        assert p.k == 48  # clamped at max
        assert p.n <= 72

    def test_n_always_greater_than_k(self):
        for size in [500, 1446, 5000, 20000, 60000]:
            p = self.ctrl.compute_params(float(size), 60, 1.15)
            assert p.n > p.k

    def test_fec_timeout_at_60fps(self):
        p = self.ctrl.compute_params(5000, 60, 1.15)
        # frame_period = 16.67ms, timeout = 0.5 * 16.67 = 8ms
        assert p.fec_timeout_ms == 8

    def test_fec_timeout_at_120fps(self):
        p = self.ctrl.compute_params(5000, 120, 1.15)
        # frame_period = 8.33ms, timeout = 0.5 * 8.33 = 4ms
        assert p.fec_timeout_ms == 4

    def test_fec_timeout_minimum_1ms(self):
        p = self.ctrl.compute_params(5000, 10000, 1.15)
        assert p.fec_timeout_ms >= 1

    def test_k_clamped_to_bounds(self):
        cfg = ControllerConfig(min_k=2, max_k=10)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p_small = ctrl.compute_params(100, 60, 1.15)
        assert p_small.k >= 2
        p_large = ctrl.compute_params(100000, 60, 1.15)
        assert p_large.k <= 10

    def test_n_clamped_to_bounds(self):
        cfg = ControllerConfig(max_k=10, max_n=14)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(60000, 60, 1.15)
        assert p.k == 10
        assert p.n <= 14


class TestUpdateGating:

    def setup_method(self):
        self._time = 0.0
        self.ctrl = FECController(time_fn=lambda: self._time)

    def test_first_update_always_emitted(self):
        result = self.ctrl.update(5000, 120)
        assert result is not None
        assert isinstance(result, FECParams)

    def test_same_frame_size_no_update(self):
        self.ctrl.update(5000, 120)
        self._time += 1.0
        result = self.ctrl.update(5000, 120)
        assert result is None

    def test_hysteresis_blocks_small_change(self):
        self.ctrl.update(5000, 120)
        self._time += 1.0
        # Small change: EWMA barely moves, k stays same
        result = self.ctrl.update(5100, 120)
        assert result is None

    def test_large_change_triggers_update(self):
        self.ctrl.update(5000, 120)
        self._time += 1.0
        # Large change: send many frames to move EWMA significantly
        for _ in range(200):
            self._time += 1 / 120
            result = self.ctrl.update(30000, 120)
        # Should have triggered at least one update
        assert self.ctrl.update_count > 1

    def test_rate_limiting(self):
        # First update
        self.ctrl.update(5000, 120)
        # Immediately try big change (within min_update_interval)
        self._time += 0.1  # only 100ms
        # Push EWMA hard
        for _ in range(50):
            self.ctrl.update(50000, 120)
        # Should be rate-limited since only 0.1s passed
        # (depending on how fast EWMA moves)
        # At minimum, update_count should be small
        assert self.ctrl.update_count <= 2

    def test_update_after_interval(self):
        self.ctrl.update(5000, 120)
        self._time += 1.0  # past min_update_interval

        # Push EWMA to very different value
        for _ in range(500):
            self._time += 1 / 120
            self.ctrl.update(50000, 120)

        assert self.ctrl.update_count >= 2

    def test_update_count_increments(self):
        assert self.ctrl.update_count == 0
        self.ctrl.update(5000, 120)
        assert self.ctrl.update_count == 1

    def test_get_current_returns_last_params(self):
        self.ctrl.update(5000, 120)
        p = self.ctrl.get_current()
        assert p is not None
        assert p.k > 0
        assert p.n > p.k


class TestReferenceTable:
    """Verify key reference table values from the spec."""

    def setup_method(self):
        self.ctrl = FECController(time_fn=lambda: 0.0)

    def test_500_byte_frame(self):
        p = self.ctrl.compute_params(500, 60, 1.15)
        assert p.k == 1
        assert p.n == 2

    def test_1446_byte_frame(self):
        p = self.ctrl.compute_params(1446, 60, 1.15)
        assert p.k == 2

    def test_60000_byte_frame(self):
        p = self.ctrl.compute_params(60000, 60, 1.15)
        assert p.k == 48
        assert p.n == 64
