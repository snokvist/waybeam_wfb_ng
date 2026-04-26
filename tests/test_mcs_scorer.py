"""Tests for the loss-aware Scorer."""

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.protocol import AntStats, PktStats, RxAntDatagram
from mcs_selector.scorer import Scorer


def _dgram(rssi_per_ant, lost=0, fec=0, data=200):
    return RxAntDatagram(
        ts_ms=0, seq=0, interval_ms=100,
        ant=[
            AntStats(5745, 3, 20, str(i), data, r - 2, r, r + 2, 18, 22, 26)
            for i, r in enumerate(rssi_per_ant)
        ],
        pkt=PktStats(
            all_=data + 5, bytes_=data * 1400, dec_err=0, session=0,
            data=data, uniq=data - lost,
            fec_recovered=fec, lost=lost, bad=0,
            outgoing=data - lost, outgoing_bytes=(data - lost) * 1400,
        ),
    )


class TestRSSIAggregator:

    def test_best_avg_picks_strongest(self):
        cfg = MCSSelectorConfig(rssi_aggregator="best_avg", rssi_ewma_alpha=1.0,
                                loss_ewma_alpha=1.0)
        s = Scorer(cfg)
        score = s.update(_dgram([-70, -50, -60]))
        assert score.raw_rssi == -50.0

    def test_mean_avg(self):
        cfg = MCSSelectorConfig(rssi_aggregator="mean_avg", rssi_ewma_alpha=1.0,
                                loss_ewma_alpha=1.0)
        s = Scorer(cfg)
        score = s.update(_dgram([-70, -50, -60]))
        assert score.raw_rssi == -60.0

    def test_empty_ant_returns_none(self):
        cfg = MCSSelectorConfig()
        s = Scorer(cfg)
        d = _dgram([])
        assert s.update(d) is None


class TestEWMA:

    def test_first_sample_seeds_ewma(self):
        cfg = MCSSelectorConfig(rssi_ewma_alpha=0.3, loss_ewma_alpha=0.5)
        s = Scorer(cfg)
        score = s.update(_dgram([-60]))
        assert score.smoothed_rssi == -60.0
        assert score.smoothed_loss_ratio == 0.0

    def test_smoothing_across_samples(self):
        cfg = MCSSelectorConfig(rssi_ewma_alpha=0.5, loss_ewma_alpha=1.0,
                                loss_penalty_db_per_pct=0)
        s = Scorer(cfg)
        s.update(_dgram([-60]))
        score = s.update(_dgram([-40]))
        # 0.5 * -40 + 0.5 * -60 = -50
        assert score.smoothed_rssi == -50.0


class TestLossPenalty:

    def test_clean_link_no_penalty(self):
        cfg = MCSSelectorConfig(rssi_ewma_alpha=1.0, loss_ewma_alpha=1.0)
        s = Scorer(cfg)
        score = s.update(_dgram([-50], lost=0, fec=0, data=200))
        assert score.loss_penalty_db == 0.0
        assert score.effective_rssi == -50.0

    def test_loss_drives_penalty(self):
        # 10% loss -> 0.5 * 10 = 5 dB penalty.
        cfg = MCSSelectorConfig(
            rssi_ewma_alpha=1.0, loss_ewma_alpha=1.0,
            loss_penalty_db_per_pct=0.5, loss_penalty_max_db=20.0,
        )
        s = Scorer(cfg)
        score = s.update(_dgram([-50], lost=20, fec=0, data=200))
        assert score.loss_ratio == 0.10
        assert score.loss_penalty_db == 5.0
        assert score.effective_rssi == -55.0

    def test_fec_recovered_counts_too(self):
        # fec_recovered should add to the stress signal — still lossy
        # channel even if FEC saved the day.
        cfg = MCSSelectorConfig(
            rssi_ewma_alpha=1.0, loss_ewma_alpha=1.0,
            loss_penalty_db_per_pct=0.5,
        )
        s = Scorer(cfg)
        score = s.update(_dgram([-50], lost=0, fec=20, data=200))
        assert score.loss_ratio == 0.10
        assert score.loss_penalty_db == 5.0

    def test_penalty_is_capped(self):
        cfg = MCSSelectorConfig(
            rssi_ewma_alpha=1.0, loss_ewma_alpha=1.0,
            loss_penalty_db_per_pct=1.0, loss_penalty_max_db=8.0,
        )
        s = Scorer(cfg)
        score = s.update(_dgram([-50], lost=100, fec=50, data=200))
        # raw = 75% loss * 1 = 75 dB, capped at 8.
        assert score.loss_penalty_db == 8.0
