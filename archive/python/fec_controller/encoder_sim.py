"""
Encoder simulator — stands in for waybeam_venc's frame generation and
packetisation behaviour.

For the variable-payload feature we need to observe the whole chain:

    encoder -> (choose_payload_size) -> packetiser -> FEC block -> wire

The real encoder picks P from config; here P comes from the sizer. The
encoder sim is otherwise behavioural-only: it emits frame sizes and the
packetiser chops each frame into ceil(size / P) equal-ish parts, with
the last packet carrying the remainder.

Deterministic under a seeded RNG. No I/O.
"""

import math
import random
from dataclasses import dataclass, field


@dataclass
class SizeProfile:
    """Configurable frame-size distribution."""
    base: int = 5000
    i_mult: float = 5.0           # IDR tends to be ~5-8x a P-frame
    gop_interval: int = 30
    jitter_sigma: float = 0.03    # std-dev of multiplicative gaussian jitter
    # (time_s, new_base) events that shift the base at given simulated times
    bitrate_events: list[tuple[float, int]] = field(default_factory=list)


@dataclass
class EncodedFrame:
    frame_id: int
    time_s: float
    size_bytes: int
    is_idr: bool
    packets: list[int]            # packet sizes in emit order
    payload: int                  # P used to packetise this frame


class EncoderSim:
    """Generate encoded frames and packetise them at a caller-supplied P."""

    def __init__(
        self,
        fps: int = 120,
        profile: SizeProfile | None = None,
        seed: int = 0xC0FFEE,
    ) -> None:
        if fps <= 0:
            raise ValueError(f"fps must be > 0, got {fps}")
        self.fps = int(fps)
        self.profile = profile or SizeProfile()
        self.rng = random.Random(seed)
        self._frame_idx = 0
        self._current_base = self.profile.base
        self._event_idx = 0

    @property
    def frame_period_s(self) -> float:
        return 1.0 / self.fps

    def _apply_events(self, t: float) -> None:
        evs = self.profile.bitrate_events
        while self._event_idx < len(evs) and t >= evs[self._event_idx][0]:
            self._current_base = evs[self._event_idx][1]
            self._event_idx += 1

    def step(self, payload: int) -> EncodedFrame:
        """Emit the next frame, packetised at the given payload size."""
        if payload < 1:
            raise ValueError(f"payload must be >= 1, got {payload}")

        t = self._frame_idx * self.frame_period_s
        self._apply_events(t)

        is_idr = (self._frame_idx % self.profile.gop_interval) == 0
        mult = self.profile.i_mult if is_idr else 1.0
        jitter = max(0.1, self.rng.gauss(1.0, self.profile.jitter_sigma))
        size = max(1, int(self._current_base * mult * jitter))

        n_packets = math.ceil(size / payload)
        # Even split, remainder goes to the final packet
        packets: list[int] = []
        remaining = size
        for _ in range(n_packets - 1):
            packets.append(payload)
            remaining -= payload
        if remaining > 0:
            packets.append(remaining)
        # Tiny frames: a single packet below min_payload would still get
        # padded to min_payload by the RS layer anyway; we keep the raw
        # size here so the benchmark sees the real bytes-on-wire calc.

        frame = EncodedFrame(
            frame_id=self._frame_idx,
            time_s=t,
            size_bytes=size,
            is_idr=is_idr,
            packets=packets,
            payload=int(payload),
        )
        self._frame_idx += 1
        return frame
