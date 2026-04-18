#!/usr/bin/env python3
"""
Ground-side RSSI forwarder for the fec_controller POC.

Reads wfb_rx stdout (stdin by default, or a spawned wfb_rx subprocess, or a
log file), filters RX_ANT lines, and forwards each line verbatim as a UDP
datagram to a target (typically the local ground wfb_tx's -u input, which
then encapsulates and sends over the uplink to the vehicle, where another
wfb_rx decapsulates and emits to fec_controller's --rssi-udp listener).

Usage:
    # Pipe from an already-running wfb_rx:
    ./wfb_rx -K key -i 207 -x -p 0 wlx40a5ef2f229b 2>&1 \
      | python3 ground_rssi_forwarder.py --target 127.0.0.1:5700
    #                                                 ^
    # Use 2>&1 to also surface wfb_rx's "N packets lost" reports.
    # Without it they go to the terminal; with it the forwarder
    # logs them as WARNING lines.

    # Spawn wfb_rx as a subprocess:
    python3 ground_rssi_forwarder.py \
      --spawn './wfb_rx -K key -i 207 -x -p 0 wlx40a5ef2f229b' \
      --target 127.0.0.1:5700

    # Tail a log file that a separate wfb_rx is writing to:
    python3 ground_rssi_forwarder.py \
      --tail /var/log/wfb_rx.log --target 127.0.0.1:5700

Throttling: --throttle-hz caps forwarded lines per second (default 4).
Set to 0 to disable throttling (ship everything).
"""

import argparse
import logging
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time

log = logging.getLogger("rssi_fwd")

RX_ANT_TAG = "RX_ANT"
PACKETS_LOST_RE = re.compile(r"^\s*(\d+)\s+packets\s+lost\b", re.IGNORECASE)


def open_source(args):
    """Return a line-yielding iterator over one of the supported sources."""
    if args.spawn:
        argv = shlex.split(args.spawn)
        log.info("spawning: %s", " ".join(argv))
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
            text=True,
        )
        return _iter_lines(proc.stdout, proc=proc)

    if args.tail:
        log.info("tailing: %s", args.tail)
        # Open read-only, non-blocking; seek to end so we ignore history.
        f = open(args.tail, "r", buffering=1)
        f.seek(0, os.SEEK_END)
        return _tail_follow(f)

    log.info("reading RX_ANT lines from stdin")
    return _iter_lines(sys.stdin)


def _iter_lines(fp, proc=None):
    try:
        for line in fp:
            yield line.rstrip("\n")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


def _tail_follow(fp):
    """Basic tail -f behavior on a text file."""
    while True:
        line = fp.readline()
        if not line:
            time.sleep(0.2)
            continue
        yield line.rstrip("\n")


def parse_target(s):
    host, _, port = s.rpartition(":")
    if not host or not port:
        raise ValueError(f"invalid --target {s!r}, expected HOST:PORT")
    return host, int(port)


class Throttle:
    """Token-bucket-ish rate limiter. hz<=0 disables."""

    def __init__(self, hz):
        self.hz = hz
        self.min_gap_s = (1.0 / hz) if hz > 0 else 0.0
        self.last_sent = 0.0

    def allow(self, now):
        if self.hz <= 0:
            return True
        if now - self.last_sent >= self.min_gap_s:
            self.last_sent = now
            return True
        return False


def main():
    ap = argparse.ArgumentParser(
        description="Ground RSSI forwarder for fec_controller POC",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--spawn", help="shell command to run as wfb_rx source")
    src.add_argument("--tail", help="path to a wfb_rx log file to tail")
    # Default source: stdin (no flag needed)

    ap.add_argument(
        "--target",
        required=True,
        help="destination HOST:PORT (typically the local ground wfb_tx -u input)",
    )
    ap.add_argument(
        "--throttle-hz",
        type=float,
        default=10.0,
        help=(
            "max RX_ANT lines forwarded per second (0 disables; default 10). "
            "Higher rates give UDP packet loss more redundancy: with 50-75%% "
            "uplink loss at high video airtime, 4 Hz often leaves the "
            "receiver below 2 Hz, which the 10 s fallback window can still "
            "trip. 10 Hz gives roughly 2.5-5 Hz arriving through the same "
            "loss, which survives brief bursts."
        ),
    )
    ap.add_argument(
        "--verbose", "-v", action="store_true", help="log each forwarded line"
    )
    ap.add_argument(
        "--print-skipped",
        action="store_true",
        help="log when we throttle-drop lines",
    )
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(name)s] %(message)s",
    )

    host, port = parse_target(args.target)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    log.info("forwarding RX_ANT → udp %s:%d (throttle=%.1f Hz)",
             host, port, args.throttle_hz)

    throttle = Throttle(args.throttle_hz)

    # Make sure we exit cleanly on Ctrl-C / SIGTERM.
    stop = {"flag": False}

    def on_sig(signum, _frame):
        log.info("caught signal %d, exiting", signum)
        stop["flag"] = True

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    sent = skipped = lost_events = lost_total = 0
    last_loss_log = 0.0
    try:
        for line in open_source(args):
            if stop["flag"]:
                break

            # Surface wfb_rx "N packets lost" reports so they're visible
            # in the same stream as our own logs. Merge stderr via shell
            # (e.g. `./wfb_rx ... 2>&1 | python3 ...`) for these to reach
            # us when using the pipe form.
            m = PACKETS_LOST_RE.match(line)
            if m:
                n = int(m.group(1))
                lost_events += 1
                lost_total += n
                now_mono = time.monotonic()
                if now_mono - last_loss_log >= 1.0:
                    log.warning("wfb_rx: %d packets lost (total=%d across %d events)",
                                n, lost_total, lost_events)
                    last_loss_log = now_mono
                continue

            if RX_ANT_TAG not in line:
                continue
            now = time.monotonic()
            if not throttle.allow(now):
                skipped += 1
                if args.print_skipped:
                    log.debug("skip (throttle): %s", line)
                continue
            pkt = (line + "\n").encode("ascii", errors="replace")
            try:
                sock.sendto(pkt, (host, port))
                sent += 1
                if args.verbose:
                    log.debug("→ %s", line)
            except OSError as e:
                log.warning("send failed: %s", e)
    finally:
        sock.close()
        log.info("exit: sent=%d throttle_dropped=%d packets_lost=%d (across %d events)",
                 sent, skipped, lost_total, lost_events)


if __name__ == "__main__":
    main()
