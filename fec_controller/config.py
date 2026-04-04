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

    # Asymmetric gating — fast increase, slow decrease.
    # Under-protection (k too low) causes frame loss; over-protection
    # (k too high) only wastes bandwidth.  Mirror of TCP AIMD.
    k_hysteresis_up: int = 1          # min k increase to trigger update
    k_hysteresis_down: int = 3        # min k decrease to trigger update
    min_interval_up: float = 0.1      # seconds between FEC increases
    min_interval_down: float = 2.0    # seconds between FEC decreases

    # Peak tracking — sliding window of recent frame sizes (in frames,
    # not time) used as a floor for k computation.  Ensures a single
    # large frame immediately raises k without waiting for EWMA.
    peak_window: int = 32    # >= one GOP at typical settings (30 frames)

    # Oscillation detector — if more than `oscillation_threshold` FEC
    # updates fire within `oscillation_window_s`, the decrease cooldown
    # is multiplied by `oscillation_backoff`.  Auto-resolves when updates
    # slow down.
    oscillation_window_s: float = 5.0
    oscillation_threshold: int = 4
    oscillation_backoff: float = 3.0
