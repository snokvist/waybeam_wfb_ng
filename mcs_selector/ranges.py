"""
RSSI bucket -> MCS index mapping for the active 3-MCS range.

Three buckets are derived from a deadband-aware crossing of two thresholds.
Bucket numbers are 0/1/2 corresponding to range[0], range[1], range[2].
Going up requires clearing the threshold by +deadband; going down, dropping
below by -deadband. Without that, an RSSI hovering exactly on a threshold
flaps every sample.
"""

from mcs_selector.config import MCSSelectorConfig


def bucket_from_rssi(
    rssi: float,
    current_bucket: int | None,
    cfg: MCSSelectorConfig,
) -> int:
    """Pick a bucket (0/1/2) for a given effective RSSI.

    `current_bucket` is the bucket the selector last committed to; the
    deadband is applied relative to that to make sticky transitions.
    `None` means "no commitment yet" — pure threshold lookup.
    """
    lo = cfg.rssi_thresh_low
    hi = cfg.rssi_thresh_high
    db = cfg.rssi_deadband_db

    if current_bucket is None:
        if rssi < lo:
            return 0
        if rssi < hi:
            return 1
        return 2

    if current_bucket == 0:
        # Need to climb above lo by +db to leave bucket 0.
        if rssi >= lo + db:
            return 2 if rssi >= hi + db else 1
        return 0

    if current_bucket == 1:
        # Drop below lo - db to fall out the bottom; climb above hi + db
        # to leap to the top. Otherwise stay.
        if rssi < lo - db:
            return 0
        if rssi >= hi + db:
            return 2
        return 1

    # current_bucket == 2
    if rssi < hi - db:
        return 0 if rssi < lo - db else 1
    return 2


def mcs_for_bucket(bucket: int, cfg: MCSSelectorConfig) -> int:
    """Map a bucket index to an MCS index using the active range preset.
    Result is clamped to [cfg.mcs_min, cfg.mcs_max]."""
    rng = cfg.selected_range()
    bucket = max(0, min(2, bucket))
    mcs = rng[bucket]
    return max(cfg.mcs_min, min(cfg.mcs_max, mcs))
