#!/usr/bin/env python3
"""
waybeam-hub adaptive FEC controller module.

Receives per-frame stat packets from the video sidecar via UDP,
computes optimal FEC k/n, and sends updates to wfb_tx via wfb_tx_cmd
control socket.

Architecture:
  sidecar --[UDP stat packets]--> fec_controller --[UDP cmd]--> wfb_tx control port

No shared memory, no custom wfb_tx patches required for FEC control.
Session announce on FEC change requires the wfb_tx session_announce patch.
"""

import math
import time
import socket
import struct
import asyncio
import logging
import argparse
from dataclasses import dataclass, field
from collections import deque
from typing import Callable

log = logging.getLogger("fec_ctrl")


# ---------------------------------------------------------------------------
# Sidecar stat packet (from video pipeline)
# ---------------------------------------------------------------------------
# Per-frame UDP packet, 8 bytes:
#   u32  frame_size     (bytes)
#   u16  fps_x10        (fps * 10, e.g. 1200 = 120.0fps)
#   u8   frame_type     (0=P, 1=I)
#   u8   reserved
#
STAT_FMT = '<IHBx'
STAT_SIZE = struct.calcsize(STAT_FMT)


def unpack_stat(data: bytes) -> dict:
    frame_size, fps_x10, frame_type = struct.unpack(STAT_FMT, data[:STAT_SIZE])
    return {
        'frame_size': frame_size,
        'fps': fps_x10 / 10.0,
        'frame_type': frame_type,
    }


def pack_stat(frame_size: int, fps: float, frame_type: int = 0) -> bytes:
    return struct.pack(STAT_FMT, frame_size, int(fps * 10), frame_type)


# ---------------------------------------------------------------------------
# FEC params
# ---------------------------------------------------------------------------

@dataclass
class FECParams:
    k: int
    n: int
    fec_timeout_ms: int
    redundancy: float
    packets_per_frame: int
    avg_frame_size: float
    headroom: float


# ---------------------------------------------------------------------------
# Headroom tracker - learns I/P variance from actual data
# ---------------------------------------------------------------------------

class HeadroomTracker:
    """
    Tracks max/avg frame size ratio over a rolling time window.
    Adapts to encoder behavior without needing to know GOP structure.
    """

    def __init__(self, window_s: float = 2.5, margin: float = 1.05,
                 floor: float = 1.05, ceiling: float = 1.40,
                 time_fn: Callable[[], float] = None):
        self.window_s = window_s
        self.margin = margin      # safety margin on top of observed ratio
        self.floor = floor        # minimum headroom
        self.ceiling = ceiling    # maximum headroom
        self._time_fn = time_fn or time.monotonic
        self._samples: deque[tuple[float, float]] = deque()
        self._avg = None
        self._alpha = 0.05

    def update(self, frame_size: float) -> None:
        now = self._time_fn()
        self._samples.append((now, frame_size))

        if self._avg is None:
            self._avg = frame_size
        else:
            self._avg = self._alpha * frame_size + (1.0 - self._alpha) * self._avg

        cutoff = now - self.window_s
        while self._samples and self._samples[0][0] < cutoff:
            self._samples.popleft()

    @property
    def headroom(self) -> float:
        if not self._samples or not self._avg or self._avg < 1:
            return self.floor
        max_in_window = max(s[1] for s in self._samples)
        ratio = (max_in_window / self._avg) * self.margin
        return max(self.floor, min(self.ceiling, ratio))


# ---------------------------------------------------------------------------
# Controller config
# ---------------------------------------------------------------------------

@dataclass
class ControllerConfig:
    mtu: int = 1446

    ewma_alpha: float = 0.05

    # Learned headroom
    headroom_window_s: float = 2.5
    headroom_min: float = 1.05
    headroom_max: float = 1.40
    headroom_margin: float = 1.05

    min_k: int = 1
    max_k: int = 48
    min_n: int = 2
    max_n: int = 72

    # (k, redundancy_fraction) - interpolated
    redundancy_curve: list = field(default_factory=lambda: [
        (1,  0.50),
        (4,  0.40),
        (8,  0.33),
        (16, 0.30),
        (32, 0.27),
        (48, 0.25),
    ])

    timeout_fraction: float = 0.5

    # Update gating
    k_hysteresis: int = 2
    min_update_interval: float = 0.5    # conservative for wfb_tx_cmd path


# ---------------------------------------------------------------------------
# Controller core
# ---------------------------------------------------------------------------

class FECController:
    def __init__(self, config: ControllerConfig = None,
                 time_fn: Callable[[], float] = None):
        self.cfg = config or ControllerConfig()
        self._time_fn = time_fn or time.monotonic

        self.avg_frame_size = None
        self.current_fps = None
        self.current_params: FECParams | None = None
        self.last_update_time = -999.0
        self.update_count = 0

        self.headroom_tracker = HeadroomTracker(
            window_s=self.cfg.headroom_window_s,
            margin=self.cfg.headroom_margin,
            floor=self.cfg.headroom_min,
            ceiling=self.cfg.headroom_max,
            time_fn=self._time_fn,
        )
        self.cfg.redundancy_curve.sort(key=lambda x: x[0])

    def _interpolate_redundancy(self, k: int) -> float:
        curve = self.cfg.redundancy_curve
        if k <= curve[0][0]:
            return curve[0][1]
        if k >= curve[-1][0]:
            return curve[-1][1]
        for i in range(len(curve) - 1):
            k0, r0 = curve[i]
            k1, r1 = curve[i + 1]
            if k0 <= k <= k1:
                t = (k - k0) / (k1 - k0)
                return r0 + t * (r1 - r0)
        return curve[-1][1]

    def compute_params(self, avg_frame_size: float, fps: float,
                       headroom: float) -> FECParams:
        target_size = avg_frame_size * headroom
        packets_per_frame = max(1, math.ceil(target_size / self.cfg.mtu))
        k = max(self.cfg.min_k, min(self.cfg.max_k, packets_per_frame))

        redundancy = self._interpolate_redundancy(k)
        n = math.ceil(k / (1.0 - redundancy))
        n = max(self.cfg.min_n, min(self.cfg.max_n, n))
        if n <= k:
            n = k + 1

        frame_period_ms = 1000.0 / fps
        fec_timeout_ms = max(1, int(frame_period_ms * self.cfg.timeout_fraction))

        return FECParams(
            k=k, n=n,
            fec_timeout_ms=fec_timeout_ms,
            redundancy=redundancy,
            packets_per_frame=packets_per_frame,
            avg_frame_size=avg_frame_size,
            headroom=headroom,
        )

    def update(self, frame_size: int, fps: float) -> FECParams | None:
        """
        Feed one frame observation.
        Returns FECParams if an update should be sent, None otherwise.
        """
        now = self._time_fn()

        self.headroom_tracker.update(float(frame_size))

        if self.avg_frame_size is None:
            self.avg_frame_size = float(frame_size)
        else:
            self.avg_frame_size = (
                self.cfg.ewma_alpha * frame_size
                + (1.0 - self.cfg.ewma_alpha) * self.avg_frame_size
            )

        self.current_fps = fps
        headroom = self.headroom_tracker.headroom
        candidate = self.compute_params(self.avg_frame_size, fps, headroom)

        if self.current_params is None:
            self.current_params = candidate
            self.last_update_time = now
            self.update_count += 1
            return candidate

        k_delta = abs(candidate.k - self.current_params.k)
        if k_delta < self.cfg.k_hysteresis:
            return None
        if now - self.last_update_time < self.cfg.min_update_interval:
            return None

        self.current_params = candidate
        self.last_update_time = now
        self.update_count += 1
        return candidate

    def get_current(self) -> FECParams | None:
        return self.current_params


# ---------------------------------------------------------------------------
# wfb_tx_cmd interface
# ---------------------------------------------------------------------------

class WfbTxControl:
    """
    Send FEC updates to wfb_tx via its UDP control socket.

    wfb_tx prints "LISTEN_UDP_CONTROL <port>" on startup.
    The control protocol is a simple text command over UDP.
    """

    def __init__(self, host: str = '127.0.0.1', port: int = 0):
        self.host = host
        self.port = port
        self._sock = None

    def connect(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_fec(self, k: int, n: int) -> bool:
        """Send set_fec command. Returns True on success."""
        if not self._sock:
            self.connect()
        try:
            # wfb_tx_cmd protocol: send command as text over UDP
            # Format based on wfb_tx_cmd source convention
            cmd = f"set_fec {k} {n}\n"
            self._sock.sendto(cmd.encode(), (self.host, self.port))
            log.info(f"Sent FEC update: k={k} n={n} -> {self.host}:{self.port}")
            return True
        except OSError as e:
            log.error(f"Failed to send FEC update: {e}")
            return False

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None


# ---------------------------------------------------------------------------
# Stats receiver + main loop (asyncio)
# ---------------------------------------------------------------------------

class FECControllerService:
    """
    Async service that receives sidecar stats and drives FEC updates.
    Designed to run as a waybeam-hub module.
    """

    def __init__(self, config: ControllerConfig,
                 stat_port: int = 5610,
                 wfb_control_host: str = '127.0.0.1',
                 wfb_control_port: int = 0,
                 dry_run: bool = False):
        self.config = config
        self.stat_port = stat_port
        self.dry_run = dry_run

        self.controller = FECController(config)
        self.wfb_tx = WfbTxControl(wfb_control_host, wfb_control_port)

        self._frame_count = 0
        self._last_log_time = 0.0

    def _apply_update(self, params: FECParams):
        """Send FEC update to wfb_tx."""
        if self.dry_run:
            log.info(f"[DRY RUN] set_fec k={params.k} n={params.n} "
                     f"(redun={params.redundancy:.0%} "
                     f"timeout={params.fec_timeout_ms}ms "
                     f"hroom={params.headroom:.2f} "
                     f"avg={params.avg_frame_size:.0f}B "
                     f"pkts={params.packets_per_frame})")
        else:
            self.wfb_tx.send_fec(params.k, params.n)

    def _handle_stat(self, data: bytes):
        """Process one stat packet from sidecar."""
        if len(data) < STAT_SIZE:
            return

        stat = unpack_stat(data)
        self._frame_count += 1

        result = self.controller.update(stat['frame_size'], stat['fps'])
        if result is not None:
            self._apply_update(result)

        # Periodic status log
        now = time.monotonic()
        if now - self._last_log_time >= 2.0:
            p = self.controller.get_current()
            if p:
                hr = self.controller.headroom_tracker.headroom
                log.info(f"status: frames={self._frame_count} "
                         f"avg={self.controller.avg_frame_size:.0f}B "
                         f"hroom={hr:.2f} k={p.k} n={p.n} "
                         f"redun={p.redundancy:.0%} "
                         f"updates={self.controller.update_count}")
            self._last_log_time = now

    async def run(self):
        """Main async loop - listen for stat packets."""
        log.info(f"FEC controller starting on UDP :{self.stat_port}")
        log.info(f"wfb_tx control: {self.wfb_tx.host}:{self.wfb_tx.port}")
        log.info(f"MTU={self.config.mtu} dry_run={self.dry_run}")

        if not self.dry_run:
            self.wfb_tx.connect()

        # Create UDP listener
        loop = asyncio.get_event_loop()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _StatProtocol(self._handle_stat),
            local_addr=('0.0.0.0', self.stat_port),
        )

        log.info("Listening for sidecar stat packets...")

        try:
            # Run forever (or until cancelled)
            await asyncio.Event().wait()
        finally:
            transport.close()
            self.wfb_tx.close()


class _StatProtocol(asyncio.DatagramProtocol):
    def __init__(self, handler):
        self._handler = handler

    def datagram_received(self, data, addr):
        self._handler(data)


# ---------------------------------------------------------------------------
# Simulation (for testing without real hardware)
# ---------------------------------------------------------------------------

def simulate_stream(
    fps: int = 120,
    base_frame_size: int = 5000,
    i_frame_multiplier: float = 1.15,
    gop_interval: int = 30,
    duration_s: float = 8.0,
    bitrate_events: list = None,
):
    import random

    if bitrate_events is None:
        bitrate_events = [(3.0, base_frame_size * 3), (6.0, base_frame_size)]

    sim_time = [0.0]
    controller = FECController(
        config=ControllerConfig(ewma_alpha=0.05),
        time_fn=lambda: sim_time[0],
    )

    frame_period = 1.0 / fps
    total_frames = int(duration_s * fps)

    print(f"\nSimulating {fps}fps, {duration_s}s, base={base_frame_size}B, "
          f"I-mult={i_frame_multiplier}x")
    for t, sz in bitrate_events:
        print(f"  Event at {t}s: base -> {sz}B")

    hdr = (f"{'Time':>6s}  {'T':>1s}  {'FrmSize':>8s}  {'EWMA':>8s}  "
           f"{'Hroom':>5s}  {'k':>3s}  {'n':>3s}  {'n-k':>3s}  "
           f"{'Redun':>5s}  {'T/O':>3s}  {'Pkt':>3s}")
    sep = "-" * len(hdr)
    print(sep)
    print(hdr)
    print(sep)

    current_base = base_frame_size
    event_idx = 0

    for i in range(total_frames):
        t = i * frame_period
        sim_time[0] = t

        while event_idx < len(bitrate_events) and t >= bitrate_events[event_idx][0]:
            current_base = bitrate_events[event_idx][1]
            event_idx += 1

        is_iframe = (i % gop_interval == 0)
        jitter = random.gauss(1.0, 0.03)
        frame_size = int(current_base * (i_frame_multiplier if is_iframe else 1.0) * jitter)
        ftype = "I" if is_iframe else "P"

        result = controller.update(frame_size, fps)

        near_event = any(abs(t - ev[0]) < frame_period * 2 for ev in bitrate_events)
        if result is not None or i % fps == 0 or near_event:
            p = result or controller.get_current()
            if p:
                marker = " <<<" if result else ""
                hr = controller.headroom_tracker.headroom
                print(f"{t:6.2f}  {ftype:>1s}  {frame_size:8d}  "
                      f"{controller.avg_frame_size:8.0f}  "
                      f"{hr:5.2f}  "
                      f"{p.k:3d}  {p.n:3d}  {p.n - p.k:3d}  "
                      f"{p.redundancy:4.0%}  {p.fec_timeout_ms:3d}  "
                      f"{p.packets_per_frame:3d}{marker}")


def print_reference_table():
    controller = FECController()

    print(f"\nReference table (MTU={controller.cfg.mtu}, headroom=1.15)")
    hdr = (f"{'FrameSize':>10s}  {'Pkts':>4s}  {'k':>3s}  {'n':>3s}  "
           f"{'n-k':>3s}  {'Redun':>5s}  {'T/O@60':>6s}  {'T/O@120':>7s}  "
           f"{'Effic':>5s}")
    sep = "-" * len(hdr)
    print(sep)
    print(hdr)
    print(sep)

    for size in [500, 1000, 1446, 2000, 3000, 5000, 8000,
                 12000, 20000, 30000, 44000, 60000]:
        p60 = controller.compute_params(float(size), 60, 1.15)
        p120 = controller.compute_params(float(size), 120, 1.15)
        print(f"{size:10d}  {p60.packets_per_frame:4d}  "
              f"{p60.k:3d}  {p60.n:3d}  {p60.n - p60.k:3d}  "
              f"{p60.redundancy:4.0%}  {p60.fec_timeout_ms:4d}  "
              f"{p120.fec_timeout_ms:5d}  {p60.k / p60.n:4.0%}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="waybeam-hub adaptive FEC controller for wfb-ng")

    sub = parser.add_subparsers(dest='cmd')

    # --- run (production) ---
    run_p = sub.add_parser('run', help='Run FEC controller service')
    run_p.add_argument('--stat-port', type=int, default=5610,
                       help='UDP port for sidecar stat packets (default: 5610)')
    run_p.add_argument('--wfb-host', default='127.0.0.1',
                       help='wfb_tx control host (default: 127.0.0.1)')
    run_p.add_argument('--wfb-port', type=int, required=True,
                       help='wfb_tx control port (from LISTEN_UDP_CONTROL)')
    run_p.add_argument('--mtu', type=int, default=1446,
                       help='Radio MTU (default: 1446)')
    run_p.add_argument('--min-update-interval', type=float, default=0.5,
                       help='Min seconds between FEC updates (default: 0.5)')
    run_p.add_argument('--dry-run', action='store_true',
                       help='Log updates without sending to wfb_tx')

    # --- simulate ---
    sim_p = sub.add_parser('simulate', help='Run simulation')
    sim_p.add_argument('--fps', type=int, default=120)
    sim_p.add_argument('--base-frame-size', type=int, default=5000)
    sim_p.add_argument('--duration', type=float, default=8.0)

    # --- table ---
    sub.add_parser('table', help='Print reference table')

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s %(name)s %(levelname)s %(message)s',
    )

    if args.cmd == 'run':
        config = ControllerConfig(
            mtu=args.mtu,
            min_update_interval=args.min_update_interval,
        )
        service = FECControllerService(
            config=config,
            stat_port=args.stat_port,
            wfb_control_host=args.wfb_host,
            wfb_control_port=args.wfb_port,
            dry_run=args.dry_run,
        )
        asyncio.run(service.run())

    elif args.cmd == 'simulate':
        simulate_stream(fps=args.fps, base_frame_size=args.base_frame_size,
                        duration_s=args.duration)
    elif args.cmd == 'table':
        print_reference_table()
    else:
        print_reference_table()
        print()
        simulate_stream()


if __name__ == "__main__":
    main()
