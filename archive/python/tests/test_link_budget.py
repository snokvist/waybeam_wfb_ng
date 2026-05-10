"""Tests for the stub link-budget estimator."""

import pytest

from fec_controller.link_budget import LinkBudgetEstimator


def test_fallback_when_no_data():
    t = [0.0]
    est = LinkBudgetEstimator(fallback=1500.0, time_fn=lambda: t[0])
    assert est.current() == 1500.0
    assert not est.is_fresh()


def test_observe_then_current():
    t = [0.0]
    est = LinkBudgetEstimator(
        window_s=1.0, ttl_s=2.0, fallback=1500.0, time_fn=lambda: t[0],
    )
    est.observe(2800)
    assert est.is_fresh()
    assert est.current() == 2800.0


def test_median_over_window():
    t = [0.0]
    est = LinkBudgetEstimator(
        window_s=1.0, ttl_s=2.0, fallback=1500.0, time_fn=lambda: t[0],
    )
    for i, pps in enumerate([2000, 2200, 2100, 2300, 2500]):
        t[0] = i * 0.1
        est.observe(pps)
    assert est.current() == 2200.0  # median of 5


def test_stale_falls_back():
    t = [0.0]
    est = LinkBudgetEstimator(
        window_s=0.5, ttl_s=0.5, fallback=1500.0, time_fn=lambda: t[0],
    )
    est.observe(3000)
    t[0] = 1.0  # past TTL
    assert not est.is_fresh()
    assert est.current() == 1500.0


def test_window_expires_old_samples():
    t = [0.0]
    est = LinkBudgetEstimator(
        window_s=1.0, ttl_s=5.0, fallback=1500.0, time_fn=lambda: t[0],
    )
    # Big PPS at t=0, then a sustained lower level
    est.observe(6000)
    for i in range(1, 20):
        t[0] = i * 0.1
        est.observe(2000)
    # After t=2.0 the 6000 spike has dropped out of the 1s window
    assert est.current() == 2000.0


def test_ignores_zero_and_negative():
    t = [0.0]
    est = LinkBudgetEstimator(fallback=1500.0, time_fn=lambda: t[0])
    est.observe(0)
    est.observe(-100)
    assert not est.is_fresh()
    assert est.current() == 1500.0


def test_rejects_invalid_init():
    with pytest.raises(ValueError):
        LinkBudgetEstimator(window_s=0)
    with pytest.raises(ValueError):
        LinkBudgetEstimator(ttl_s=0)
    with pytest.raises(ValueError):
        LinkBudgetEstimator(fallback=0)
