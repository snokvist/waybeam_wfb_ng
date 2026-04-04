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

# Verify sizes match the C packed structs
assert struct.calcsize(FRAME_BASE_FMT) == FRAME_BASE_SIZE, (
    f"FRAME_BASE_FMT size {struct.calcsize(FRAME_BASE_FMT)} != {FRAME_BASE_SIZE}"
)
assert struct.calcsize(ENC_TRAILER_FMT) == ENC_TRAILER_SIZE, (
    f"ENC_TRAILER_FMT size {struct.calcsize(ENC_TRAILER_FMT)} != {ENC_TRAILER_SIZE}"
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
