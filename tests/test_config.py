"""Tests for ControllerConfig — extreme and invalid values."""

import pytest

from fec_controller.config import ControllerConfig
from fec_controller.controller import FECController


class TestConfigExtremes:
    """Verify controller handles extreme config values without crashing."""

    def test_zero_mtu(self):
        """MTU=0 -> division by zero in packets_per_frame."""
        cfg = ControllerConfig(mtu=0)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        with pytest.raises(ZeroDivisionError):
            ctrl.compute_params(5000, 60, 1.15)

    def test_negative_mtu(self):
        """Negative MTU produces negative packets_per_frame -> clamped to min_k."""
        cfg = ControllerConfig(mtu=-1)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(5000, 60, 1.15)
        assert p.k >= 1

    def test_min_k_greater_than_max_k(self):
        """Inverted k bounds — k clamped to max_k (which is less than min_k)."""
        cfg = ControllerConfig(min_k=10, max_k=5)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(5000, 60, 1.15)
        assert p.k == 10
        assert p.n > p.k

    def test_min_n_greater_than_max_n(self):
        """Inverted n bounds — n clamped, but n > k enforced."""
        cfg = ControllerConfig(min_n=20, max_n=10)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(500, 60, 1.15)
        assert p.n > p.k

    def test_empty_redundancy_curve(self):
        """Empty curve raises on first interpolation (IndexError)."""
        cfg = ControllerConfig(redundancy_curve=[])
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        with pytest.raises(IndexError):
            ctrl.compute_params(5000, 60, 1.15)

    def test_single_point_curve(self):
        """Single-point curve: same redundancy for all k."""
        cfg = ControllerConfig(redundancy_curve=[(1, 0.40)])
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(5000, 60, 1.15)
        assert p.redundancy == pytest.approx(0.40)

    def test_zero_timeout_fraction(self):
        """timeout_fraction=0 -> fec_timeout clamped to 1ms minimum."""
        cfg = ControllerConfig(timeout_fraction=0.0)
        ctrl = FECController(config=cfg, time_fn=lambda: 0.0)
        p = ctrl.compute_params(5000, 60, 1.15)
        assert p.fec_timeout_ms == 1

    def test_zero_hysteresis_up(self):
        """k_hysteresis_up=0: every k increase triggers update."""
        t = [0.0]
        cfg = ControllerConfig(k_hysteresis_up=0, min_interval_up=0.0)
        ctrl = FECController(config=cfg, time_fn=lambda: t[0])
        ctrl.update(5000, 60)
        t[0] = 0.01
        # Even a tiny increase should trigger
        for _ in range(50):
            t[0] += 0.01
            ctrl.update(8000, 60)
        assert ctrl.update_count >= 2

    def test_zero_interval_up(self):
        """min_interval_up=0: no rate limiting on increases."""
        t = [0.0]
        cfg = ControllerConfig(min_interval_up=0.0)
        ctrl = FECController(config=cfg, time_fn=lambda: t[0])
        ctrl.update(2000, 60)
        # Rapid increases with no cooldown
        for size in [5000, 10000, 20000, 40000]:
            t[0] += 0.001
            ctrl.update(size, 60)
        assert ctrl.update_count >= 3

    def test_custom_config_overrides(self):
        """Verify custom values are preserved."""
        cfg = ControllerConfig(
            mtu=1200, min_k=2, max_k=32, ewma_alpha=0.1, peak_window=16,
        )
        assert cfg.mtu == 1200
        assert cfg.min_k == 2
        assert cfg.max_k == 32
        assert cfg.ewma_alpha == 0.1
        assert cfg.peak_window == 16

    def test_asymmetric_defaults_sensible(self):
        """Increase thresholds are lower/faster than decrease."""
        cfg = ControllerConfig()
        assert cfg.k_hysteresis_up < cfg.k_hysteresis_down
        assert cfg.min_interval_up < cfg.min_interval_down
