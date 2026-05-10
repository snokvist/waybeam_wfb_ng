"""CLI entry point for the FEC controller."""

import asyncio
import logging
import argparse

from fec_controller.config import ControllerConfig
from fec_controller.service import FECControllerService
from fec_controller.simulation import simulate_stream, print_reference_table
from fec_controller.benchmark import run_all, replay
from fec_controller.payload_benchmark import (
    BenchmarkConfig as PayloadBenchmarkConfig,
    LinkBudgetProfile,
    compare_policies,
    format_report,
)
from fec_controller.encoder_sim import SizeProfile


def main() -> None:
    parser = argparse.ArgumentParser(
        description="waybeam-hub adaptive FEC controller for wfb-ng"
    )
    sub = parser.add_subparsers(dest="cmd")

    # --- run (production) ---
    run_p = sub.add_parser("run", help="Run FEC controller service")
    run_p.add_argument(
        "--stat-port",
        type=int,
        default=5610,
        help="UDP port to listen for sidecar FRAME packets (default: 5610)",
    )
    run_p.add_argument(
        "--wfb-host",
        default="127.0.0.1",
        help="wfb_tx control host (default: 127.0.0.1)",
    )
    run_p.add_argument(
        "--wfb-port",
        type=int,
        required=True,
        help="wfb_tx control port (from LISTEN_UDP_CONTROL)",
    )
    run_p.add_argument(
        "--mtu", type=int, default=1446, help="Radio MTU (default: 1446)"
    )
    run_p.add_argument(
        "--min-update-interval",
        type=float,
        default=0.5,
        help="Min seconds between FEC updates (default: 0.5)",
    )
    run_p.add_argument(
        "--dry-run",
        action="store_true",
        help="Log updates without sending to wfb_tx",
    )
    run_p.add_argument(
        "--sidecar-host",
        default="",
        help="Venc sidecar host to subscribe to (e.g. 10.0.0.1). "
             "If set, sends SUBSCRIBE messages to keep subscription alive.",
    )
    run_p.add_argument(
        "--sidecar-port",
        type=int,
        default=6666,
        help="Venc sidecar UDP port (default: 6666)",
    )
    run_p.add_argument(
        "--enable-variable-payload",
        action="store_true",
        help=(
            "Run the P-first sizer as a read-only observer alongside the "
            "legacy FEC controller. Logs chosen payload on change. Does "
            "not yet drive venc or wfb_tx (awaits sidecar protocol "
            "extensions)."
        ),
    )
    run_p.add_argument(
        "--target-fec-k",
        type=int,
        default=8,
        help="Sizer's desired source-packet cap (default: 8)",
    )
    run_p.add_argument(
        "--min-payload",
        type=int,
        default=800,
        help="Sizer payload floor in bytes (default: 800)",
    )
    run_p.add_argument(
        "--mtu-override",
        type=int,
        default=1500,
        help="Sizer payload ceiling hint; hard-capped at 3900 (default: 1500)",
    )

    # --- simulate ---
    sim_p = sub.add_parser("simulate", help="Run simulation")
    sim_p.add_argument("--fps", type=int, default=120)
    sim_p.add_argument("--base-frame-size", type=int, default=5000)
    sim_p.add_argument("--duration", type=float, default=8.0)

    # --- table ---
    sub.add_parser("table", help="Print reference table")

    # --- payload-benchmark (variable-P vs fixed-P) ---
    pb_p = sub.add_parser(
        "payload-benchmark",
        help="Compare fixed-P vs variable-P sizing over a synthetic trace",
    )
    pb_p.add_argument("--fps", type=int, default=60)
    pb_p.add_argument("--frames", type=int, default=600)
    pb_p.add_argument("--fec-k", type=int, default=8)
    pb_p.add_argument("--base", type=int, default=8000)
    pb_p.add_argument("--i-mult", type=float, default=5.0)
    pb_p.add_argument("--gop", type=int, default=30)
    pb_p.add_argument("--jitter", type=float, default=0.03)
    pb_p.add_argument("--pps-budget", type=float, default=3000.0)
    pb_p.add_argument("--fixed-payload", type=int, default=1500)
    pb_p.add_argument("--mtu-override", type=int, default=3000)
    pb_p.add_argument("--min-payload", type=int, default=800)
    pb_p.add_argument("--hysteresis", type=float, default=0.12)
    pb_p.add_argument(
        "--ramp-at", type=float, default=0.0,
        help="If > 0, apply a bitrate ramp event at this time (seconds).",
    )
    pb_p.add_argument(
        "--ramp-to", type=int, default=0,
        help="Post-ramp frame size in bytes; requires --ramp-at > 0.",
    )
    pb_p.add_argument(
        "--budget-schedule",
        type=str,
        default="",
        help=(
            "Comma-separated schedule of pps_budget changes, e.g. "
            "'0:3000,2:600,4:3000'. Overrides --pps-budget per frame."
        ),
    )
    pb_p.add_argument("--seed", type=int, default=0xC0FFEE)

    # --- benchmark ---
    bench_p = sub.add_parser("benchmark", help="Run benchmark scenarios with KPIs")
    bench_p.add_argument(
        "--realtime",
        action="store_true",
        help="Run at wall-clock speed (slow but realistic timing)",
    )
    bench_p.add_argument(
        "--replay",
        type=str,
        default="",
        help="Path to CSV with recorded frame data (timestamp_us,frame_size,is_iframe)",
    )
    bench_p.add_argument(
        "--csv-out",
        type=str,
        default="",
        help="Write frame trace CSV to this path",
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )

    if args.cmd == "run":
        config = ControllerConfig(
            mtu=args.mtu,
            min_update_interval=args.min_update_interval,
            enable_variable_payload=args.enable_variable_payload,
            target_fec_k=args.target_fec_k,
            min_payload=args.min_payload,
            mtu_override=args.mtu_override,
        )
        service = FECControllerService(
            config=config,
            stat_port=args.stat_port,
            wfb_control_host=args.wfb_host,
            wfb_control_port=args.wfb_port,
            dry_run=args.dry_run,
            sidecar_host=args.sidecar_host,
            sidecar_port=args.sidecar_port,
        )
        asyncio.run(service.run())
    elif args.cmd == "simulate":
        simulate_stream(
            fps=args.fps,
            base_frame_size=args.base_frame_size,
            duration_s=args.duration,
        )
    elif args.cmd == "table":
        print_reference_table()
    elif args.cmd == "payload-benchmark":
        events = []
        if args.ramp_at > 0 and args.ramp_to > 0:
            events.append((float(args.ramp_at), int(args.ramp_to)))
        profile = SizeProfile(
            base=args.base,
            i_mult=args.i_mult,
            gop_interval=args.gop,
            jitter_sigma=args.jitter,
            bitrate_events=events,
        )
        budget_profile = LinkBudgetProfile()
        if args.budget_schedule:
            try:
                for token in args.budget_schedule.split(","):
                    t_str, pps_str = token.strip().split(":")
                    budget_profile.events.append(
                        (float(t_str), float(pps_str))
                    )
            except (ValueError, IndexError) as exc:
                parser.error(
                    f"--budget-schedule must be 't:pps,t:pps,...' — {exc}"
                )
        cfg = PayloadBenchmarkConfig(
            fps=args.fps,
            frames=args.frames,
            fec_k=args.fec_k,
            profile=profile,
            pps_budget=args.pps_budget,
            budget_profile=budget_profile,
            min_payload=args.min_payload,
            fixed_payload=args.fixed_payload,
            mtu_override=args.mtu_override,
            hysteresis=args.hysteresis,
            seed=args.seed,
        )
        result = compare_policies(cfg)
        print(format_report(result))
    elif args.cmd == "benchmark":
        if args.replay:
            results = [replay(
                args.replay,
                realtime=args.realtime,
                scenario=args.replay,
            )]
        else:
            results = run_all(realtime=args.realtime)
        for r in results:
            print(r.kpi.summary())
            print()
            if args.csv_out:
                path = args.csv_out
                if len(results) > 1:
                    base, ext = path.rsplit(".", 1) if "." in path else (path, "csv")
                    path = f"{base}_{r.kpi.scenario}.{ext}"
                with open(path, "w") as f:
                    f.write(r.to_csv())
                print(f"  -> {path}")
    else:
        print_reference_table()
        print()
        simulate_stream()
