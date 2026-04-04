"""
Headroom tracker - learns I/P frame size variance from actual data.

Tracks max/avg frame size ratio over a rolling time window to adapt
to encoder behavior without needing to know GOP structure.
"""

import time
from collections import deque
from typing import Callable


class HeadroomTracker:

    def __init__(
        self,
        window_s: float = 2.5,
        margin: float = 1.05,
        floor: float = 1.05,
        ceiling: float = 1.40,
        time_fn: Callable[[], float] | None = None,
    ):
        self.window_s = window_s
        self.margin = margin
        self.floor = floor
        self.ceiling = ceiling
        self._time_fn = time_fn or time.monotonic
        self._samples: deque[tuple[float, float]] = deque()
        self._avg: float | None = None
        self._alpha = 0.05

    def update(self, frame_size: float) -> None:
        now = self._time_fn()
        self._samples.append((now, frame_size))

        if self._avg is None:
            self._avg = frame_size
        else:
            self._avg = self._alpha * frame_size + (1.0 - self._alpha) * self._avg

        cutoff = now - self.window_s
        while self._samples and self._samples[0][0] < cutoff:
            self._samples.popleft()

    @property
    def headroom(self) -> float:
        if not self._samples or not self._avg or self._avg < 1:
            return self.floor
        max_in_window = max(s[1] for s in self._samples)
        ratio = (max_in_window / self._avg) * self.margin
        return max(self.floor, min(self.ceiling, ratio))
