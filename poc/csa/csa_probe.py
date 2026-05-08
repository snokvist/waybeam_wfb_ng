#!/usr/bin/env python3
"""Inject timestamped JSON frames into a wfb_tx UDP injector.

Used to characterize the cpe510 -> vehicle uplink for CSA timing budget:
  send N frames to <host:port> (cpe510 wfb_tx -u 5801), then on the vehicle
  capture lo:5801 to count arrivals and measure jitter.
"""
import argparse
import json
import socket
import time
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.2.2")
    ap.add_argument("--port", type=int, default=5801)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--interval-ms", type=float, default=100.0)
    ap.add_argument("--size", type=int, default=0,
                    help="pad payload to >= N bytes (0=natural size)")
    ap.add_argument("--tag", default="csa_probe")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    addr = (args.host, args.port)

    interval = args.interval_ms / 1000.0
    t_start = time.monotonic()
    for seq in range(args.count):
        target = t_start + seq * interval
        now = time.monotonic()
        if target > now:
            time.sleep(target - now)
        payload = {
            "type": args.tag,
            "seq": seq,
            "t_send_ns": time.monotonic_ns(),
            "t_wall_us": int(time.time() * 1_000_000),
        }
        body = json.dumps(payload, separators=(",", ":"))
        if args.size > len(body) + 12:
            payload["pad"] = "x" * (args.size - len(body) - 11)
            body = json.dumps(payload, separators=(",", ":"))
        body += "\n"
        sock.sendto(body.encode(), addr)

    elapsed = time.monotonic() - t_start
    print(f"sent={args.count} elapsed_s={elapsed:.3f} "
          f"target_rate_hz={1.0/interval:.1f}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
