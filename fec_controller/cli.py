"""CLI entry point for the FEC controller."""

import asyncio
import logging
import argparse

from fec_controller.config import ControllerConfig
from fec_controller.service import FECControllerService
from fec_controller.simulation import simulate_stream, print_reference_table
from fec_controller.benchmark import run_all, replay


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

    # --- simulate ---
    sim_p = sub.add_parser("simulate", help="Run simulation")
    sim_p.add_argument("--fps", type=int, default=120)
    sim_p.add_argument("--base-frame-size", type=int, default=5000)
    sim_p.add_argument("--duration", type=float, default=8.0)

    # --- table ---
    sub.add_parser("table", help="Print reference table")

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
