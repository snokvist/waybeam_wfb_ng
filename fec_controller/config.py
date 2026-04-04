"""FEC controller configuration."""

from dataclasses import dataclass, field


@dataclass
class ControllerConfig:
    mtu: int = 1446

    ewma_alpha: float = 0.05

    # Learned headroom parameters
    headroom_window_s: float = 2.5
    headroom_min: float = 1.05
    headroom_max: float = 1.40
    headroom_margin: float = 1.05

    # k/n bounds
    min_k: int = 1
    max_k: int = 48
    min_n: int = 2
    max_n: int = 72

    # (k, redundancy_fraction) control points - linearly interpolated
    redundancy_curve: list = field(default_factory=lambda: [
        (1, 0.50),
        (4, 0.40),
        (8, 0.33),
        (16, 0.30),
        (32, 0.27),
        (48, 0.25),
    ])

    # fec_timeout = frame_period_ms * timeout_fraction
    timeout_fraction: float = 0.5

    # Update gating
    k_hysteresis: int = 2          # k must change by >= this to trigger update
    min_update_interval: float = 0.5  # seconds between FEC updates
