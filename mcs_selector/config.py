"""MCS selector configuration."""

from dataclasses import dataclass, field


# Range presets — operator picks one externally; selector never auto-switches
# between them. RSSI buckets within each map to range[0], range[1], range[2].
RANGE_PRESETS: dict[str, tuple[int, int, int]] = {
    "low":  (0, 1, 2),
    "med":  (1, 2, 3),
    "high": (2, 3, 4),
}


@dataclass
class MCSSelectorConfig:

    # ---- Range selection -------------------------------------------------

    range: str = "med"  # one of RANGE_PRESETS

    # RSSI thresholds (dBm) defining the three buckets within a range:
    #   rssi <  rssi_thresh_low                            -> range[0]
    #   rssi_thresh_low <= rssi < rssi_thresh_high         -> range[1]
    #   rssi >= rssi_thresh_high                           -> range[2]
    rssi_thresh_low: float = -70.0
    rssi_thresh_high: float = -50.0

    # Crossing deadband (dB). Going UP, you need to clear the threshold by
    # +deadband; going DOWN, you need to drop below it by -deadband. Stops
    # threshold-grazing oscillation. Applies symmetrically to both
    # thresholds.
    rssi_deadband_db: float = 2.0

    # ---- Smoothing -------------------------------------------------------

    # EWMA alpha for RSSI input. 0.3 ≈ 3-sample memory at 10 Hz, fast enough
    # to react to fades but smooth enough that a single noisy interval
    # doesn't trigger.
    rssi_ewma_alpha: float = 0.3

    # EWMA alpha for loss ratio. Loss reacts faster than RSSI — a sudden
    # spike in `lost` should land in the score within ~1 interval.
    loss_ewma_alpha: float = 0.5

    # RSSI aggregator across multi-antenna `ant[]`. One of:
    #   "best_avg" — max(ant[].rssi.avg)  (default; best receiver this interval)
    #   "best_max" — max(ant[].rssi.max)  (most optimistic)
    #   "mean_avg" — mean(ant[].rssi.avg) (smoother, ignores diversity gain)
    rssi_aggregator: str = "best_avg"

    # ---- Loss penalty (lost + fec_recovered) ----------------------------

    # Penalty in dB per percentage point of (lost + fec_recovered) / data.
    # Default 0.5 dB/% means 10% loss subtracts 5 dB from effective RSSI;
    # heavy stress (>40%) saturates at the cap below.
    loss_penalty_db_per_pct: float = 0.5
    loss_penalty_max_db: float = 20.0

    # ---- Asymmetric gating (fast drop, slow climb) -----------------------

    # Hysteresis (consecutive samples in a different bucket required to
    # commit the change). Bad links stay bad — be eager to drop, careful
    # to climb.
    up_consecutive: int = 3
    down_consecutive: int = 1

    # Cooldowns (seconds since the last MCS change before another is
    # allowed in that direction).
    up_cooldown_s: float = 3.0
    down_cooldown_s: float = 0.2

    # ---- Failsafe --------------------------------------------------------

    # Watchdog gap. If no rx_ant datagram for this long, force range[0]
    # immediately and freeze until recovery.
    failsafe_timeout_s: float = 0.5

    # After a failsafe trip, require this many consecutive normal samples
    # before normal selection logic resumes.
    failsafe_recovery_consecutive: int = 3

    # ---- Oscillation detector --------------------------------------------

    # Mirrors fec_controller pattern: if more than `oscillation_threshold`
    # MCS changes fire within `oscillation_window_s`, the up cooldown is
    # multiplied by `oscillation_backoff` until updates calm.
    oscillation_window_s: float = 5.0
    oscillation_threshold: int = 4
    oscillation_backoff: float = 3.0

    # ---- Wire defaults ---------------------------------------------------

    # Listener for wfb_rx -Y JSON datagrams (selector binds here).
    stats_host: str = "127.0.0.1"
    stats_port: int = 5801

    # wfb_tx control endpoint (selector connects here for SET/GET RADIO).
    tx_host: str = "127.0.0.1"
    tx_port: int = 8000

    # wfb_tx control response timeout (used for CMD_GET_RADIO sync).
    control_response_timeout_s: float = 0.5

    # Don't send CMD_SET_RADIO updates (compute-only). Useful for
    # observability / dry-run validation against a live link.
    dry_run: bool = False

    # Bounds clamp on emitted mcs_index. Defence in depth against bad
    # range presets or a config that nominates an unsupported MCS.
    mcs_min: int = 0
    mcs_max: int = 11

    def selected_range(self) -> tuple[int, int, int]:
        if self.range not in RANGE_PRESETS:
            raise ValueError(
                f"unknown range {self.range!r}; expected one of {sorted(RANGE_PRESETS)}"
            )
        return RANGE_PRESETS[self.range]

    def __post_init__(self) -> None:
        if self.rssi_thresh_low >= self.rssi_thresh_high:
            raise ValueError(
                f"rssi_thresh_low ({self.rssi_thresh_low}) must be strictly less "
                f"than rssi_thresh_high ({self.rssi_thresh_high})"
            )
        if self.rssi_aggregator not in ("best_avg", "best_max", "mean_avg"):
            raise ValueError(f"unknown rssi_aggregator: {self.rssi_aggregator}")
        if self.range not in RANGE_PRESETS:
            raise ValueError(f"unknown range {self.range!r}")
