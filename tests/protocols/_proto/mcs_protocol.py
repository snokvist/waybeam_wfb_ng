"""
Wire protocols consumed and emitted by mcs_selector.

Two formats are locked here:

1. **wfb_rx -Y stats stream** — single-line UTF-8 JSON datagram per
   `-l log_interval` (newline-terminated). Emitted by patched wfb_rx,
   schema `type:"rx_ant" ver:1`. See poc/SHM_HOWTO.md.

2. **wfb_tx control protocol** — binary request/response over UDP.
   CMD_SET_RADIO (cmd_id=2) writes the full radiotap header in one
   packet; CMD_GET_RADIO (cmd_id=4) reads it back. Structs match
   upstream wfb-ng src/tx_cmd.h verbatim.

Setting only mcs_index requires preserving the other radiotap fields
(bandwidth, stbc, ldpc, short_gi, vht_mode, vht_nss); blanks would
overwrite the header with garbage. The selector pulls those fields via
CMD_GET_RADIO at startup and re-reads after every successful SET.
"""

import json
import socket
import struct
from dataclasses import dataclass

# ---- wfb_tx control commands (tx_cmd.h) -----------------------------------

CMD_SET_FEC = 1
CMD_SET_RADIO = 2
CMD_GET_FEC = 3
CMD_GET_RADIO = 4

# cmd_req_t header: uint32_t req_id (network byte order), uint8_t cmd_id.
# Followed by the command-specific union member, packed with no padding.
_REQ_HEADER = struct.Struct("!IB")  # 5 bytes

# cmd_set_radio union member (7 bytes, packed):
#   uint8_t stbc; bool ldpc; bool short_gi;
#   uint8_t bandwidth; uint8_t mcs_index;
#   bool vht_mode; uint8_t vht_nss;
# bool is _Bool == 1 byte under packed.
_RADIO_BODY = struct.Struct("BBBBBBB")  # 7 bytes
_REQ_SET_RADIO = struct.Struct("!IBBBBBBBB")  # 12 bytes total

# cmd_resp_t header: uint32_t req_id, uint32_t rc (both network byte order).
_RESP_HEADER = struct.Struct("!II")  # 8 bytes
_RESP_GET_RADIO = struct.Struct("!IIBBBBBBB")  # 8 + 7 = 15 bytes


@dataclass
class RadioParams:
    """Mirrors cmd_set_radio / cmd_get_radio. All fields are bytes on the
    wire; bools are 0/1.
    """
    stbc: int
    ldpc: int
    short_gi: int
    bandwidth: int
    mcs_index: int
    vht_mode: int
    vht_nss: int

    def with_mcs(self, mcs_index: int) -> "RadioParams":
        return RadioParams(
            stbc=self.stbc,
            ldpc=self.ldpc,
            short_gi=self.short_gi,
            bandwidth=self.bandwidth,
            mcs_index=mcs_index,
            vht_mode=self.vht_mode,
            vht_nss=self.vht_nss,
        )


def pack_set_radio(req_id: int, params: RadioParams) -> bytes:
    """Build the 12-byte CMD_SET_RADIO request."""
    return _REQ_SET_RADIO.pack(
        req_id & 0xFFFFFFFF,
        CMD_SET_RADIO,
        params.stbc & 0xFF,
        1 if params.ldpc else 0,
        1 if params.short_gi else 0,
        params.bandwidth & 0xFF,
        params.mcs_index & 0xFF,
        1 if params.vht_mode else 0,
        params.vht_nss & 0xFF,
    )


def pack_get_radio(req_id: int) -> bytes:
    """Build the 5-byte CMD_GET_RADIO request (no body)."""
    return _REQ_HEADER.pack(req_id & 0xFFFFFFFF, CMD_GET_RADIO)


def parse_get_radio_response(data: bytes) -> tuple[int, int, RadioParams]:
    """Parse a CMD_GET_RADIO response. Returns (req_id, rc, params)."""
    if len(data) != _RESP_GET_RADIO.size:
        raise ValueError(
            f"CMD_GET_RADIO response wrong size: {len(data)} != {_RESP_GET_RADIO.size}"
        )
    req_id, rc, stbc, ldpc, short_gi, bw, mcs, vht_mode, vht_nss = (
        _RESP_GET_RADIO.unpack(data)
    )
    return (
        req_id,
        rc,
        RadioParams(
            stbc=stbc,
            ldpc=ldpc,
            short_gi=short_gi,
            bandwidth=bw,
            mcs_index=mcs,
            vht_mode=vht_mode,
            vht_nss=vht_nss,
        ),
    )


def parse_set_radio_response(data: bytes) -> tuple[int, int]:
    """Parse a CMD_SET_RADIO response (header only). Returns (req_id, rc)."""
    if len(data) < _RESP_HEADER.size:
        raise ValueError(
            f"CMD_SET_RADIO response too short: {len(data)} < {_RESP_HEADER.size}"
        )
    req_id, rc = _RESP_HEADER.unpack(data[: _RESP_HEADER.size])
    return req_id, rc


# ---- wfb_rx -Y stats datagram (rx_ant ver 1) ------------------------------


@dataclass
class AntStats:
    freq: int
    mcs: int
    bw: int
    id: str
    pkts: int
    rssi_min: int
    rssi_avg: int
    rssi_max: int
    snr_min: int
    snr_avg: int
    snr_max: int


@dataclass
class PktStats:
    """Aggregate counters from the rx_ant `pkt` block.

    `lost` and `fec_recovered` together signal channel stress for the
    loss-penalty scorer. `data` is the denominator. Other fields are
    kept for diagnostics / future inputs.
    """
    all_: int
    bytes_: int
    dec_err: int
    session: int
    data: int
    uniq: int
    fec_recovered: int
    lost: int
    bad: int
    outgoing: int
    outgoing_bytes: int


@dataclass
class RxAntDatagram:
    ts_ms: int
    seq: int
    interval_ms: int
    ant: list[AntStats]
    pkt: PktStats


def parse_rx_ant(payload: bytes) -> RxAntDatagram | None:
    """Parse one wfb_rx -Y datagram. Returns None for unrelated/garbage
    datagrams (different `type` or wrong `ver`); raises ValueError only
    on malformed JSON or schema violations within an rx_ant ver 1 frame.
    """
    try:
        obj = json.loads(payload.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as e:
        raise ValueError(f"rx_ant: invalid JSON: {e}") from e

    if obj.get("type") != "rx_ant":
        return None
    if obj.get("ver") != 1:
        return None

    try:
        ant_list = []
        for entry in obj.get("ant") or []:
            rssi = entry.get("rssi") or {}
            snr = entry.get("snr") or {}
            ant_list.append(
                AntStats(
                    freq=int(entry["freq"]),
                    mcs=int(entry["mcs"]),
                    bw=int(entry["bw"]),
                    id=str(entry["id"]),
                    pkts=int(entry["pkts"]),
                    rssi_min=int(rssi["min"]),
                    rssi_avg=int(rssi["avg"]),
                    rssi_max=int(rssi["max"]),
                    snr_min=int(snr["min"]),
                    snr_avg=int(snr["avg"]),
                    snr_max=int(snr["max"]),
                )
            )

        p = obj.get("pkt") or {}
        pkt = PktStats(
            all_=int(p.get("all", 0)),
            bytes_=int(p.get("bytes", 0)),
            dec_err=int(p.get("dec_err", 0)),
            session=int(p.get("session", 0)),
            data=int(p.get("data", 0)),
            uniq=int(p.get("uniq", 0)),
            fec_recovered=int(p.get("fec_recovered", 0)),
            lost=int(p.get("lost", 0)),
            bad=int(p.get("bad", 0)),
            outgoing=int(p.get("outgoing", 0)),
            outgoing_bytes=int(p.get("outgoing_bytes", 0)),
        )

        return RxAntDatagram(
            ts_ms=int(obj["ts_ms"]),
            seq=int(obj.get("seq", 0)),
            interval_ms=int(obj.get("interval_ms", 0)),
            ant=ant_list,
            pkt=pkt,
        )
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError(f"rx_ant: schema mismatch: {e}") from e


def encode_rx_ant(d: RxAntDatagram) -> bytes:
    """Serialise an RxAntDatagram back to the on-wire JSON.  Used by the
    simulator and by tests that want to feed deterministic samples
    through the real parser."""
    obj = {
        "ts_ms": d.ts_ms,
        "type": "rx_ant",
        "ver": 1,
        "seq": d.seq,
        "interval_ms": d.interval_ms,
        "ant": [
            {
                "freq": a.freq,
                "mcs": a.mcs,
                "bw": a.bw,
                "id": a.id,
                "pkts": a.pkts,
                "rssi": {"min": a.rssi_min, "avg": a.rssi_avg, "max": a.rssi_max},
                "snr": {"min": a.snr_min, "avg": a.snr_avg, "max": a.snr_max},
            }
            for a in d.ant
        ],
        "pkt": {
            "all": d.pkt.all_,
            "bytes": d.pkt.bytes_,
            "dec_err": d.pkt.dec_err,
            "session": d.pkt.session,
            "data": d.pkt.data,
            "uniq": d.pkt.uniq,
            "fec_recovered": d.pkt.fec_recovered,
            "lost": d.pkt.lost,
            "bad": d.pkt.bad,
            "outgoing": d.pkt.outgoing,
            "outgoing_bytes": d.pkt.outgoing_bytes,
        },
    }
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")


def udp_send(sock: socket.socket, host: str, port: int, payload: bytes) -> None:
    """Tiny helper to keep send sites free of try/except boilerplate.
    Caller decides whether OSError is fatal."""
    sock.sendto(payload, (host, port))
