"""Tests for ranges.bucket_from_rssi — deadband-aware threshold crossing."""

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.ranges import bucket_from_rssi, mcs_for_bucket


def _cfg(**kw):
    base = dict(rssi_thresh_low=-70.0, rssi_thresh_high=-50.0, rssi_deadband_db=2.0)
    base.update(kw)
    return MCSSelectorConfig(**base)


class TestBucketFresh:
    """current_bucket=None — pure threshold lookup, no deadband."""

    def test_below_low(self):
        assert bucket_from_rssi(-80.0, None, _cfg()) == 0

    def test_between(self):
        assert bucket_from_rssi(-60.0, None, _cfg()) == 1

    def test_at_low_boundary_goes_up(self):
        # rssi >= rssi_thresh_low -> bucket 1 per spec
        assert bucket_from_rssi(-70.0, None, _cfg()) == 1

    def test_above_high(self):
        assert bucket_from_rssi(-40.0, None, _cfg()) == 2

    def test_at_high_boundary(self):
        assert bucket_from_rssi(-50.0, None, _cfg()) == 2


class TestBucketDeadband:
    """current_bucket set — deadband makes transitions sticky."""

    def test_stays_in_0_within_deadband(self):
        # Sitting at bucket 0; -69 is above -70 but within +2dB deadband.
        assert bucket_from_rssi(-69.0, 0, _cfg()) == 0
        # -68.0 = lo + db => clears the deadband, climb permitted.
        assert bucket_from_rssi(-68.0, 0, _cfg()) == 1

    def test_stays_in_1_grazing_low(self):
        # In bucket 1, RSSI grazes the low threshold from above; needs to
        # drop below -72 to fall to 0.
        assert bucket_from_rssi(-71.0, 1, _cfg()) == 1
        assert bucket_from_rssi(-72.0, 1, _cfg()) == 1
        assert bucket_from_rssi(-72.5, 1, _cfg()) == 0

    def test_stays_in_1_grazing_high(self):
        # Need +2dB above -50 to climb to bucket 2.
        assert bucket_from_rssi(-49.0, 1, _cfg()) == 1
        assert bucket_from_rssi(-48.0, 1, _cfg()) == 2

    def test_stays_in_2_within_deadband(self):
        # Bucket 2; -51 is below -50 but inside the deadband.
        assert bucket_from_rssi(-51.0, 2, _cfg()) == 2
        assert bucket_from_rssi(-52.0, 2, _cfg()) == 2
        assert bucket_from_rssi(-52.5, 2, _cfg()) == 1

    def test_skip_bucket_on_huge_jump(self):
        # Going from 0 to 2 in one step is allowed when RSSI clears
        # both deadband-shifted thresholds.
        assert bucket_from_rssi(-40.0, 0, _cfg()) == 2
        assert bucket_from_rssi(-90.0, 2, _cfg()) == 0


class TestMcsForBucket:

    def test_low_range(self):
        cfg = _cfg(range="low")
        assert (mcs_for_bucket(0, cfg), mcs_for_bucket(1, cfg), mcs_for_bucket(2, cfg)) == (0, 1, 2)

    def test_med_range(self):
        cfg = _cfg(range="med")
        assert (mcs_for_bucket(0, cfg), mcs_for_bucket(1, cfg), mcs_for_bucket(2, cfg)) == (1, 2, 3)

    def test_high_range(self):
        cfg = _cfg(range="high")
        assert (mcs_for_bucket(0, cfg), mcs_for_bucket(1, cfg), mcs_for_bucket(2, cfg)) == (2, 3, 4)

    def test_clamp_to_mcs_max(self):
        cfg = _cfg(range="high", mcs_max=3)
        assert mcs_for_bucket(2, cfg) == 3
