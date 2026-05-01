#!/usr/bin/env python3
"""Bridge wfb_rx -Y JSON stats into ELRS Backpack PTR injection.

Why
---
link_controller (vehicle side) currently consumes wfb_rx -Y "rx_ant" JSON
over UDP. This script provides an alternative transport that piggy-backs on
the existing ELRS uplink: ground-side reception quality is encoded into the
3 PTR slots of MSP_ELRS_BACKPACK_SET_PTR (function 0x0383) and forwarded
over ESP-NOW by a USB-CDC-attached TX Backpack. A future vehicle-side
decoder will read those 3 RC channels and synthesize a compatible rx_ant
JSON for link_controller's :5801 listener.

The 3 slots (default mapping pan/roll/tilt; remap with --map):
  - sel  : selector at 30 Hz, rotates {1100=loss%, 1500=fec_recov%, 1900=adapters}
  - val  : value for the currently-selected field, 1000..2000
  - rssi : effective RSSI (best-antenna avg dBm), pinned at 30 Hz

Failsafe: if no rx_ant within --stale-timeout seconds, send 1000/1000/1000.
The vehicle decoder should treat sel=1000 as "no data" and synthesize
"100% loss + RSSI=lo" so link_controller drops to bucket 0.

Usage
-----
    wfb_rx -Y 127.0.0.1:5801 ...
    ./wfb_rx_to_backpack.py --listen 0.0.0.0:5801

Dependencies: pyserial >= 3.5
"""

from __future__ import annotations

import argparse
import json
import logging
import select
import signal
import socket
import struct
import sys
import time
from dataclasses import dataclass
from typing import Optional

import serial
from serial.tools import list_ports


# ---- MSP v2 framing (vendored from Waybeam-backpack-android/tools/msp.py) ---

def _crsf_crc8(data: bytes, poly: int = 0xD5, init: int = 0) -> int:
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def msp_encode(function: int, payload: bytes = b"", direction: str = "<", flag: int = 0) -> bytes:
    body = bytes([flag]) + struct.pack("<HH", function, len(payload)) + payload
    return b"$X" + direction.encode("ascii") + body + bytes([_crsf_crc8(body)])


MSP_ELRS_BACKPACK_SET_HEAD_TRACKING = 0x030D
MSP_ELRS_BACKPACK_SET_PTR = 0x0383

ESPRESSIF_VID = 0x303A
ESPRESSIF_PID = 0x1001


# ---- channel encoding -------------------------------------------------------

CRSF_LOW = 1000
CRSF_HIGH = 2000

# Selector field IDs (CRSF microsecond bands; 400 us spacing for clean decode).
SEL_LOSS = 1100
SEL_FEC = 1500
SEL_ADAPTERS = 1900

FAILSAFE_VALUE = 1000


def _clamp(v: int) -> int:
    if v < CRSF_LOW:
        return CRSF_LOW
    if v > CRSF_HIGH:
        return CRSF_HIGH
    return v


def encode_pct(pct: float) -> int:
    if pct < 0.0:
        pct = 0.0
    if pct > 100.0:
        pct = 100.0
    return _clamp(int(round(CRSF_LOW + pct * 10.0)))


def encode_count(n: int, scale: int = 100) -> int:
    return _clamp(CRSF_LOW + max(0, n) * scale)


def encode_rssi(dbm: float, lo_dbm: float, hi_dbm: float) -> int:
    if dbm <= lo_dbm:
        return CRSF_LOW
    if dbm >= hi_dbm:
        return CRSF_HIGH
    span = hi_dbm - lo_dbm
    return _clamp(int(round(CRSF_LOW + (dbm - lo_dbm) / span * 1000.0)))


# ---- rx_ant aggregation -----------------------------------------------------

@dataclass
class RxAntStats:
    uniq: int = 0
    lost: int = 0
    fec_recovered: int = 0
    adapters: int = 0
    rssi_best_avg: Optional[float] = None
    rx_ts: float = 0.0

    @property
    def loss_pct(self) -> float:
        return 100.0 * self.lost / self.uniq if self.uniq > 0 else 0.0

    @property
    def fec_pct(self) -> float:
        return 100.0 * self.fec_recovered / self.uniq if self.uniq > 0 else 0.0


def parse_rx_ant(raw: bytes) -> Optional[RxAntStats]:
    """Parse one wfb_rx -Y rx_ant datagram. Returns None for non-rx_ant or bad shape.

    Shape (matches link_controller.c parser, line 4088-4116):
        {"type":"rx_ant","ver":1,
         "pkt":{"uniq":N,"lost":N,"fec_recovered":N,"adapters":N,...},
         <one or more antenna entries with "rssi":{"avg":N,"max":N}>}
    """
    try:
        j = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(j, dict):
        return None
    if j.get("type") != "rx_ant":
        return None
    try:
        if int(j.get("ver", 0)) != 1:
            return None
    except (TypeError, ValueError):
        return None

    s = RxAntStats(rx_ts=time.monotonic())

    pkt = j.get("pkt") if isinstance(j.get("pkt"), dict) else {}
    uniq = pkt.get("uniq")
    if uniq is None:
        uniq = pkt.get("data", 0)
    try:
        s.uniq = max(0, int(uniq))
    except (TypeError, ValueError):
        s.uniq = 0
    for fld in ("lost", "fec_recovered"):
        try:
            setattr(s, fld, max(0, int(pkt.get(fld, 0))))
        except (TypeError, ValueError):
            pass
    try:
        s.adapters = max(0, int(pkt.get("adapters", 0)))
    except (TypeError, ValueError):
        s.adapters = 0

    # Best-avg RSSI across every antenna entry — mirrors AGG_BEST_AVG default.
    best_avg: Optional[float] = None
    candidates = []
    for key in ("rx_ant", "rx_ant_stats", "antennas"):
        v = j.get(key)
        if isinstance(v, list):
            candidates.extend(v)
        elif isinstance(v, dict):
            candidates.extend(v.values())
    # Some emitters put antenna entries flat at top level — skim every dict
    # value with an "rssi" subobject.
    for v in j.values():
        if isinstance(v, dict) and "rssi" in v:
            candidates.append(v)
    seen = set()
    for ant in candidates:
        if not isinstance(ant, dict):
            continue
        ident = id(ant)
        if ident in seen:
            continue
        seen.add(ident)
        rssi = ant.get("rssi")
        if not isinstance(rssi, dict):
            continue
        try:
            avg = float(rssi.get("avg"))
        except (TypeError, ValueError):
            continue
        if best_avg is None or avg > best_avg:
            best_avg = avg
    s.rssi_best_avg = best_avg
    return s


# ---- Backpack USB transport -------------------------------------------------

class Backpack:
    """USB-CDC writer: send PTR frames, auto-reconnect with exponential backoff."""

    def __init__(self, device: Optional[str], baud: int = 460800,
                 reconnect_min: float = 0.5, reconnect_max: float = 5.0):
        self.device = device
        self.baud = baud
        self.reconnect_min = reconnect_min
        self.reconnect_max = reconnect_max
        self._ser: Optional[serial.Serial] = None
        self._next_attempt = 0.0
        self._backoff = reconnect_min
        self._log = logging.getLogger("backpack")

    @staticmethod
    def autodetect() -> Optional[str]:
        for p in list_ports.comports():
            if (p.vid, p.pid) == (ESPRESSIF_VID, ESPRESSIF_PID):
                return p.device
        for p in list_ports.comports():
            if p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB"):
                return p.device
        return None

    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def ensure_open(self) -> bool:
        if self.is_open():
            return True
        now = time.monotonic()
        if now < self._next_attempt:
            return False
        port = self.device or self.autodetect()
        if port is None:
            self._log.warning("no Backpack USB device found; retry in %.1fs", self._backoff)
            self._schedule_retry()
            return False
        try:
            self._ser = serial.Serial(port=port, baudrate=self.baud, timeout=0)
        except (serial.SerialException, OSError) as e:
            self._log.warning("open %s failed: %s; retry in %.1fs", port, e, self._backoff)
            self._ser = None
            self._schedule_retry()
            return False
        self._log.info("connected to %s @ %d", port, self.baud)
        try:
            self._ser.write(msp_encode(MSP_ELRS_BACKPACK_SET_HEAD_TRACKING, bytes([1])))
        except (serial.SerialException, OSError) as e:
            self._log.warning("enable head-tracking failed: %s", e)
            self._close_port()
            self._schedule_retry()
            return False
        self._backoff = self.reconnect_min
        return True

    def send_ptr(self, pan: int, roll: int, tilt: int) -> bool:
        if not self.ensure_open():
            return False
        payload = struct.pack("<hhh", pan, roll, tilt)
        try:
            assert self._ser is not None
            self._ser.write(msp_encode(MSP_ELRS_BACKPACK_SET_PTR, payload))
        except (serial.SerialException, OSError) as e:
            self._log.warning("write failed: %s; closing port", e)
            self._close_port()
            self._schedule_retry()
            return False
        return True

    def _schedule_retry(self) -> None:
        self._next_attempt = time.monotonic() + self._backoff
        self._backoff = min(self._backoff * 2.0, self.reconnect_max)

    def _close_port(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    def close(self) -> None:
        self._close_port()


# ---- multiplex scheduler ----------------------------------------------------

class Multiplex:
    def __init__(self, rotation=(SEL_LOSS, SEL_FEC, SEL_ADAPTERS)):
        self.rotation = rotation
        self.idx = 0

    def next(self) -> int:
        sel = self.rotation[self.idx % len(self.rotation)]
        self.idx += 1
        return sel


# ---- slot mapping -----------------------------------------------------------

PTR_SLOTS = ("pan", "roll", "tilt")
LOGICAL = ("sel", "val", "rssi")


def parse_slot_map(spec: str) -> dict:
    """`sel=pan,val=roll,rssi=tilt` -> {'sel':'pan','val':'roll','rssi':'tilt'}."""
    out: dict = {}
    for kv in spec.split(","):
        kv = kv.strip()
        if not kv:
            continue
        k, _, v = kv.partition("=")
        k = k.strip().lower()
        v = v.strip().lower()
        if k not in LOGICAL or v not in PTR_SLOTS:
            raise argparse.ArgumentTypeError(f"bad map entry {kv!r}")
        out[k] = v
    if set(out.keys()) != set(LOGICAL):
        raise argparse.ArgumentTypeError(f"--map must define {','.join(LOGICAL)}")
    if len(set(out.values())) != 3:
        raise argparse.ArgumentTypeError("--map slots must be unique")
    return out


def build_ptr(sel_v: int, val_v: int, rssi_v: int, slot_map: dict) -> tuple[int, int, int]:
    by_logical = {"sel": sel_v, "val": val_v, "rssi": rssi_v}
    inv = {v: k for k, v in slot_map.items()}  # 'pan' -> 'sel' etc.
    return (
        by_logical[inv["pan"]],
        by_logical[inv["roll"]],
        by_logical[inv["tilt"]],
    )


# ---- main loop --------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("-d", "--device",
                    help="USB serial device (default: autodetect VID:PID 303A:1001)")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--listen", default="0.0.0.0:5801",
                    help="UDP listener for wfb_rx -Y stats (default 0.0.0.0:5801)")
    ap.add_argument("--rate", type=float, default=30.0,
                    help="PTR send rate Hz cap (default 30)")
    ap.add_argument("--stale-timeout", type=float, default=2.0,
                    help="seconds with no rx_ant before failsafe (default 2.0)")
    ap.add_argument("--rssi-min", type=float, default=-100.0)
    ap.add_argument("--rssi-max", type=float, default=0.0)
    ap.add_argument("--map", type=parse_slot_map,
                    default=parse_slot_map("sel=pan,val=roll,rssi=tilt"),
                    dest="slot_map",
                    help="logical->PTR slot map (default: sel=pan,val=roll,rssi=tilt)")
    ap.add_argument("--dry-run", action="store_true",
                    help="don't open USB; log what would have been sent")
    ap.add_argument("--log-json", action="store_true",
                    help="log each rx_ant frame received")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    log = logging.getLogger("main")

    host, _, port_s = args.listen.rpartition(":")
    if not host:
        host = "0.0.0.0"
    listen_port = int(port_s)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, listen_port))
    sock.setblocking(False)
    log.info("listening for wfb_rx -Y on %s:%d", host, listen_port)

    bp: Optional[Backpack] = None
    if not args.dry_run:
        bp = Backpack(args.device, baud=args.baud)

    last: Optional[RxAntStats] = None
    mux = Multiplex()
    period = 1.0 / args.rate
    next_tick = time.monotonic()
    in_failsafe = False
    stop = False

    def _sig(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    while not stop:
        timeout = max(0.0, next_tick - time.monotonic())
        try:
            r, _, _ = select.select([sock], [], [], timeout)
        except InterruptedError:
            continue
        if r:
            while True:
                try:
                    data, _addr = sock.recvfrom(65535)
                except BlockingIOError:
                    break
                stats = parse_rx_ant(data)
                if stats is None:
                    log.debug("skip non-rx_ant datagram (%d bytes)", len(data))
                    continue
                last = stats
                if args.log_json:
                    log.info("rx_ant uniq=%d lost=%d fec=%d adapters=%d rssi=%s",
                             stats.uniq, stats.lost, stats.fec_recovered,
                             stats.adapters,
                             f"{stats.rssi_best_avg:.1f}" if stats.rssi_best_avg is not None else "n/a")

        now = time.monotonic()
        if now < next_tick:
            continue
        next_tick += period
        if next_tick < now - period:
            # Fell behind (e.g. blocked on USB). Resync without spamming.
            next_tick = now + period

        stale = last is None or (now - last.rx_ts) > args.stale_timeout
        if stale:
            sel_v = val_v = rssi_v = FAILSAFE_VALUE
            if not in_failsafe:
                log.warning("failsafe: no rx_ant within %.1fs — sending %d/%d/%d",
                            args.stale_timeout, FAILSAFE_VALUE, FAILSAFE_VALUE, FAILSAFE_VALUE)
                in_failsafe = True
        else:
            if in_failsafe:
                log.info("failsafe cleared — rx_ant resumed")
                in_failsafe = False
            sel_id = mux.next()
            assert last is not None
            if sel_id == SEL_LOSS:
                val_v = encode_pct(last.loss_pct)
            elif sel_id == SEL_FEC:
                val_v = encode_pct(last.fec_pct)
            else:  # SEL_ADAPTERS
                val_v = encode_count(last.adapters, scale=100)
            sel_v = sel_id
            rssi_dbm = last.rssi_best_avg if last.rssi_best_avg is not None else args.rssi_min
            rssi_v = encode_rssi(rssi_dbm, args.rssi_min, args.rssi_max)

        pan, roll, tilt = build_ptr(sel_v, val_v, rssi_v, args.slot_map)
        if args.verbose:
            log.debug("tx pan=%d roll=%d tilt=%d (sel=%d val=%d rssi=%d)",
                      pan, roll, tilt, sel_v, val_v, rssi_v)
        if bp is not None:
            bp.send_ptr(pan, roll, tilt)

    sock.close()
    if bp is not None:
        bp.close()
    log.info("shutdown")
    return 0


if __name__ == "__main__":
    sys.exit(main())
