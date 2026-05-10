"""Tests for the rolling-window percentile tracker."""

import pytest

from fec_controller.frame_size_percentile import FrameSizePercentile


def test_percentile_returns_fallback_when_undersampled():
    t = [0.0]
    p = FrameSizePercentile(
        window_s=2.5, quantile=0.95, min_samples=8, fallback=1234.0,
        time_fn=lambda: t[0],
    )
    for i in range(3):
        t[0] = i * 0.1
        p.update(5000)
    assert p.value() == 1234.0


def test_percentile_basic_p95():
    t = [0.0]
    p = FrameSizePercentile(
        window_s=10.0, quantile=0.95, min_samples=4,
        time_fn=lambda: t[0],
    )
    for i, size in enumerate([100, 200, 300, 400, 500, 600, 700, 800, 900, 10_000]):
        t[0] = i * 0.1
        p.update(size)
    # P95 with 10 samples: ceil(0.95 * 10) - 1 = index 9 (max) -> 10_000
    assert p.value() == 10_000.0


def test_percentile_expires_old_samples():
    t = [0.0]
    p = FrameSizePercentile(
        window_s=1.0, quantile=1.0, min_samples=1,
        time_fn=lambda: t[0],
    )
    # Write a huge spike at t=0, then small frames for 2s
    p.update(50_000)
    for i in range(1, 20):
        t[0] = i * 0.1
        p.update(1000)
    assert p.value() <= 1000  # spike expired


def test_percentile_picks_median_at_q50():
    t = [0.0]
    p = FrameSizePercentile(
        window_s=10.0, quantile=0.5, min_samples=4,
        time_fn=lambda: t[0],
    )
    for i, size in enumerate([1000, 2000, 3000, 4000, 5000]):
        t[0] = i * 0.1
        p.update(size)
    # ceil(0.5 * 5) - 1 = 2 -> size 3000
    assert p.value() == 3000.0


def test_percentile_invalid_quantile():
    with pytest.raises(ValueError):
        FrameSizePercentile(quantile=0.0)
    with pytest.raises(ValueError):
        FrameSizePercentile(quantile=1.5)


def test_percentile_invalid_window():
    with pytest.raises(ValueError):
        FrameSizePercentile(window_s=0.0)


def test_percentile_p99_absorbs_idr_bursts():
    """IDR every 30 frames at 6x a P-frame: P99 should catch them.
    P95 would not — IDRs are only 3.3 % of samples in the window."""
    t = [0.0]
    p = FrameSizePercentile(
        window_s=2.5, quantile=0.99, min_samples=8,
        time_fn=lambda: t[0],
    )
    fps = 60
    dt = 1.0 / fps
    for i in range(150):
        t[0] = i * dt
        size = 6000 if i % 30 == 0 else 1000
        p.update(size)
    assert p.value() == 6000.0


def test_percentile_p95_misses_sparse_spikes():
    """Companion test to the one above — documents why the default is P99."""
    t = [0.0]
    p = FrameSizePercentile(
        window_s=2.5, quantile=0.95, min_samples=8,
        time_fn=lambda: t[0],
    )
    fps = 60
    dt = 1.0 / fps
    for i in range(150):
        t[0] = i * dt
        size = 6000 if i % 30 == 0 else 1000
        p.update(size)
    # P95 lands on the P-frame band with this distribution
    assert p.value() == 1000.0
