"""
Variable-payload benchmark — run two policies over the same trace and
compare wire cost, packet count, one-block hit rate, and P volatility.

The benchmark glues together:
    - EncoderSim (frame generation + packetisation)
    - LinkBudgetEstimator (hand-fed pps_budget samples)
    - FrameSizePercentile (S_ref for the variable policy)
    - choose_payload_size (the policy under test)
    - make_block / pack_frame_into_blocks (wire cost)

Two policies:
    FIXED: P = mtu_override (status quo; matches current FEC controller).
    VARIABLE: P = choose_payload_size(...).

Both run over the same EncoderSim output, so any difference in totals
attributes to the payload policy alone.
"""

from dataclasses import dataclass, field

from fec_controller.block_model import pack_frame_into_blocks
from fec_controller.encoder_sim import EncodedFrame, EncoderSim, SizeProfile
from fec_controller.frame_size_percentile import FrameSizePercentile
from fec_controller.link_budget import LinkBudgetEstimator
from fec_controller.payload_sizer import (
    DEFAULT_HYSTERESIS,
    DEFAULT_MIN_PAYLOAD,
    DEFAULT_MTU_OVERRIDE,
    choose_payload_size,
)


@dataclass
class PolicyStats:
    name: str
    frames: int = 0
    total_packets: int = 0          # sum of n over all blocks (wire packets)
    total_source_packets: int = 0   # sum of k over all blocks
    total_wire_bytes: int = 0
    total_source_bytes: int = 0
    total_padding_bytes: int = 0
    total_recovery_bytes: int = 0
    frames_in_one_block: int = 0
    frames_spilled: int = 0
    payload_values: list[int] = field(default_factory=list)
    k_values: list[int] = field(default_factory=list)

    # Frame-by-frame details for debugging / plotting
    per_frame: list[dict] = field(default_factory=list)

    def record(
        self,
        frame: EncodedFrame,
        payload: int,
        blocks: list,
    ) -> None:
        self.frames += 1
        self.payload_values.append(payload)
        self.k_values.extend(b.k for b in blocks)
        fits = len(blocks) == 1
        if fits:
            self.frames_in_one_block += 1
        else:
            self.frames_spilled += 1
        for b in blocks:
            self.total_packets += b.packets_in_block
            self.total_source_packets += b.k
            self.total_wire_bytes += b.wire_bytes
            self.total_source_bytes += b.source_bytes
            self.total_padding_bytes += b.source_padding
            self.total_recovery_bytes += b.recovery_bytes
        self.per_frame.append({
            "frame_id": frame.frame_id,
            "time_s": frame.time_s,
            "size": frame.size_bytes,
            "is_idr": frame.is_idr,
            "payload": payload,
            "blocks": len(blocks),
            "packets": sum(b.packets_in_block for b in blocks),
            "wire_bytes": sum(b.wire_bytes for b in blocks),
        })

    def summary(self) -> dict:
        one_block_rate = (
            self.frames_in_one_block / self.frames if self.frames else 0.0
        )
        p_volatility = _volatility(self.payload_values)
        k_volatility = _volatility(self.k_values)
        return {
            "name": self.name,
            "frames": self.frames,
            "total_packets": self.total_packets,
            "total_source_packets": self.total_source_packets,
            "total_wire_bytes": self.total_wire_bytes,
            "total_source_bytes": self.total_source_bytes,
            "total_padding_bytes": self.total_padding_bytes,
            "total_recovery_bytes": self.total_recovery_bytes,
            "one_block_hit_rate": one_block_rate,
            "frames_spilled": self.frames_spilled,
            "payload_volatility": p_volatility,
            "k_volatility": k_volatility,
            "avg_payload": _mean(self.payload_values),
            "avg_k": _mean(self.k_values),
        }


def _mean(xs: list[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def _volatility(xs: list) -> float:
    """Mean absolute first-difference — a cheap, interpretable flap metric."""
    if len(xs) < 2:
        return 0.0
    diffs = [abs(xs[i] - xs[i - 1]) for i in range(1, len(xs))]
    return sum(diffs) / len(diffs)


@dataclass
class BenchmarkConfig:
    fps: int = 120
    frames: int = 600
    fec_k: int = 16
    redundancy: float = 0.33
    profile: SizeProfile = field(default_factory=SizeProfile)
    pps_budget: float | None = 3000.0  # static budget; None = fallback
    min_payload: int = DEFAULT_MIN_PAYLOAD
    mtu_override: int = DEFAULT_MTU_OVERRIDE
    # "Fixed" represents the status-quo packetiser configuration. Real
    # deployments hard-code ~1500 B in venc config; the variable policy
    # is what gets access to the wider mtu_override ceiling.
    fixed_payload: int = 1500
    hysteresis: float = DEFAULT_HYSTERESIS
    s_ref_window_s: float = 2.5
    s_ref_quantile: float = 0.99
    seed: int = 0xC0FFEE


def compare_policies(cfg: BenchmarkConfig | None = None) -> dict:
    """Run FIXED and VARIABLE policies over the same synthetic trace."""
    cfg = cfg or BenchmarkConfig()

    # Shared RNG seed → identical frame sizes across the two runs
    fixed_stats = _run_fixed(cfg)
    variable_stats = _run_variable(cfg)
    return {
        "fixed": fixed_stats.summary(),
        "variable": variable_stats.summary(),
        "delta": _delta(fixed_stats.summary(), variable_stats.summary()),
    }


def _run_fixed(cfg: BenchmarkConfig) -> PolicyStats:
    enc = EncoderSim(fps=cfg.fps, profile=cfg.profile, seed=cfg.seed)
    stats = PolicyStats(name="fixed")
    payload = min(cfg.fixed_payload, 3900)
    for _ in range(cfg.frames):
        frame = enc.step(payload)
        blocks = pack_frame_into_blocks(frame.packets, cfg.fec_k, cfg.redundancy)
        stats.record(frame, payload, blocks)
    return stats


def _run_variable(cfg: BenchmarkConfig) -> PolicyStats:
    enc = EncoderSim(fps=cfg.fps, profile=cfg.profile, seed=cfg.seed)
    sim_time = [0.0]
    tracker = FrameSizePercentile(
        window_s=cfg.s_ref_window_s,
        quantile=cfg.s_ref_quantile,
        fallback=float(cfg.profile.base),
        time_fn=lambda: sim_time[0],
    )
    budget = LinkBudgetEstimator(
        window_s=1.0,
        ttl_s=2.0,
        fallback=float(cfg.pps_budget) if cfg.pps_budget else 1500.0,
        time_fn=lambda: sim_time[0],
    )
    if cfg.pps_budget:
        budget.observe(cfg.pps_budget)

    stats = PolicyStats(name="variable")
    prev_payload: int | None = None

    for _ in range(cfg.frames):
        sim_time[0] += 1.0 / cfg.fps
        # Re-seed the budget so it stays fresh throughout the run
        if cfg.pps_budget:
            budget.observe(cfg.pps_budget)

        # Peek: need a frame size before sizing, but the sizer consumes
        # S_ref from the tracker (updated with PRIOR frames only, so the
        # tracker lag matches a real deployment where the encoder reports
        # AFTER the fact).
        s_ref = tracker.value() or float(cfg.profile.base)
        decision = choose_payload_size(
            s_ref=s_ref,
            fps=float(cfg.fps),
            pps_budget=budget.current(),
            fec_k=cfg.fec_k,
            min_payload=cfg.min_payload,
            mtu_override=cfg.mtu_override,
            prev=prev_payload,
            hysteresis=cfg.hysteresis,
        )
        payload = decision.payload

        frame = enc.step(payload)
        tracker.update(frame.size_bytes)
        blocks = pack_frame_into_blocks(frame.packets, cfg.fec_k, cfg.redundancy)
        stats.record(frame, payload, blocks)
        prev_payload = payload

    return stats


def _delta(fixed: dict, variable: dict) -> dict:
    """Percentage improvements of variable over fixed (lower = improvement)."""
    def pct(f: float, v: float) -> float:
        if f == 0:
            return 0.0
        return (v - f) / f * 100.0
    return {
        "source_packets_pct": pct(fixed["total_source_packets"], variable["total_source_packets"]),
        "packets_pct": pct(fixed["total_packets"], variable["total_packets"]),
        "wire_bytes_pct": pct(fixed["total_wire_bytes"], variable["total_wire_bytes"]),
        "padding_pct": pct(fixed["total_padding_bytes"], variable["total_padding_bytes"]),
        "recovery_pct": pct(fixed["total_recovery_bytes"], variable["total_recovery_bytes"]),
        "one_block_rate_abs": variable["one_block_hit_rate"] - fixed["one_block_hit_rate"],
    }


def format_report(result: dict) -> str:
    """Render a human-readable comparison table."""
    fx = result["fixed"]
    va = result["variable"]
    dl = result["delta"]
    lines = [
        "",
        "=" * 72,
        "Variable-payload benchmark",
        "=" * 72,
        f"{'Metric':<28s} {'fixed':>14s} {'variable':>14s} {'delta':>12s}",
        "-" * 72,
        f"{'frames':<28s} {fx['frames']:>14d} {va['frames']:>14d} {'':>12s}",
        f"{'total source packets':<28s} {fx['total_source_packets']:>14d} {va['total_source_packets']:>14d} {dl['source_packets_pct']:>+11.1f}%",
        f"{'total wire packets':<28s} {fx['total_packets']:>14d} {va['total_packets']:>14d} {dl['packets_pct']:>+11.1f}%",
        f"{'total wire bytes':<28s} {fx['total_wire_bytes']:>14d} {va['total_wire_bytes']:>14d} {dl['wire_bytes_pct']:>+11.1f}%",
        f"{'source padding':<28s} {fx['total_padding_bytes']:>14d} {va['total_padding_bytes']:>14d} {dl['padding_pct']:>+11.1f}%",
        f"{'recovery bytes':<28s} {fx['total_recovery_bytes']:>14d} {va['total_recovery_bytes']:>14d} {dl['recovery_pct']:>+11.1f}%",
        f"{'one-block hit rate':<28s} {fx['one_block_hit_rate']:>13.1%} {va['one_block_hit_rate']:>13.1%} {dl['one_block_rate_abs']:>+11.1%}",
        f"{'frames spilled':<28s} {fx['frames_spilled']:>14d} {va['frames_spilled']:>14d} {'':>12s}",
        f"{'avg payload':<28s} {fx['avg_payload']:>14.0f} {va['avg_payload']:>14.0f} {'':>12s}",
        f"{'avg k':<28s} {fx['avg_k']:>14.1f} {va['avg_k']:>14.1f} {'':>12s}",
        f"{'payload volatility (MAD)':<28s} {fx['payload_volatility']:>14.1f} {va['payload_volatility']:>14.1f} {'':>12s}",
        f"{'k volatility (MAD)':<28s} {fx['k_volatility']:>14.2f} {va['k_volatility']:>14.2f} {'':>12s}",
        "=" * 72,
    ]
    return "\n".join(lines)
