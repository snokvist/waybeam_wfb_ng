"""
Effective-RSSI scorer.

Combines smoothed antenna RSSI with a smoothed loss penalty so that a
strong-signal but lossy link still drives an MCS downshift. ELRS uses
the same idea for its dynamic-power algorithm — link quality can
override raw signal strength.

  effective_rssi = ewma(raw_rssi)
                 - clip(loss_penalty_db_per_pct * 100 * ewma(loss_ratio),
                        0, loss_penalty_max_db)

Loss ratio uses (lost + fec_recovered) / max(1, data) — both signal
that the channel was bad enough that FEC was needed or insufficient.
`dec_err` is excluded because it spikes during off-channel warmup and
isn't strictly a link-quality signal.
"""

from dataclasses import dataclass

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.protocol import RxAntDatagram


@dataclass
class Score:
    raw_rssi: float
    smoothed_rssi: float
    loss_ratio: float
    smoothed_loss_ratio: float
    loss_penalty_db: float
    effective_rssi: float


class Scorer:

    def __init__(self, cfg: MCSSelectorConfig):
        self.cfg = cfg
        self._smoothed_rssi: float | None = None
        self._smoothed_loss: float | None = None

    def reset(self) -> None:
        self._smoothed_rssi = None
        self._smoothed_loss = None

    def _aggregate_rssi(self, d: RxAntDatagram) -> float | None:
        if not d.ant:
            return None
        if self.cfg.rssi_aggregator == "best_avg":
            return float(max(a.rssi_avg for a in d.ant))
        if self.cfg.rssi_aggregator == "best_max":
            return float(max(a.rssi_max for a in d.ant))
        # mean_avg
        return sum(a.rssi_avg for a in d.ant) / len(d.ant)

    def _loss_ratio(self, d: RxAntDatagram) -> float:
        denom = max(1, d.pkt.data)
        return (d.pkt.lost + d.pkt.fec_recovered) / denom

    def update(self, d: RxAntDatagram) -> Score | None:
        """Fold one datagram into the smoothers. Returns None if the
        datagram has no antenna data (warmup) — caller should treat
        that the same as a missed datagram for failsafe purposes."""
        raw = self._aggregate_rssi(d)
        if raw is None:
            return None

        a_r = self.cfg.rssi_ewma_alpha
        if self._smoothed_rssi is None:
            self._smoothed_rssi = raw
        else:
            self._smoothed_rssi = a_r * raw + (1.0 - a_r) * self._smoothed_rssi

        loss = self._loss_ratio(d)
        a_l = self.cfg.loss_ewma_alpha
        if self._smoothed_loss is None:
            self._smoothed_loss = loss
        else:
            self._smoothed_loss = a_l * loss + (1.0 - a_l) * self._smoothed_loss

        penalty = self.cfg.loss_penalty_db_per_pct * 100.0 * self._smoothed_loss
        penalty = max(0.0, min(self.cfg.loss_penalty_max_db, penalty))

        return Score(
            raw_rssi=raw,
            smoothed_rssi=self._smoothed_rssi,
            loss_ratio=loss,
            smoothed_loss_ratio=self._smoothed_loss,
            loss_penalty_db=penalty,
            effective_rssi=self._smoothed_rssi - penalty,
        )
