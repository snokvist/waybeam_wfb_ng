"""
FEC controller core - computes optimal k/n from frame statistics.

k is sized for the *average* frame (EWMA + bounded headroom).  I-frames that
exceed k*MTU span multiple FEC blocks; the RTP M-bit honored by wfb_tx closes
each frame's final block cleanly at the frame boundary, so cross-frame
contamination on block loss is impossible regardless of how k compares to any
single frame's packet count.

This trades "one frame ≈ one block" for "airtime efficiency on P-frames": a
peak-sized k (matching I-frame packet count) would pad every P-frame's block
with empty FEC_ONLY fragments.  Sizing for the average keeps P-frames cheap;
the occasional 2-3 block I-frame costs a handful of extra FEC parities once
per GOP.

Uses asymmetric gating (fast increase, slow decrease) because the cost of
under-protection (frame loss) is far higher than over-protection (wasted
bandwidth).
"""

import math
import time
from collections import deque
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

        # Oscillation detector: timestamps of recent FEC updates
        self._update_times: deque[float] = deque()

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
        # Guard: redundancy >= 1.0 causes division by zero
        redundancy = min(redundancy, 0.99)
        n = math.ceil(k / (1.0 - redundancy))
        n = max(self.cfg.min_n, min(self.cfg.max_n, n))
        if n <= k:
            n = k + 1

        # Guard: fps <= 0 causes division by zero
        frame_period_ms = 1000.0 / fps if fps > 0 else 1000.0
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

    @property
    def is_oscillating(self) -> bool:
        """True if recent update rate exceeds the oscillation threshold."""
        return len(self._update_times) >= self.cfg.oscillation_threshold

    def _record_update(self, now: float) -> None:
        """Record an update timestamp and expire old entries."""
        self._update_times.append(now)
        cutoff = now - self.cfg.oscillation_window_s
        while self._update_times and self._update_times[0] < cutoff:
            self._update_times.popleft()

    def update(self, frame_size: int, fps: float) -> FECParams | None:
        """Feed one frame observation. Returns FECParams if an update should be sent."""
        now = self._time_fn()

        self.headroom_tracker.update(float(frame_size))

        # EWMA of frame size
        if self.avg_frame_size is None:
            self.avg_frame_size = float(frame_size)
        else:
            self.avg_frame_size = (
                self.cfg.ewma_alpha * frame_size
                + (1.0 - self.cfg.ewma_alpha) * self.avg_frame_size
            )

        self.current_fps = fps
        headroom = self.headroom_tracker.headroom

        # k sized for average frame (EWMA * bounded headroom).  I-frames that
        # exceed k*MTU span multiple blocks; the M-bit in wfb_tx aligns every
        # frame's final block to a frame boundary, so this is safe.
        candidate = self.compute_params(self.avg_frame_size, fps, headroom)

        # Expire old oscillation records
        self._record_update(now)
        # Remove the speculative entry — only keep it if we actually emit
        self._update_times.pop()

        # First update always emitted
        if self.current_params is None:
            self.current_params = candidate
            self.last_update_time = now
            self.update_count += 1
            self._update_times.append(now)
            return candidate

        k_delta = candidate.k - self.current_params.k
        elapsed = now - self.last_update_time

        # Effective decrease cooldown: widened during oscillation
        effective_interval_down = self.cfg.min_interval_down
        if self.is_oscillating:
            effective_interval_down *= self.cfg.oscillation_backoff

        if k_delta > 0:
            # INCREASE path: fast response, low bar
            if k_delta < self.cfg.k_hysteresis_up:
                return None
            if elapsed < self.cfg.min_interval_up:
                return None
        elif k_delta < 0:
            # DECREASE path: slow response, high bar
            if abs(k_delta) < self.cfg.k_hysteresis_down:
                return None
            if elapsed < effective_interval_down:
                return None
        else:
            # No change in k
            return None

        self.current_params = candidate
        self.last_update_time = now
        self.update_count += 1
        self._update_times.append(now)
        return candidate

    def get_current(self) -> FECParams | None:
        return self.current_params
