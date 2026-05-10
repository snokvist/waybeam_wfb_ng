"""
Extended benchmark scenarios with KPI collection for parameter optimization.

Supports three execution modes:
  - **simulated** (default): instant, deterministic, for CI and parameter sweeps.
  - **realtime**: wall-clock paced, catches timing-dependent behaviour.
  - **replay**: feed recorded frame data from hardware captures.

All modes produce the same BenchmarkResult / BenchmarkKPI, so the same
comparison and optimization code works across synthetic and real-world data.
"""

import csv
import io
import math
import random
import time as _time
from dataclasses import dataclass, field
from typing import Callable

from fec_controller.config import ControllerConfig
from fec_controller.controller import FECController
from fec_controller.protocol import pack_frame, parse_frame, FRAME_TYPE_I, FRAME_TYPE_P
from fec_controller.service import FPSEstimator


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class FrameRecord:
    time: float
    frame_size: int
    k_actual: int
    n_actual: int
    k_needed: int
    protected: bool
    overhead: float       # (k_actual - k_needed) / k_needed when protected
    update_fired: bool
    update_direction: int  # +1 increase, -1 decrease, 0 no update
    is_iframe: bool


@dataclass
class DistributionStats:
    """Distribution statistics for a value series."""
    mean: float
    stddev: float
    min: int
    max: int
    p25: int
    p50: int       # median
    p75: int
    p95: int


@dataclass
class BenchmarkKPI:
    """Aggregate KPIs for a benchmark run."""
    scenario: str
    duration_s: float
    total_frames: int
    realtime: bool              # True if wall-clock paced

    # Protection
    protection_rate: float
    unprotected_frames: int
    max_deficit: int

    # Efficiency
    avg_overhead: float
    median_overhead: float

    # Stability — total
    update_count: int
    updates_per_second: float
    oscillation_events: int

    # Stability — directional
    increases: int
    decreases: int
    increases_per_second: float
    decreases_per_second: float

    # k/n distribution
    k_dist: DistributionStats
    n_dist: DistributionStats

    # Responsiveness
    avg_reaction_frames: float
    max_reaction_frames: int

    def summary(self) -> str:
        mode = "REALTIME" if self.realtime else "simulated"
        return (
            f"[{self.scenario}] {self.duration_s:.1f}s, "
            f"{self.total_frames} frames ({mode})\n"
            f"  Protection:    {self.protection_rate:.1%} "
            f"({self.unprotected_frames} unprotected, max deficit={self.max_deficit})\n"
            f"  Efficiency:    avg overhead={self.avg_overhead:.2f}, "
            f"median={self.median_overhead:.2f}\n"
            f"  Stability:     {self.update_count} updates "
            f"({self.updates_per_second:.2f}/s), "
            f"{self.oscillation_events} oscillation events\n"
            f"    increases={self.increases} ({self.increases_per_second:.2f}/s), "
            f"decreases={self.decreases} ({self.decreases_per_second:.2f}/s)\n"
            f"  k dist:        mean={self.k_dist.mean:.1f} "
            f"std={self.k_dist.stddev:.1f} "
            f"[{self.k_dist.min}-{self.k_dist.max}] "
            f"p25={self.k_dist.p25} p50={self.k_dist.p50} "
            f"p75={self.k_dist.p75} p95={self.k_dist.p95}\n"
            f"  n dist:        mean={self.n_dist.mean:.1f} "
            f"std={self.n_dist.stddev:.1f} "
            f"[{self.n_dist.min}-{self.n_dist.max}] "
            f"p25={self.n_dist.p25} p50={self.n_dist.p50} "
            f"p75={self.n_dist.p75} p95={self.n_dist.p95}\n"
            f"  Reaction:      avg={self.avg_reaction_frames:.1f} frames, "
            f"max={self.max_reaction_frames} frames"
        )


@dataclass
class BenchmarkResult:
    kpi: BenchmarkKPI
    frames: list[FrameRecord] = field(repr=False)

    def to_csv(self) -> str:
        """Export frame trace as CSV for external analysis."""
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow([
            "time", "frame_size", "is_iframe", "k_actual", "n_actual",
            "k_needed", "protected", "overhead", "update_fired", "update_direction",
        ])
        for f in self.frames:
            w.writerow([
                f"{f.time:.6f}", f.frame_size, int(f.is_iframe),
                f.k_actual, f.n_actual, f.k_needed,
                int(f.protected), f"{f.overhead:.4f}",
                int(f.update_fired), f.update_direction,
            ])
        return buf.getvalue()


# ---------------------------------------------------------------------------
# KPI computation (shared across all modes)
# ---------------------------------------------------------------------------

def _percentile(sorted_vals: list, p: float) -> int:
    if not sorted_vals:
        return 0
    idx = min(int(len(sorted_vals) * p), len(sorted_vals) - 1)
    return sorted_vals[idx]


def _dist_stats(values: list[int]) -> DistributionStats:
    if not values:
        return DistributionStats(0, 0, 0, 0, 0, 0, 0, 0)
    n = len(values)
    mean = sum(values) / n
    variance = sum((v - mean) ** 2 for v in values) / n
    stddev = variance ** 0.5
    s = sorted(values)
    return DistributionStats(
        mean=mean, stddev=stddev, min=s[0], max=s[-1],
        p25=_percentile(s, 0.25), p50=_percentile(s, 0.50),
        p75=_percentile(s, 0.75), p95=_percentile(s, 0.95),
    )


def compute_kpi(
    scenario: str,
    duration_s: float,
    frames: list[FrameRecord],
    controller: FECController,
    realtime: bool = False,
) -> BenchmarkKPI:
    total = len(frames)
    protected = [f for f in frames if f.protected]
    unprotected = [f for f in frames if not f.protected]

    overheads = sorted(f.overhead for f in protected)
    deficits = [f.k_needed - f.k_actual for f in unprotected]

    increases = sum(1 for f in frames if f.update_direction > 0)
    decreases = sum(1 for f in frames if f.update_direction < 0)

    k_values = [f.k_actual for f in frames]
    n_values = [f.n_actual for f in frames]

    reaction_runs = []
    run_len = 0
    for f in frames:
        if not f.protected:
            run_len += 1
        else:
            if run_len > 0:
                reaction_runs.append(run_len)
            run_len = 0
    if run_len > 0:
        reaction_runs.append(run_len)

    return BenchmarkKPI(
        scenario=scenario, duration_s=duration_s, total_frames=total,
        realtime=realtime,
        protection_rate=len(protected) / total if total else 0,
        unprotected_frames=len(unprotected),
        max_deficit=max(deficits) if deficits else 0,
        avg_overhead=sum(overheads) / len(overheads) if overheads else 0,
        median_overhead=overheads[len(overheads) // 2] if overheads else 0,
        update_count=controller.update_count,
        updates_per_second=controller.update_count / duration_s if duration_s > 0 else 0,
        oscillation_events=sum(
            1 for f in frames if f.update_fired
        ) // max(1, controller.cfg.oscillation_threshold),
        increases=increases, decreases=decreases,
        increases_per_second=increases / duration_s if duration_s > 0 else 0,
        decreases_per_second=decreases / duration_s if duration_s > 0 else 0,
        k_dist=_dist_stats(k_values), n_dist=_dist_stats(n_values),
        avg_reaction_frames=sum(reaction_runs) / len(reaction_runs) if reaction_runs else 0,
        max_reaction_frames=max(reaction_runs) if reaction_runs else 0,
    )


# ---------------------------------------------------------------------------
# Core runner — feeds frames through the full protocol path
# ---------------------------------------------------------------------------

@dataclass
class FrameInput:
    """One input frame for the runner."""
    timestamp_us: int    # frame_ready_us (monotonic, microseconds)
    frame_size: int
    is_iframe: bool


def run_frames(
    scenario: str,
    frame_inputs: list[FrameInput],
    config: ControllerConfig | None = None,
    mtu: int = 1446,
    realtime: bool = False,
) -> BenchmarkResult:
    """Feed a sequence of frames through the controller and collect KPIs.

    When *realtime* is True the controller uses ``time.monotonic`` and
    the runner sleeps between frames to match the original timestamps.
    When False (default) the controller uses the frame timestamps directly
    for deterministic, instant execution.
    """
    cfg = config or ControllerConfig(mtu=mtu)

    if realtime:
        t0_wall = _time.monotonic()
        t0_input = frame_inputs[0].timestamp_us / 1_000_000.0 if frame_inputs else 0.0
        controller = FECController(config=cfg)  # uses time.monotonic
    else:
        sim_time = [0.0]
        controller = FECController(config=cfg, time_fn=lambda: sim_time[0])

    fps_est = FPSEstimator(alpha=0.05)
    frames: list[FrameRecord] = []

    for i, fi in enumerate(frame_inputs):
        input_time_s = fi.timestamp_us / 1_000_000.0

        if realtime:
            # Pace to wall clock: sleep until this frame's target time
            target_wall = t0_wall + (input_time_s - t0_input)
            now = _time.monotonic()
            if target_wall > now:
                _time.sleep(target_wall - now)
            t = _time.monotonic() - t0_wall
        else:
            sim_time[0] = input_time_s
            t = input_time_s

        frame_type = FRAME_TYPE_I if fi.is_iframe else FRAME_TYPE_P
        wire = pack_frame(
            frame_ready_us=fi.timestamp_us,
            frame_id=i,
            frame_size_bytes=fi.frame_size,
            frame_type=frame_type,
            seq_count=max(1, math.ceil(fi.frame_size / mtu)),
        )
        parsed = parse_frame(wire)
        est_fps = fps_est.update(parsed.frame_ready_us)
        result = controller.update(parsed.frame_size_bytes, est_fps)

        p = controller.get_current()
        k_actual = p.k if p else 1
        n_actual = p.n if p else 2
        k_needed = max(1, math.ceil(fi.frame_size / mtu))
        protected = k_actual >= k_needed
        overhead = (k_actual - k_needed) / k_needed if protected and k_needed > 0 else 0.0

        if result is not None and frames:
            prev_k = frames[-1].k_actual
            update_direction = 1 if result.k > prev_k else (-1 if result.k < prev_k else 0)
        elif result is not None:
            update_direction = 0
        else:
            update_direction = 0

        frames.append(FrameRecord(
            time=t, frame_size=fi.frame_size, k_actual=k_actual,
            n_actual=n_actual, k_needed=k_needed, protected=protected,
            overhead=overhead, update_fired=result is not None,
            update_direction=update_direction, is_iframe=fi.is_iframe,
        ))

    duration = frames[-1].time - frames[0].time if len(frames) >= 2 else 0.0
    kpi = compute_kpi(scenario, duration, frames, controller, realtime=realtime)
    return BenchmarkResult(kpi=kpi, frames=frames)


# ---------------------------------------------------------------------------
# Replay recorded frame data
# ---------------------------------------------------------------------------

def load_frame_csv(path_or_text: str) -> list[FrameInput]:
    """Load frame data from CSV.

    Expected columns (header required):
      timestamp_us, frame_size, is_iframe

    ``is_iframe`` is 0/1 or True/False.
    """
    if "\n" in path_or_text:
        reader = csv.DictReader(io.StringIO(path_or_text))
    else:
        reader = csv.DictReader(open(path_or_text))
    inputs = []
    for row in reader:
        inputs.append(FrameInput(
            timestamp_us=int(row["timestamp_us"]),
            frame_size=int(row["frame_size"]),
            is_iframe=row["is_iframe"].strip().lower() in ("1", "true"),
        ))
    return inputs


def replay(
    path_or_text: str,
    config: ControllerConfig | None = None,
    realtime: bool = False,
    scenario: str = "replay",
) -> BenchmarkResult:
    """Replay recorded frame data and collect KPIs."""
    inputs = load_frame_csv(path_or_text)
    return run_frames(scenario, inputs, config=config, realtime=realtime)


# ---------------------------------------------------------------------------
# Scenario helpers — generate FrameInput lists
# ---------------------------------------------------------------------------

def _generate_frames(
    fps: int,
    duration_s: float,
    size_fn: Callable[[int, float, bool], int],
    gop_interval: int = 30,
    seed: int = 42,
) -> list[FrameInput]:
    random.seed(seed)
    frame_period_us = int(1_000_000 / fps)
    total = int(duration_s * fps)
    inputs = []
    for i in range(total):
        t = i / fps
        ts_us = i * frame_period_us
        is_iframe = i % gop_interval == 0
        size = max(1, size_fn(i, t, is_iframe))
        inputs.append(FrameInput(timestamp_us=ts_us, frame_size=size, is_iframe=is_iframe))
    return inputs


# ---------------------------------------------------------------------------
# Scenarios (return BenchmarkResult)
# ---------------------------------------------------------------------------

def scenario_steady_gop(
    duration_s: float = 60.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Steady state: P=5KB, I=15KB every 30 frames, 3% jitter."""
    def size_fn(i, t, is_iframe):
        base = 15000 if is_iframe else 5000
        return int(base * random.gauss(1.0, 0.03))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("steady_gop", inputs, config=config, realtime=realtime)


def scenario_scene_changes(
    duration_s: float = 60.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Scene changes at t=5,12,20,35,50s.  Each shifts bitrate 2-4x for 1-3s."""
    changes = [
        (5.0, 3.0, 3.0), (12.0, 1.5, 2.0), (20.0, 2.0, 4.0),
        (35.0, 1.0, 2.5), (50.0, 3.0, 1.5),
    ]
    def size_fn(i, t, is_iframe):
        mult = 1.0
        for start, dur, m in changes:
            if start <= t < start + dur:
                mult = m
                break
        base = (12000 if is_iframe else 4000) * mult
        return int(base * random.gauss(1.0, 0.05))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("scene_changes", inputs, config=config, realtime=realtime)


def scenario_gradual_ramp(
    duration_s: float = 30.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Bitrate ramps linearly from 3KB to 30KB over the full duration."""
    def size_fn(i, t, is_iframe):
        progress = t / duration_s
        base = 3000 + (30000 - 3000) * progress
        iframe_mult = 1.3 if is_iframe else 1.0
        return int(base * iframe_mult * random.gauss(1.0, 0.03))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("gradual_ramp", inputs, config=config, realtime=realtime)


def scenario_bitrate_cliff(
    duration_s: float = 45.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """High bitrate (25KB) for 10s, cliff to 3KB for 15s, recovery to 15KB."""
    def size_fn(i, t, is_iframe):
        if t < 10.0:
            base = 25000
        elif t < 25.0:
            base = 3000
        else:
            base = 15000
        iframe_mult = 1.2 if is_iframe else 1.0
        return int(base * iframe_mult * random.gauss(1.0, 0.03))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("bitrate_cliff", inputs, config=config, realtime=realtime)


def scenario_encoder_hunting(
    duration_s: float = 30.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Encoder rate control hunting: bitrate swings +-50% every 0.5-2s."""
    random.seed(99)
    swings = []
    t = 0.0
    high = True
    while t < duration_s:
        dur = random.uniform(0.5, 2.0)
        swings.append((t, t + dur, high))
        t += dur
        high = not high
    random.seed(42)

    def size_fn(i, t, is_iframe):
        base = 8000
        for start, end, is_high in swings:
            if start <= t < end:
                base = 12000 if is_high else 4000
                break
        iframe_mult = 1.5 if is_iframe else 1.0
        return int(base * iframe_mult * random.gauss(1.0, 0.05))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("encoder_hunting", inputs, config=config, realtime=realtime)


def scenario_real_world_mixed(
    duration_s: float = 60.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Mixed: steady -> spike -> oscillation -> cliff -> ramp -> steady."""
    def size_fn(i, t, is_iframe):
        if t < 10.0:
            base = 6000
        elif t < 15.0:
            base = 20000
        elif t < 25.0:
            phase = (t - 15.0) / 1.5 * 2 * 3.14159
            base = 10000 + 5000 * math.sin(phase)
        elif t < 30.0:
            base = 2000
        elif t < 40.0:
            progress = (t - 30.0) / 10.0
            base = 2000 + 10000 * progress
        else:
            base = 12000
        iframe_mult = 1.4 if is_iframe else 1.0
        return int(base * iframe_mult * random.gauss(1.0, 0.04))
    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("real_world_mixed", inputs, config=config, realtime=realtime)


def scenario_low_bitrate_cruise(
    duration_s: float = 45.0, fps: int = 60,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Low bitrate cruising: tiny P=800B, small I=2KB, 60fps."""
    def size_fn(i, t, is_iframe):
        base = 2000 if is_iframe else 800
        return int(base * random.gauss(1.0, 0.02))
    inputs = _generate_frames(fps, duration_s, size_fn, gop_interval=60)
    return run_frames("low_bitrate_cruise", inputs, config=config, realtime=realtime)


def scenario_iframe_burst_stress(
    duration_s: float = 30.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """I-frame burst stress: I-frames 8x larger, short GOP=15."""
    def size_fn(i, t, is_iframe):
        if is_iframe:
            return int(40000 * random.gauss(1.0, 0.05))
        return int(5000 * random.gauss(1.0, 0.03))
    inputs = _generate_frames(fps, duration_s, size_fn, gop_interval=15)
    return run_frames("iframe_burst_stress", inputs, config=config, realtime=realtime)


def scenario_mcs_plateaus(
    duration_s: float = 60.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """MCS/bandwidth plateaus with +-20% variance and steep transitions.

    Simulates locked MCS rates stepping between bitrate levels.
    Each plateau has realistic scene-complexity variance (+-20%).

    0-12s:  8 Mbps plateau  (P~5.5KB, I~16KB)
    12-24s: 12 Mbps plateau (P~8.3KB, I~25KB)  — MCS bump up
    24-36s: 8 Mbps plateau  (P~5.5KB, I~16KB)  — MCS drop back
    36-48s: 18 Mbps plateau (P~12.5KB, I~37KB) — high MCS
    48-60s: 12 Mbps plateau (P~8.3KB, I~25KB)  — settle mid
    """
    # At each MCS level, base P-frame size roughly = bitrate_kbps / fps
    # I-frames ~3x P-frames
    plateaus = [
        (0.0,  12.0, 5500),    # 8 Mbps
        (12.0, 24.0, 8300),    # 12 Mbps
        (24.0, 36.0, 5500),    # 8 Mbps
        (36.0, 48.0, 12500),   # 18 Mbps
        (48.0, 60.0, 8300),    # 12 Mbps
    ]

    def size_fn(i, t, is_iframe):
        base = 5500
        for start, end, b in plateaus:
            if start <= t < end:
                base = b
                break
        iframe_mult = 3.0 if is_iframe else 1.0
        # +-20% variance from scene complexity
        return int(base * iframe_mult * random.uniform(0.80, 1.20))

    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("mcs_plateaus", inputs, config=config, realtime=realtime)


def scenario_mcs_rapid_switching(
    duration_s: float = 45.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """Rapid MCS switching — short plateaus (2-5s) with +-20% variance.

    Simulates aggressive link adaptation where RSSI fluctuations cause
    frequent MCS changes.  Tests whether the controller avoids excessive
    FEC updates when the bitrate jumps but each new level is stable.

    Pattern: 6 plateaus of 2-5s each, alternating between 2-3 MCS levels.
    """
    random.seed(77)
    levels_kbps = [5500, 8300, 12500]  # 8/12/18 Mbps base P-frame sizes
    plateaus = []
    t = 0.0
    while t < duration_s:
        dur = random.uniform(2.0, 5.0)
        base = random.choice(levels_kbps)
        plateaus.append((t, t + dur, base))
        t += dur
    random.seed(42)

    def size_fn(i, t, is_iframe):
        base = 5500
        for start, end, b in plateaus:
            if start <= t < end:
                base = b
                break
        iframe_mult = 3.0 if is_iframe else 1.0
        return int(base * iframe_mult * random.uniform(0.80, 1.20))

    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("mcs_rapid_switching", inputs, config=config, realtime=realtime)


def scenario_mcs_step_up_down(
    duration_s: float = 40.0, fps: int = 120,
    config: ControllerConfig | None = None, realtime: bool = False,
) -> BenchmarkResult:
    """MCS staircase: step up through 3 levels, then step back down.

    Tests that k increases quickly at each step-up and decreases
    conservatively at each step-down, with plateau variance not
    causing spurious updates within each level.

    0-8s:   6 Mbps  (P~4.2KB)
    8-16s:  12 Mbps (P~8.3KB)  — step up
    16-24s: 20 Mbps (P~13.9KB) — step up
    24-32s: 12 Mbps (P~8.3KB)  — step down
    32-40s: 6 Mbps  (P~4.2KB)  — step down
    """
    plateaus = [
        (0.0,  8.0,  4200),
        (8.0,  16.0, 8300),
        (16.0, 24.0, 13900),
        (24.0, 32.0, 8300),
        (32.0, 40.0, 4200),
    ]

    def size_fn(i, t, is_iframe):
        base = 4200
        for start, end, b in plateaus:
            if start <= t < end:
                base = b
                break
        iframe_mult = 3.0 if is_iframe else 1.0
        return int(base * iframe_mult * random.uniform(0.80, 1.20))

    inputs = _generate_frames(fps, duration_s, size_fn)
    return run_frames("mcs_step_up_down", inputs, config=config, realtime=realtime)


ALL_SCENARIOS = [
    scenario_steady_gop,
    scenario_scene_changes,
    scenario_gradual_ramp,
    scenario_bitrate_cliff,
    scenario_encoder_hunting,
    scenario_real_world_mixed,
    scenario_low_bitrate_cruise,
    scenario_iframe_burst_stress,
    scenario_mcs_plateaus,
    scenario_mcs_rapid_switching,
    scenario_mcs_step_up_down,
]


def run_all(
    config: ControllerConfig | None = None, realtime: bool = False,
) -> list[BenchmarkResult]:
    """Run all scenarios and return results."""
    return [fn(config=config, realtime=realtime) for fn in ALL_SCENARIOS]
