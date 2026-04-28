"""
Sidecar wire protocol — matches rtp_sidecar.h.

All sidecar messages share a 6-byte header (magic, version, msg_type) and
use network byte order (big-endian), packed structs.

Message types:
  SUBSCRIBE  (probe -> venc, 8 B)  — start/refresh frame subscription
  FRAME      (venc -> probe, 52 B base / 64 B with encoder trailer)
  SYNC_REQ   (probe -> venc, 16 B)
  SYNC_RESP  (venc -> probe, 32 B)

The FEC controller acts as a sidecar consumer: it sends SUBSCRIBE to keep
the subscription alive and parses incoming FRAME messages for frame_size,
frame_type, and timing (to derive fps from frame_ready_us intervals).

The FEC controller uses the real sidecar FRAME messages for all paths
including simulation and testing.
"""

import struct
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Sidecar constants (from rtp_sidecar.h)
# ---------------------------------------------------------------------------

SIDECAR_MAGIC = 0x52545053  # "RTPS" big-endian
SIDECAR_VERSION = 1

MSG_SUBSCRIBE = 1
MSG_FRAME = 2
MSG_SYNC_REQ = 3
MSG_SYNC_RESP = 4

FLAG_KEYFRAME = 0x01
FLAG_ENC_INFO = 0x02
FLAG_TRANSPORT_INFO = 0x04

FRAME_TYPE_P = 0
FRAME_TYPE_I = 1
FRAME_TYPE_IDR = 2

# Subscription TTL on the venc side
SUB_TTL_US = 5_000_000  # 5 seconds

# ---------------------------------------------------------------------------
# Wire formats (network byte order = big-endian, packed)
#
# Derived from the #pragma pack(push,1) structs in rtp_sidecar.h.
# ---------------------------------------------------------------------------

# Common header: magic(u32) + version(u8) + msg_type(u8)
HDR_FMT = "!IBB"
HDR_SIZE = struct.calcsize(HDR_FMT)  # 6 bytes

# RtpSidecarSubscribe: magic(4) + version(1) + msg_type(1) + _pad(2) = 8
SUBSCRIBE_FMT = "!IBB2s"
SUBSCRIBE_SIZE = 8

# RtpSidecarFrame (52 bytes, packed):
#   magic(4) version(1) msg_type(1) stream_id(1) flags(1)
#   ssrc(4) rtp_timestamp(4) frame_id(8) frame_ready_us(8)
#   seq_first(2) seq_count(2) capture_us(8) last_pkt_send_us(8)
FRAME_BASE_FMT = "!IBBBB II Q Q HH Q Q"
FRAME_BASE_SIZE = 52

# RtpSidecarEncInfoWire (12 bytes, packed):
#   frame_size_bytes(4) frame_type(1) qp(1) complexity(1) scene_change(1)
#   gop_state(1) idr_inserted(1) frames_since_idr(2)
ENC_TRAILER_FMT = "!I BBBB BB H"
ENC_TRAILER_SIZE = 12

# RtpSidecarFrameExt = base(52) + trailer(12) = 64 bytes
FRAME_EXT_SIZE = FRAME_BASE_SIZE + ENC_TRAILER_SIZE

# RtpSidecarTransportInfoWire (16 bytes, packed):
#   fill_pct(1) in_pressure(1) _pad(2) transport_drops(4)
#   pressure_drops(4) packets_sent(4)
TRANSPORT_TRAILER_FMT = "!BB2sIII"
TRANSPORT_TRAILER_SIZE = 16

# Verify sizes match the C packed structs
assert struct.calcsize(FRAME_BASE_FMT) == FRAME_BASE_SIZE, (
    f"FRAME_BASE_FMT size {struct.calcsize(FRAME_BASE_FMT)} != {FRAME_BASE_SIZE}"
)
assert struct.calcsize(ENC_TRAILER_FMT) == ENC_TRAILER_SIZE, (
    f"ENC_TRAILER_FMT size {struct.calcsize(ENC_TRAILER_FMT)} != {ENC_TRAILER_SIZE}"
)
assert struct.calcsize(TRANSPORT_TRAILER_FMT) == TRANSPORT_TRAILER_SIZE, (
    f"TRANSPORT_TRAILER_FMT size {struct.calcsize(TRANSPORT_TRAILER_FMT)}"
    f" != {TRANSPORT_TRAILER_SIZE}"
)


# ---------------------------------------------------------------------------
# Parsed frame data
# ---------------------------------------------------------------------------

@dataclass
class SidecarFrame:
    """Parsed sidecar FRAME message."""
    ssrc: int
    rtp_timestamp: int
    frame_id: int
    frame_ready_us: int
    seq_first: int
    seq_count: int
    capture_us: int
    last_pkt_send_us: int
    stream_id: int = 0
    flags: int = 0
    # Encoder trailer (only valid when has_enc_info is True)
    has_enc_info: bool = False
    frame_size_bytes: int = 0
    frame_type: int = FRAME_TYPE_P
    qp: int = 0
    complexity: int = 0
    scene_change: int = 0
    gop_state: int = 0
    idr_inserted: int = 0
    frames_since_idr: int = 0
    # Transport-stats trailer (only valid when has_transport_info is True).
    # Field semantics by transport — see rtp_sidecar.h. fill_pct, in_pressure
    # and pressure_drops are authoritative across shm:// / unix:// / udp://.
    # transport_drops and packets_sent are authoritative for shm:// in v0.9.2;
    # unix:// / udp:// emit zero pending socket-side instrumentation.
    has_transport_info: bool = False
    fill_pct: int = 0
    in_pressure: int = 0
    transport_drops: int = 0
    pressure_drops: int = 0
    packets_sent: int = 0


# ---------------------------------------------------------------------------
# Pack / unpack sidecar messages
# ---------------------------------------------------------------------------

def pack_subscribe() -> bytes:
    """Build a SUBSCRIBE message (8 bytes) to send to the venc sidecar."""
    return struct.pack(
        SUBSCRIBE_FMT, SIDECAR_MAGIC, SIDECAR_VERSION, MSG_SUBSCRIBE, b"\x00\x00"
    )


def parse_header(data: bytes) -> tuple[int, int, int] | None:
    """Parse the 6-byte sidecar header. Returns (magic, version, msg_type) or None."""
    if len(data) < HDR_SIZE:
        return None
    magic, version, msg_type = struct.unpack(HDR_FMT, data[:HDR_SIZE])
    if magic != SIDECAR_MAGIC or version != SIDECAR_VERSION:
        return None
    return magic, version, msg_type


def parse_frame(data: bytes) -> SidecarFrame | None:
    """Parse a FRAME message (52 or 64 bytes). Returns SidecarFrame or None."""
    if len(data) < FRAME_BASE_SIZE:
        return None

    (magic, version, msg_type, stream_id, flags,
     ssrc, rtp_timestamp, frame_id, frame_ready_us,
     seq_first, seq_count, capture_us, last_pkt_send_us
     ) = struct.unpack(FRAME_BASE_FMT, data[:FRAME_BASE_SIZE])

    if magic != SIDECAR_MAGIC or version != SIDECAR_VERSION:
        return None
    if msg_type != MSG_FRAME:
        return None

    frame = SidecarFrame(
        ssrc=ssrc,
        rtp_timestamp=rtp_timestamp,
        frame_id=frame_id,
        frame_ready_us=frame_ready_us,
        seq_first=seq_first,
        seq_count=seq_count,
        capture_us=capture_us,
        last_pkt_send_us=last_pkt_send_us,
        stream_id=stream_id,
        flags=flags,
    )

    if (flags & FLAG_ENC_INFO) and len(data) >= FRAME_EXT_SIZE:
        trailer = data[FRAME_BASE_SIZE:FRAME_EXT_SIZE]
        (frame_size_bytes, frame_type, qp, complexity, scene_change,
         gop_state, idr_inserted, frames_since_idr,
         ) = struct.unpack(ENC_TRAILER_FMT, trailer)

        frame.has_enc_info = True
        frame.frame_size_bytes = frame_size_bytes
        frame.frame_type = frame_type
        frame.qp = qp
        frame.complexity = complexity
        frame.scene_change = scene_change
        frame.gop_state = gop_state
        frame.idr_inserted = idr_inserted
        frame.frames_since_idr = frames_since_idr

    # The transport-stats trailer slides up to land directly after the base
    # frame when ENC_INFO is absent — the producer never emits an empty
    # ENC_INFO trailer just to keep the offset stable. Compute the offset
    # explicitly so all four flag combinations parse correctly.
    if flags & FLAG_TRANSPORT_INFO:
        transport_offset = FRAME_BASE_SIZE + (
            ENC_TRAILER_SIZE if (flags & FLAG_ENC_INFO) else 0
        )
        if len(data) >= transport_offset + TRANSPORT_TRAILER_SIZE:
            trailer = data[
                transport_offset : transport_offset + TRANSPORT_TRAILER_SIZE
            ]
            (fill_pct, in_pressure, _pad, transport_drops, pressure_drops,
             packets_sent) = struct.unpack(TRANSPORT_TRAILER_FMT, trailer)
            frame.has_transport_info = True
            frame.fill_pct = fill_pct
            frame.in_pressure = in_pressure
            frame.transport_drops = transport_drops
            frame.pressure_drops = pressure_drops
            frame.packets_sent = packets_sent

    return frame


def pack_frame(
    ssrc: int = 0,
    rtp_timestamp: int = 0,
    frame_id: int = 0,
    frame_ready_us: int = 0,
    seq_first: int = 0,
    seq_count: int = 0,
    capture_us: int = 0,
    last_pkt_send_us: int = 0,
    stream_id: int = 0,
    frame_size_bytes: int = 0,
    frame_type: int = FRAME_TYPE_P,
    qp: int = 0,
    complexity: int = 0,
    scene_change: int = 0,
    gop_state: int = 0,
    idr_inserted: int = 0,
    frames_since_idr: int = 0,
) -> bytes:
    """Build a FRAME message with encoder trailer (64 bytes). For testing."""
    flags = FLAG_ENC_INFO
    base = struct.pack(
        FRAME_BASE_FMT,
        SIDECAR_MAGIC, SIDECAR_VERSION, MSG_FRAME, stream_id, flags,
        ssrc, rtp_timestamp, frame_id, frame_ready_us,
        seq_first, seq_count, capture_us, last_pkt_send_us,
    )
    trailer = struct.pack(
        ENC_TRAILER_FMT,
        frame_size_bytes, frame_type, qp, complexity, scene_change,
        gop_state, idr_inserted, frames_since_idr,
    )
    return base + trailer


def pack_frame_base(
    ssrc: int = 0,
    rtp_timestamp: int = 0,
    frame_id: int = 0,
    frame_ready_us: int = 0,
    seq_first: int = 0,
    seq_count: int = 0,
    capture_us: int = 0,
    last_pkt_send_us: int = 0,
    stream_id: int = 0,
) -> bytes:
    """Build a base FRAME message without encoder trailer (52 bytes). For testing."""
    flags = 0
    return struct.pack(
        FRAME_BASE_FMT,
        SIDECAR_MAGIC, SIDECAR_VERSION, MSG_FRAME, stream_id, flags,
        ssrc, rtp_timestamp, frame_id, frame_ready_us,
        seq_first, seq_count, capture_us, last_pkt_send_us,
    )


def pack_frame_full(
    ssrc: int = 0,
    rtp_timestamp: int = 0,
    frame_id: int = 0,
    frame_ready_us: int = 0,
    seq_first: int = 0,
    seq_count: int = 0,
    capture_us: int = 0,
    last_pkt_send_us: int = 0,
    stream_id: int = 0,
    keyframe: bool = False,
    enc: dict | None = None,
    transport: dict | None = None,
) -> bytes:
    """Build a FRAME message with optional ENC_INFO and TRANSPORT_INFO trailers.

    Mirrors rtp_sidecar_send_frame_transport on the venc side: when ENC_INFO
    is absent the transport trailer slides up to offset 52, producing a
    52 + 16 = 68-byte packet. Used by tests to round-trip all four flag
    combinations.

    enc: dict with keys frame_size_bytes, frame_type, qp, complexity,
         scene_change, gop_state, idr_inserted, frames_since_idr
         (all default 0 / FRAME_TYPE_P).
    transport: dict with keys fill_pct, in_pressure, transport_drops,
         pressure_drops, packets_sent (all default 0).
    """
    flags = 0
    if keyframe:
        flags |= FLAG_KEYFRAME
    if enc is not None:
        flags |= FLAG_ENC_INFO
    if transport is not None:
        flags |= FLAG_TRANSPORT_INFO

    out = struct.pack(
        FRAME_BASE_FMT,
        SIDECAR_MAGIC, SIDECAR_VERSION, MSG_FRAME, stream_id, flags,
        ssrc, rtp_timestamp, frame_id, frame_ready_us,
        seq_first, seq_count, capture_us, last_pkt_send_us,
    )

    if enc is not None:
        out += struct.pack(
            ENC_TRAILER_FMT,
            enc.get("frame_size_bytes", 0),
            enc.get("frame_type", FRAME_TYPE_P),
            enc.get("qp", 0),
            enc.get("complexity", 0),
            enc.get("scene_change", 0),
            enc.get("gop_state", 0),
            enc.get("idr_inserted", 0),
            enc.get("frames_since_idr", 0),
        )

    if transport is not None:
        out += struct.pack(
            TRANSPORT_TRAILER_FMT,
            transport.get("fill_pct", 0),
            transport.get("in_pressure", 0),
            b"\x00\x00",
            transport.get("transport_drops", 0),
            transport.get("pressure_drops", 0),
            transport.get("packets_sent", 0),
        )

    return out
