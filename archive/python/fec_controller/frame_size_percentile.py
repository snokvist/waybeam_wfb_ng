"""
Percentile-tracking frame-size reference (S_ref).

The legacy HeadroomTracker (fec_controller/headroom.py) learns a max/avg
ratio clamped to [1.05, 1.40]. For IDR absorption in the variable-payload
sizer that clamp is too tight: IDR frames are often 5-8x a P-frame, so
1.40 under-sizes P at every GOP boundary.

This module tracks a configurable quantile (default P95) over a rolling
time window of recent frame sizes. Feeding the quantile to the sizer as
S_ref biases payload upward enough that a typical IDR lands in one FEC
block, while outliers beyond the quantile are allowed to spill to two
blocks by design.

Deterministic, no I/O. Time source is injected so tests and the sim can
drive it from a virtual clock.
"""

import bisect
import time
from collections import deque
from typing import Callable


class FrameSizePercentile:
    """Rolling-window percentile estimator over recent frame sizes."""

    def __init__(
        self,
        window_s: float = 2.5,
        quantile: float = 0.95,
        min_samples: int = 8,
        fallback: float = 0.0,
        time_fn: Callable[[], float] | None = None,
    ) -> None:
        if not (0.0 < quantile <= 1.0):
            raise ValueError(f"quantile must be in (0, 1], got {quantile}")
        if window_s <= 0:
            raise ValueError(f"window_s must be > 0, got {window_s}")
        if min_samples < 1:
            raise ValueError(f"min_samples must be >= 1, got {min_samples}")

        self.window_s = float(window_s)
        self.quantile = float(quantile)
        self.min_samples = int(min_samples)
        self.fallback = float(fallback)
        self._time_fn = time_fn or time.monotonic

        # (timestamp, size) pairs in arrival order
        self._samples: deque[tuple[float, int]] = deque()
        # Parallel sorted list of sizes for O(log n) quantile lookup
        self._sorted_sizes: list[int] = []

    @property
    def sample_count(self) -> int:
        return len(self._samples)

    def update(self, size: int) -> None:
        """Record a new frame size and expire samples outside the window."""
        now = self._time_fn()
        cutoff = now - self.window_s

        # Expire old samples from the head
        while self._samples and self._samples[0][0] < cutoff:
            _, old_size = self._samples.popleft()
            idx = bisect.bisect_left(self._sorted_sizes, old_size)
            if idx < len(self._sorted_sizes) and self._sorted_sizes[idx] == old_size:
                self._sorted_sizes.pop(idx)

        self._samples.append((now, size))
        bisect.insort(self._sorted_sizes, size)

    def value(self) -> float:
        """Return the current quantile, or the fallback if under-sampled.

        For quantile=1.0 this returns the max; for 0.5 it is the median.
        Uses the "lower" interpolation convention (pick the nearest lower
        index); deterministic and matches common percentile definitions.
        """
        n = len(self._sorted_sizes)
        if n < self.min_samples:
            return self.fallback
        # Index of the k-th order statistic; clamp so quantile=1.0 picks n-1
        idx = min(n - 1, max(0, int(self.quantile * n)))
        # For quantile < 1.0 with small n, bias toward the upper side
        # so P95 of 10 samples picks the largest sample rather than #9.
        # Use ceil-based index: idx = ceil(q * n) - 1
        import math
        idx = max(0, min(n - 1, math.ceil(self.quantile * n) - 1))
        return float(self._sorted_sizes[idx])
