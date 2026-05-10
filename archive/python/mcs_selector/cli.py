"""Command-line entry point for the MCS selector POC."""

import argparse
import asyncio
import logging
import sys

from mcs_selector.config import RANGE_PRESETS, MCSSelectorConfig
from mcs_selector.ranges import mcs_for_bucket
from mcs_selector.selector import MCSSelector
from mcs_selector.service import MCSSelectorService
from mcs_selector.simulation import TraceConfig, generate


def _build_config(args) -> MCSSelectorConfig:
    cfg = MCSSelectorConfig(range=args.range)
    if args.stats_host is not None:
        cfg.stats_host = args.stats_host
    if args.stats_port is not None:
        cfg.stats_port = args.stats_port
    if args.tx_host is not None:
        cfg.tx_host = args.tx_host
    if args.tx_port is not None:
        cfg.tx_port = args.tx_port
    cfg.dry_run = args.dry_run
    return cfg


def cmd_run(args) -> int:
    cfg = _build_config(args)
    service = MCSSelectorService(cfg)

    async def _run():
        loop = asyncio.get_running_loop()
        stop = asyncio.Event()
        try:
            import signal
            for sig in (signal.SIGINT, signal.SIGTERM):
                loop.add_signal_handler(sig, stop.set)
        except (ImportError, NotImplementedError):
            pass

        await service.start()
        await stop.wait()
        await service.stop()

    asyncio.run(_run())
    return 0


def cmd_simulate(args) -> int:
    cfg = MCSSelectorConfig(range=args.range, dry_run=True)
    selector = MCSSelector(cfg, time_fn=lambda: cmd_simulate._sim_now)
    cmd_simulate._sim_now = 0.0

    trace_cfg = TraceConfig(duration_s=args.duration, rate_hz=args.rate)
    print(
        f"# range={cfg.range} rssi_lo={cfg.rssi_thresh_low} "
        f"rssi_hi={cfg.rssi_thresh_high} deadband={cfg.rssi_deadband_db}"
    )
    print("# t_s\teff_rssi\tloss%\tbucket\tmcs\treason")
    for t, dgram in generate(trace_cfg):
        cmd_simulate._sim_now = t
        decision = selector.update(dgram, now=t)
        if decision is None:
            continue
        s = decision.score
        print(
            f"{t:6.2f}\t{(s.effective_rssi if s else 0):7.2f}\t"
            f"{(s.smoothed_loss_ratio*100 if s else 0):5.2f}\t"
            f"{decision.bucket}\t{decision.mcs_index}\t{decision.reason}"
        )
    return 0


def cmd_table(args) -> int:
    cfg = MCSSelectorConfig(range=args.range)
    rng = cfg.selected_range()
    print(f"range='{cfg.range}' -> mcs presets {rng}")
    print(f"  rssi <  {cfg.rssi_thresh_low:6.1f} dBm  ->  bucket 0  ->  mcs {mcs_for_bucket(0, cfg)}")
    print(
        f"  {cfg.rssi_thresh_low:6.1f} <= rssi < "
        f"{cfg.rssi_thresh_high:6.1f}     ->  bucket 1  ->  mcs {mcs_for_bucket(1, cfg)}"
    )
    print(f"  rssi >= {cfg.rssi_thresh_high:6.1f} dBm  ->  bucket 2  ->  mcs {mcs_for_bucket(2, cfg)}")
    print(f"  deadband: {cfg.rssi_deadband_db} dB symmetric")
    print(
        f"  hysteresis: up={cfg.up_consecutive} samples / "
        f"down={cfg.down_consecutive} samples"
    )
    print(
        f"  cooldown:   up={cfg.up_cooldown_s}s / down={cfg.down_cooldown_s}s"
    )
    print(f"  failsafe:   {cfg.failsafe_timeout_s}s gap -> range[0]")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mcs_selector")
    parser.add_argument(
        "--log-level", default="INFO",
        help="DEBUG / INFO / WARNING / ERROR (default INFO)",
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="run the selector against a live wfb_rx -Y feed")
    p_run.add_argument("--range", choices=sorted(RANGE_PRESETS), default="med")
    p_run.add_argument("--stats-host", default=None)
    p_run.add_argument("--stats-port", type=int, default=None)
    p_run.add_argument("--tx-host", default=None)
    p_run.add_argument("--tx-port", type=int, default=None)
    p_run.add_argument("--dry-run", action="store_true",
                       help="compute decisions but don't send CMD_SET_RADIO")
    p_run.set_defaults(func=cmd_run)

    p_sim = sub.add_parser("simulate", help="run the FSM against a synthetic trace")
    p_sim.add_argument("--range", choices=sorted(RANGE_PRESETS), default="med")
    p_sim.add_argument("--duration", type=float, default=30.0)
    p_sim.add_argument("--rate", type=float, default=10.0)
    p_sim.set_defaults(func=cmd_simulate)

    p_tab = sub.add_parser("table", help="print the bucket -> mcs mapping for a range")
    p_tab.add_argument("--range", choices=sorted(RANGE_PRESETS), default="med")
    p_tab.set_defaults(func=cmd_table)

    args = parser.parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
    )
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
