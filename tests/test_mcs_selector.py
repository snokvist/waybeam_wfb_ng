"""End-to-end FSM tests for MCSSelector.

Real Scorer + RxAntDatagram payloads, deterministic clock. No mocks.
"""

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.protocol import AntStats, PktStats, RxAntDatagram
from mcs_selector.selector import MCSSelector


def _dgram(rssi, lost=0, fec=0, data=200):
    return RxAntDatagram(
        ts_ms=0, seq=0, interval_ms=100,
        ant=[AntStats(5745, 3, 20, "0", data, rssi - 2, rssi, rssi + 2, 18, 22, 26)],
        pkt=PktStats(
            all_=data + 5, bytes_=data * 1400, dec_err=0, session=0,
            data=data, uniq=data - lost, fec_recovered=fec, lost=lost,
            bad=0, outgoing=data - lost, outgoing_bytes=(data - lost) * 1400,
        ),
    )


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


def _quick_cfg(**kw):
    base = dict(
        range="med",
        rssi_ewma_alpha=1.0,        # no smoothing — tests assert exact values
        loss_ewma_alpha=1.0,
        rssi_thresh_low=-70.0, rssi_thresh_high=-50.0, rssi_deadband_db=2.0,
        up_consecutive=3, down_consecutive=1,
        up_cooldown_s=3.0, down_cooldown_s=0.2,
        failsafe_timeout_s=0.5,
        oscillation_threshold=4, oscillation_window_s=5.0, oscillation_backoff=3.0,
    )
    base.update(kw)
    return MCSSelectorConfig(**base)


class TestInitialCommit:

    def test_first_datagram_emits_immediately(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        d = sel.update(_dgram(-60))
        assert d is not None
        assert d.changed is True
        assert d.reason == "init"
        assert d.bucket == 1
        assert d.mcs_index == 2  # med range[1]


class TestFastDownSlowUp:

    def test_down_takes_one_sample_within_cooldown(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-40))  # init, bucket 2, mcs 3
        clock.advance(1.0)
        # RSSI plummets — should drop to bucket 0 in a single sample.
        d = sel.update(_dgram(-90))
        assert d is not None
        assert d.changed is True
        assert d.reason == "down"
        assert d.bucket == 0
        assert d.mcs_index == 1  # med range[0]

    def test_down_blocked_by_cooldown(self):
        clock = FakeClock()
        cfg = _quick_cfg(down_cooldown_s=1.0)
        sel = MCSSelector(cfg, time_fn=clock)
        sel.update(_dgram(-40))  # init, bucket 2
        clock.advance(0.05)       # too soon
        d = sel.update(_dgram(-90))
        assert d is None
        clock.advance(1.0)
        d = sel.update(_dgram(-90))
        assert d is not None
        assert d.bucket == 0

    def test_up_requires_three_consecutive(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-90))  # init bucket 0
        clock.advance(5.0)        # past up cooldown

        # First good sample — pending streak = 1, no commit.
        assert sel.update(_dgram(-40)) is None
        # Second — still pending.
        assert sel.update(_dgram(-40)) is None
        # Third — commits (skips two buckets in one go to range[2]).
        d = sel.update(_dgram(-40))
        assert d is not None
        assert d.changed is True
        assert d.reason == "up"
        assert d.bucket == 2

    def test_up_streak_resets_on_inconsistent_bucket(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-90))  # bucket 0
        clock.advance(5.0)
        sel.update(_dgram(-40))  # streak 1 toward bucket 2
        sel.update(_dgram(-60))  # different bucket (1) — restart streak
        sel.update(_dgram(-40))  # streak 1 toward bucket 2 again
        d = sel.update(_dgram(-40))  # streak 2 — still no commit
        assert d is None


class TestFailsafe:

    def test_gap_forces_lowest_mcs(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-40))  # bucket 2, mcs 3
        # Simulate 0.6s of no datagrams.
        clock.advance(0.6)
        d = sel.tick_no_data()
        assert d is not None
        assert d.failsafe is True
        assert d.bucket == 0
        assert d.mcs_index == 1
        assert d.changed is True
        assert d.reason == "failsafe"

    def test_no_failsafe_within_timeout(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-40))
        clock.advance(0.4)
        assert sel.tick_no_data() is None

    def test_failsafe_idempotent(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        sel.update(_dgram(-40))
        clock.advance(0.6)
        sel.tick_no_data()
        clock.advance(0.6)
        # Already failsafed at bucket 0 — second tick is a no-op.
        assert sel.tick_no_data() is None

    def test_recovery_requires_consecutive_good_samples(self):
        clock = FakeClock()
        cfg = _quick_cfg(failsafe_recovery_consecutive=3)
        sel = MCSSelector(cfg, time_fn=clock)
        sel.update(_dgram(-40))
        clock.advance(0.6)
        sel.tick_no_data()  # failsafe -> bucket 0

        # First good sample resumes datagram flow but hasn't recovered yet.
        clock.advance(5.0)
        assert sel.update(_dgram(-40)) is None
        assert sel.update(_dgram(-40)) is None
        # Third good sample — recovery completes; falls through to normal
        # logic, which records a streak toward bucket 2 (only 1 sample
        # post-recovery so no commit yet).
        assert sel.update(_dgram(-40)) is None
        # Now build the up streak.
        assert sel.update(_dgram(-40)) is None
        d = sel.update(_dgram(-40))
        assert d is not None
        assert d.bucket == 2
        assert d.failsafe is False

    def test_no_failsafe_before_first_datagram(self):
        clock = FakeClock()
        sel = MCSSelector(_quick_cfg(), time_fn=clock)
        clock.advance(60.0)
        # Never received a datagram — no panic action; service layer
        # is responsible for the boot-time SET.
        assert sel.tick_no_data() is None


class TestLossDrivenDownshift:

    def test_high_loss_at_strong_rssi_drops_bucket(self):
        clock = FakeClock()
        cfg = _quick_cfg(loss_penalty_db_per_pct=0.5, loss_penalty_max_db=20.0)
        sel = MCSSelector(cfg, time_fn=clock)
        sel.update(_dgram(-40))  # init bucket 2
        clock.advance(1.0)
        # RSSI still strong but 30% loss -> 15 dB penalty -> eff -55 dBm
        # -> bucket 1.
        d = sel.update(_dgram(-40, lost=60, fec=0, data=200))
        assert d is not None
        assert d.bucket == 1
        assert d.reason == "down"


class TestOscillationBackoff:

    def test_thrashing_widens_up_cooldown(self):
        clock = FakeClock()
        cfg = _quick_cfg(
            up_cooldown_s=1.0, up_consecutive=1, down_cooldown_s=0.0,
            oscillation_threshold=3, oscillation_window_s=5.0,
            oscillation_backoff=10.0,
        )
        sel = MCSSelector(cfg, time_fn=clock)

        # Drive 3+ updates in a 5s window to trigger oscillation flag.
        sel.update(_dgram(-40))  # init bucket 2
        clock.advance(0.5)
        sel.update(_dgram(-90))  # down to 0
        clock.advance(1.5)
        sel.update(_dgram(-40))  # up to 2
        clock.advance(0.5)
        sel.update(_dgram(-90))  # down to 0
        assert sel.is_oscillating

        # With backoff=10x, normal up cooldown=1s becomes 10s. A 2s wait
        # is now insufficient.
        clock.advance(2.0)
        d = sel.update(_dgram(-40))
        assert d is None

        clock.advance(10.0)
        d = sel.update(_dgram(-40))
        assert d is not None
