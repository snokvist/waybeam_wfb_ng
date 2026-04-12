"""
Link-budget estimator — stand-in for mod_aalink in simulation.

Real deployment: mod_aalink (waybeam-hub, vehicle) publishes pps_budget
over an extended sidecar message. For the sim we only need a plausible
signal with:

    - rolling-window smoothing of observed PPS,
    - conservative fallback when no data is fresh,
    - a freshness TTL after which the sample is considered stale.

The estimator is fed by tests and the encoder sim. It is deliberately
boring — no Kalman filter, no EWMA tuning. Any smoothing that's cheap to
explain survives; everything else was cut.
"""

import statistics
import time
from collections import deque
from typing import Callable


class LinkBudgetEstimator:
    """Rolling-median PPS estimator with a freshness TTL."""

    def __init__(
        self,
        window_s: float = 1.0,
        ttl_s: float = 2.0,
        fallback: float = 1500.0,
        time_fn: Callable[[], float] | None = None,
    ) -> None:
        if window_s <= 0:
            raise ValueError(f"window_s must be > 0, got {window_s}")
        if ttl_s <= 0:
            raise ValueError(f"ttl_s must be > 0, got {ttl_s}")
        if fallback <= 0:
            raise ValueError(f"fallback must be > 0, got {fallback}")

        self.window_s = float(window_s)
        self.ttl_s = float(ttl_s)
        self.fallback = float(fallback)
        self._time_fn = time_fn or time.monotonic
        self._samples: deque[tuple[float, float]] = deque()
        self._last_seen: float | None = None

    def observe(self, pps: float) -> None:
        """Record one PPS observation."""
        if pps <= 0:
            return
        now = self._time_fn()
        self._last_seen = now
        self._samples.append((now, float(pps)))
        cutoff = now - self.window_s
        while self._samples and self._samples[0][0] < cutoff:
            self._samples.popleft()

    def is_fresh(self) -> bool:
        if self._last_seen is None:
            return False
        return (self._time_fn() - self._last_seen) <= self.ttl_s

    def current(self) -> float:
        """Current budget. Fallback if no data or stale."""
        if not self.is_fresh() or not self._samples:
            return self.fallback
        # Expire stale samples inside the window before computing
        now = self._time_fn()
        cutoff = now - self.window_s
        fresh = [pps for ts, pps in self._samples if ts >= cutoff]
        if not fresh:
            return self.fallback
        return float(statistics.median(fresh))
