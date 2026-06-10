"""
Boundary-probe PER record — wire format frozen for Phase 4.

One format is locked here:

**probe record** — single-line UTF-8 JSON datagram (newline-terminated),
emitted by `gs_supervisor` (probe-flagged rx tunnel, `probe_window_flush()`)
onto the tunnel's `stats_out` (the uplink back-channel, udp_in 6600), riding
the rx_ant tunnel to the vehicle's `link_controller` (`--stats :5801`).

    {"type":"probe","ts_ms":...,"radio_port":50,"mcs":7,"per":0.1,
     "recv":18,"lost":2,"accounted":20,"rssi":-55,"window_s":0.5}

Consumer contract (link_controller.c probe ingest):
  * REQUIRED keys: ``mcs`` (int, 0..15), ``accounted`` (int), ``lost`` (int).
    Everything else is observability-only and may be extended.
  * ``per`` is never parsed by the consumer; it derives integer per-mille:
        per_milli = (lost*1000 + accounted//2) // accounted   (round-half-up)
        per_milli = -1 when accounted == 0  (INVALID -> hold, never demote)
  * ``rssi`` may be JSON null (no antenna reading in the window).
  * The record must contain the literal token ``"type":"probe"`` (compact,
    no space after the colon) — the vehicle demux is strstr-based.

Producer notes: one record per non-empty *received*-MCS bucket per window
(default 500 ms); a window straddling a probe retune emits one clean record
per MCS. Records with accounted == 0 are never emitted.
"""

import json
from dataclasses import dataclass

PROBE_MCS_MAX = 16  # link_controller.c rung table size


@dataclass
class ProbeRecord:
    ts_ms: int
    radio_port: int
    mcs: int
    per: float | None
    recv: int
    lost: int
    accounted: int
    rssi: int | None
    window_s: float


def encode_probe(r: ProbeRecord) -> bytes:
    """Compact single-line JSON, exactly as gs_supervisor emits it."""
    obj = {
        "type": "probe",
        "ts_ms": r.ts_ms,
        "radio_port": r.radio_port,
        "mcs": r.mcs,
        "per": r.per,
        "recv": r.recv,
        "lost": r.lost,
        "accounted": r.accounted,
        "rssi": r.rssi,
        "window_s": r.window_s,
    }
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode()


def parse_probe(payload: bytes) -> ProbeRecord | None:
    """Parse + validate against the consumer contract. None on reject."""
    try:
        obj = json.loads(payload.decode())
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(obj, dict) or obj.get("type") != "probe":
        return None
    # Consumer-required keys.
    for key in ("mcs", "accounted", "lost"):
        if not isinstance(obj.get(key), int):
            return None
    if not (0 <= obj["mcs"] < PROBE_MCS_MAX):
        return None
    return ProbeRecord(
        ts_ms=int(obj.get("ts_ms", 0)),
        radio_port=int(obj.get("radio_port", -1)),
        mcs=obj["mcs"],
        per=obj.get("per"),
        recv=int(obj.get("recv", 0)),
        lost=obj["lost"],
        accounted=obj["accounted"],
        rssi=obj.get("rssi"),
        window_s=float(obj.get("window_s", 0.0)),
    )


def per_milli(accounted: int, lost: int) -> int:
    """The vehicle-side derivation (link_controller.c probe ingest),
    integer round-half-up per-mille; -1 = invalid (no traffic)."""
    if accounted <= 0:
        return -1
    return (lost * 1000 + accounted // 2) // accounted
