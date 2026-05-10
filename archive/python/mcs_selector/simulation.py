"""
Synthetic rx_ant trace generator for offline testing of the selector FSM.

Drives the real Scorer + MCSSelector with deterministic RSSI / loss
profiles. No mocks; emits real RxAntDatagram objects that round-trip
through the parser via encode_rx_ant() if you want UDP-in-the-loop.
"""

import math
import random
from dataclasses import dataclass

from mcs_selector.protocol import AntStats, PktStats, RxAntDatagram


@dataclass
class TraceConfig:
    duration_s: float = 30.0
    rate_hz: float = 10.0
    base_rssi: float = -60.0
    fade_amplitude_db: float = 25.0
    fade_period_s: float = 12.0
    noise_db: float = 1.5
    base_loss_pct: float = 0.5
    burst_loss_pct: float = 8.0
    burst_at_s: tuple[float, float] | None = (8.0, 11.0)
    seed: int = 42


def generate(cfg: TraceConfig | None = None):
    cfg = cfg or TraceConfig()
    rng = random.Random(cfg.seed)
    period = 1.0 / cfg.rate_hz
    n = int(cfg.duration_s * cfg.rate_hz)
    seq = 0

    for i in range(n):
        t = i * period
        seq += 1

        fade = cfg.fade_amplitude_db * math.sin(2 * math.pi * t / cfg.fade_period_s)
        rssi = cfg.base_rssi + fade + rng.gauss(0.0, cfg.noise_db)

        loss_pct = cfg.base_loss_pct
        if cfg.burst_at_s is not None and cfg.burst_at_s[0] <= t < cfg.burst_at_s[1]:
            loss_pct = cfg.burst_loss_pct

        # Map rssi+loss into the rx_ant schema. We use one antenna entry;
        # tests for multi-antenna aggregators construct datagrams directly.
        data_pkts = 200  # 100ms at ~2000 pps
        lost_pkts = int(data_pkts * loss_pct / 100.0 * 0.4)
        fec_recovered = int(data_pkts * loss_pct / 100.0 * 0.6)

        avg = int(round(rssi))
        ant = [
            AntStats(
                freq=5745, mcs=3, bw=20, id="0",
                pkts=data_pkts,
                rssi_min=avg - 2, rssi_avg=avg, rssi_max=avg + 2,
                snr_min=20, snr_avg=24, snr_max=28,
            )
        ]
        pkt = PktStats(
            all_=data_pkts + 5, bytes_=data_pkts * 1400, dec_err=0,
            session=0, data=data_pkts, uniq=data_pkts - lost_pkts,
            fec_recovered=fec_recovered, lost=lost_pkts, bad=0,
            outgoing=data_pkts - lost_pkts,
            outgoing_bytes=(data_pkts - lost_pkts) * 1400,
        )
        yield t, RxAntDatagram(
            ts_ms=int(t * 1000),
            seq=seq,
            interval_ms=int(period * 1000),
            ant=ant,
            pkt=pkt,
        )
