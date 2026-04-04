"""
FEC controller core - computes optimal k/n from frame statistics.

One video frame ~ one FEC block: k is sized so a full frame fits in one block.
This avoids latency from a frame spanning multiple blocks where a partial last
block stalls waiting for the next frame's packets.
"""

import math
import time
from dataclasses import dataclass
from typing import Callable

from fec_controller.config import ControllerConfig
from fec_controller.headroom import HeadroomTracker


@dataclass
class FECParams:
    k: int
    n: int
    fec_timeout_ms: int
    redundancy: float
    packets_per_frame: int
    avg_frame_size: float
    headroom: float


class FECController:

    def __init__(
        self,
        config: ControllerConfig | None = None,
        time_fn: Callable[[], float] | None = None,
    ):
        self.cfg = config or ControllerConfig()
        self._time_fn = time_fn or time.monotonic

        self.avg_frame_size: float | None = None
        self.current_fps: float | None = None
        self.current_params: FECParams | None = None
        self.last_update_time: float = -999.0
        self.update_count: int = 0

        self.headroom_tracker = HeadroomTracker(
            window_s=self.cfg.headroom_window_s,
            margin=self.cfg.headroom_margin,
            floor=self.cfg.headroom_min,
            ceiling=self.cfg.headroom_max,
            time_fn=self._time_fn,
        )
        self.cfg.redundancy_curve.sort(key=lambda x: x[0])

    def _interpolate_redundancy(self, k: int) -> float:
        curve = self.cfg.redundancy_curve
        if k <= curve[0][0]:
            return curve[0][1]
        if k >= curve[-1][0]:
            return curve[-1][1]
        for i in range(len(curve) - 1):
            k0, r0 = curve[i]
            k1, r1 = curve[i + 1]
            if k0 <= k <= k1:
                t = (k - k0) / (k1 - k0)
                return r0 + t * (r1 - r0)
        return curve[-1][1]

    def compute_params(
        self, avg_frame_size: float, fps: float, headroom: float
    ) -> FECParams:
        target_size = avg_frame_size * headroom
        packets_per_frame = max(1, math.ceil(target_size / self.cfg.mtu))
        k = max(self.cfg.min_k, min(self.cfg.max_k, packets_per_frame))

        redundancy = self._interpolate_redundancy(k)
        n = math.ceil(k / (1.0 - redundancy))
        n = max(self.cfg.min_n, min(self.cfg.max_n, n))
        if n <= k:
            n = k + 1

        frame_period_ms = 1000.0 / fps
        fec_timeout_ms = max(1, int(frame_period_ms * self.cfg.timeout_fraction))

        return FECParams(
            k=k,
            n=n,
            fec_timeout_ms=fec_timeout_ms,
            redundancy=redundancy,
            packets_per_frame=packets_per_frame,
            avg_frame_size=avg_frame_size,
            headroom=headroom,
        )

    def update(self, frame_size: int, fps: float) -> FECParams | None:
        """Feed one frame observation. Returns FECParams if an update should be sent."""
        now = self._time_fn()

        self.headroom_tracker.update(float(frame_size))

        if self.avg_frame_size is None:
            self.avg_frame_size = float(frame_size)
        else:
            self.avg_frame_size = (
                self.cfg.ewma_alpha * frame_size
                + (1.0 - self.cfg.ewma_alpha) * self.avg_frame_size
            )

        self.current_fps = fps
        headroom = self.headroom_tracker.headroom
        candidate = self.compute_params(self.avg_frame_size, fps, headroom)

        # First update always emitted
        if self.current_params is None:
            self.current_params = candidate
            self.last_update_time = now
            self.update_count += 1
            return candidate

        # Hysteresis: k must change by >= threshold
        k_delta = abs(candidate.k - self.current_params.k)
        if k_delta < self.cfg.k_hysteresis:
            return None

        # Rate limiting
        if now - self.last_update_time < self.cfg.min_update_interval:
            return None

        self.current_params = candidate
        self.last_update_time = now
        self.update_count += 1
        return candidate

    def get_current(self) -> FECParams | None:
        return self.current_params
