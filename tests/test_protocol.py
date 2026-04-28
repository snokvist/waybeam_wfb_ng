"""Tests for sidecar wire protocol (rtp_sidecar.h)."""

import struct
import pytest

from fec_controller.protocol import (
    FRAME_BASE_SIZE,
    FRAME_EXT_SIZE,
    ENC_TRAILER_SIZE,
    TRANSPORT_TRAILER_SIZE,
    SUBSCRIBE_SIZE,
    SIDECAR_MAGIC,
    SIDECAR_VERSION,
    MSG_SUBSCRIBE,
    MSG_FRAME,
    FLAG_KEYFRAME,
    FLAG_ENC_INFO,
    FLAG_TRANSPORT_INFO,
    FRAME_TYPE_P,
    FRAME_TYPE_I,
    FRAME_TYPE_IDR,
    SidecarFrame,
    pack_subscribe,
    parse_header,
    parse_frame,
    pack_frame,
    pack_frame_base,
    pack_frame_full,
)


class TestWireConstants:
    """Verify sizes match the C packed structs from rtp_sidecar.h."""

    def test_subscribe_size(self):
        assert SUBSCRIBE_SIZE == 8

    def test_frame_base_size(self):
        assert FRAME_BASE_SIZE == 52

    def test_enc_trailer_size(self):
        assert ENC_TRAILER_SIZE == 12

    def test_frame_ext_size(self):
        assert FRAME_EXT_SIZE == 64

    def test_transport_trailer_size(self):
        assert TRANSPORT_TRAILER_SIZE == 16

    def test_flag_values(self):
        assert FLAG_KEYFRAME == 0x01
        assert FLAG_ENC_INFO == 0x02
        assert FLAG_TRANSPORT_INFO == 0x04

    def test_frame_type_values(self):
        assert FRAME_TYPE_P == 0
        assert FRAME_TYPE_I == 1
        assert FRAME_TYPE_IDR == 2


class TestSubscribe:

    def test_pack_subscribe_size(self):
        msg = pack_subscribe()
        assert len(msg) == 8

    def test_pack_subscribe_magic(self):
        msg = pack_subscribe()
        magic = struct.unpack("!I", msg[:4])[0]
        assert magic == SIDECAR_MAGIC

    def test_pack_subscribe_version_and_type(self):
        msg = pack_subscribe()
        assert msg[4] == SIDECAR_VERSION
        assert msg[5] == MSG_SUBSCRIBE


class TestParseHeader:

    def test_valid_header(self):
        data = struct.pack("!IBB", SIDECAR_MAGIC, SIDECAR_VERSION, MSG_FRAME)
        result = parse_header(data)
        assert result == (SIDECAR_MAGIC, SIDECAR_VERSION, MSG_FRAME)

    def test_wrong_magic_returns_none(self):
        data = struct.pack("!IBB", 0xDEADBEEF, SIDECAR_VERSION, MSG_FRAME)
        assert parse_header(data) is None

    def test_wrong_version_returns_none(self):
        data = struct.pack("!IBB", SIDECAR_MAGIC, 99, MSG_FRAME)
        assert parse_header(data) is None

    def test_too_short_returns_none(self):
        assert parse_header(b"\x00\x01") is None
        assert parse_header(b"") is None


class TestFrameRoundtrip:

    def test_pack_parse_extended_frame(self):
        data = pack_frame(
            ssrc=0x12345678,
            rtp_timestamp=90000,
            frame_id=42,
            frame_ready_us=1_000_000,
            seq_first=100,
            seq_count=8,
            capture_us=999_000,
            last_pkt_send_us=1_001_000,
            frame_size_bytes=12000,
            frame_type=FRAME_TYPE_I,
            qp=28,
            complexity=180,
            scene_change=1,
            gop_state=2,
            idr_inserted=1,
            frames_since_idr=0,
        )
        assert len(data) == 64

        frame = parse_frame(data)
        assert frame is not None
        assert frame.ssrc == 0x12345678
        assert frame.rtp_timestamp == 90000
        assert frame.frame_id == 42
        assert frame.frame_ready_us == 1_000_000
        assert frame.seq_first == 100
        assert frame.seq_count == 8
        assert frame.capture_us == 999_000
        assert frame.last_pkt_send_us == 1_001_000

        assert frame.has_enc_info is True
        assert frame.frame_size_bytes == 12000
        assert frame.frame_type == FRAME_TYPE_I
        assert frame.qp == 28
        assert frame.complexity == 180
        assert frame.scene_change == 1
        assert frame.gop_state == 2
        assert frame.idr_inserted == 1
        assert frame.frames_since_idr == 0

    def test_pack_parse_base_frame(self):
        data = pack_frame_base(
            ssrc=0xAABBCCDD,
            rtp_timestamp=180000,
            frame_id=99,
            frame_ready_us=2_000_000,
            seq_first=200,
            seq_count=4,
            capture_us=1_999_000,
            last_pkt_send_us=2_001_000,
        )
        assert len(data) == 52

        frame = parse_frame(data)
        assert frame is not None
        assert frame.ssrc == 0xAABBCCDD
        assert frame.frame_id == 99
        assert frame.has_enc_info is False
        assert frame.frame_size_bytes == 0
        assert frame.frame_type == FRAME_TYPE_P

    def test_parse_frame_wrong_msg_type(self):
        """A SUBSCRIBE message should not parse as a FRAME."""
        data = pack_subscribe() + b"\x00" * 56  # pad to 64 bytes
        assert parse_frame(data) is None

    def test_parse_frame_too_short(self):
        data = pack_frame()[:20]
        assert parse_frame(data) is None

    def test_stream_id_preserved(self):
        data = pack_frame(stream_id=3)
        frame = parse_frame(data)
        assert frame.stream_id == 3

    def test_flags_enc_info_set(self):
        data = pack_frame(frame_size_bytes=5000)
        frame = parse_frame(data)
        assert frame.flags & FLAG_ENC_INFO

    def test_large_frame_id(self):
        data = pack_frame(frame_id=2**63)
        frame = parse_frame(data)
        assert frame.frame_id == 2**63

    def test_frame_type_idr(self):
        data = pack_frame(frame_type=FRAME_TYPE_IDR)
        frame = parse_frame(data)
        assert frame.frame_type == FRAME_TYPE_IDR

    def test_base_frame_fallback_no_enc_info(self):
        """Base-only frame: seq_count available, no frame_size_bytes."""
        data = pack_frame_base(seq_count=7)
        frame = parse_frame(data)
        assert frame.has_enc_info is False
        assert frame.seq_count == 7
        # Service would estimate frame_size = seq_count * MTU

    def test_all_encoder_trailer_fields(self):
        """Verify every encoder trailer field survives roundtrip."""
        data = pack_frame(
            frame_size_bytes=44000,
            frame_type=FRAME_TYPE_IDR,
            qp=22,
            complexity=200,
            scene_change=1,
            gop_state=3,
            idr_inserted=1,
            frames_since_idr=300,
        )
        frame = parse_frame(data)
        assert frame.frame_size_bytes == 44000
        assert frame.frame_type == FRAME_TYPE_IDR
        assert frame.qp == 22
        assert frame.complexity == 200
        assert frame.scene_change == 1
        assert frame.gop_state == 3
        assert frame.idr_inserted == 1
        assert frame.frames_since_idr == 300


class TestMalformedPackets:
    """Malformed packet handling critical for C port safety."""

    def test_enc_info_flag_but_short_data(self):
        """FLAG_ENC_INFO set but data < 64 bytes: trailer silently skipped."""
        base = pack_frame_base(seq_count=4)
        data = bytearray(base)
        data[7] = FLAG_ENC_INFO  # set flag manually on 52-byte packet
        frame = parse_frame(bytes(data))
        assert frame is not None
        assert frame.has_enc_info is False  # too short for trailer
        assert frame.seq_count == 4

    def test_truncated_at_every_boundary(self):
        """Truncation at various offsets: None or graceful degradation."""
        full = pack_frame(frame_size_bytes=5000)
        # Below base size: always None
        for length in [0, 1, 6, 10, 30, 51]:
            assert parse_frame(full[:length]) is None, f"len={length}"
        # Exactly base size with enc_info flag: parses base, no trailer
        frame_52 = parse_frame(full[:52])
        assert frame_52 is not None
        assert frame_52.has_enc_info is False
        # Between base and ext: parses base, no trailer
        frame_55 = parse_frame(full[:55])
        assert frame_55 is not None
        assert frame_55.has_enc_info is False
        # Full ext: trailer parsed
        frame_64 = parse_frame(full[:64])
        assert frame_64 is not None
        assert frame_64.has_enc_info is True
        assert frame_64.frame_size_bytes == 5000

    def test_extra_trailing_data(self):
        """Extra bytes after 64-byte frame are ignored."""
        data = pack_frame(frame_size_bytes=5000) + b"\xFF" * 100
        frame = parse_frame(data)
        assert frame is not None
        assert frame.has_enc_info is True
        assert frame.frame_size_bytes == 5000

    def test_all_zero_packet(self):
        """All-zero packet: magic=0 -> rejected."""
        assert parse_frame(b"\x00" * 64) is None

    def test_all_zero_fields_valid_header(self):
        """Valid header but all payload fields zero."""
        data = pack_frame(
            ssrc=0, rtp_timestamp=0, frame_id=0, frame_ready_us=0,
            seq_first=0, seq_count=0, capture_us=0, last_pkt_send_us=0,
            frame_size_bytes=0, frame_type=0, qp=0,
        )
        frame = parse_frame(data)
        assert frame is not None
        assert frame.ssrc == 0
        assert frame.seq_count == 0
        assert frame.frame_size_bytes == 0

    def test_wrong_magic_in_full_frame(self):
        """Full 64-byte frame with corrupted magic."""
        data = bytearray(pack_frame(frame_size_bytes=5000))
        data[0] = 0xFF  # corrupt magic
        assert parse_frame(bytes(data)) is None

    def test_wrong_version_in_full_frame(self):
        """Full 64-byte frame with wrong version."""
        data = bytearray(pack_frame(frame_size_bytes=5000))
        data[4] = 99  # corrupt version
        assert parse_frame(bytes(data)) is None


class TestTransportInfoTrailer:
    """Verify the transport-stats trailer mirrors waybeam_venc's
    test_star6e_video_sidecar_transport_layouts (the producer-side wire
    layout test in tests/test_star6e_video.c)."""

    def test_case1_no_enc_no_transport(self):
        """52 bytes, no flags."""
        data = pack_frame_full(frame_id=100)
        assert len(data) == 52
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags == 0
        assert frame.has_enc_info is False
        assert frame.has_transport_info is False

    def test_case2_enc_only(self):
        """64 bytes, ENC_INFO flag."""
        data = pack_frame_full(
            frame_id=200,
            enc={"frame_size_bytes": 5000, "frame_type": FRAME_TYPE_P, "qp": 30},
        )
        assert len(data) == 64
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags & FLAG_ENC_INFO
        assert not (frame.flags & FLAG_TRANSPORT_INFO)
        assert frame.has_enc_info is True
        assert frame.has_transport_info is False
        assert frame.frame_size_bytes == 5000

    def test_case3_transport_only_slides_to_offset_52(self):
        """68 bytes, TRANSPORT_INFO flag, trailer at offset 52."""
        data = pack_frame_full(
            frame_id=300,
            transport={
                "fill_pct": 80,
                "in_pressure": 1,
                "transport_drops": 0x11223344,
                "pressure_drops": 0xAABBCCDD,
                "packets_sent": 0x55667788,
            },
        )
        assert len(data) == 68
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags & FLAG_TRANSPORT_INFO
        assert not (frame.flags & FLAG_ENC_INFO)
        assert frame.has_enc_info is False
        assert frame.has_transport_info is True
        assert frame.fill_pct == 80
        assert frame.in_pressure == 1
        assert frame.transport_drops == 0x11223344
        assert frame.pressure_drops == 0xAABBCCDD
        assert frame.packets_sent == 0x55667788

    def test_case4_enc_plus_transport(self):
        """80 bytes, both flags set, enc at offset 52, transport at offset 64."""
        data = pack_frame_full(
            frame_id=400,
            enc={"frame_size_bytes": 8000, "frame_type": FRAME_TYPE_I, "qp": 25},
            transport={
                "fill_pct": 50,
                "in_pressure": 0,
                "transport_drops": 7,
                "pressure_drops": 11,
                "packets_sent": 13,
            },
        )
        assert len(data) == 80
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags & FLAG_ENC_INFO
        assert frame.flags & FLAG_TRANSPORT_INFO
        assert frame.has_enc_info is True
        assert frame.has_transport_info is True
        assert frame.frame_size_bytes == 8000
        assert frame.frame_type == FRAME_TYPE_I
        assert frame.qp == 25
        assert frame.fill_pct == 50
        assert frame.in_pressure == 0
        assert frame.transport_drops == 7
        assert frame.pressure_drops == 11
        assert frame.packets_sent == 13

    def test_keyframe_flag_combines_with_transport(self):
        """KEYFRAME + TRANSPORT_INFO without ENC_INFO is legal wire."""
        data = pack_frame_full(
            keyframe=True,
            transport={"fill_pct": 5, "in_pressure": 0,
                       "transport_drops": 0, "pressure_drops": 0,
                       "packets_sent": 1},
        )
        assert len(data) == 68
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags == (FLAG_KEYFRAME | FLAG_TRANSPORT_INFO)
        assert frame.has_transport_info is True
        assert frame.fill_pct == 5

    def test_transport_trailer_byte_order(self):
        """Multi-byte transport fields are big-endian on the wire."""
        data = pack_frame_full(
            transport={"fill_pct": 0, "in_pressure": 0,
                       "transport_drops": 0x01020304,
                       "pressure_drops": 0,
                       "packets_sent": 0},
        )
        # Trailer at offset 52 (no enc). Skip fill_pct(1)+in_pressure(1)+pad(2)
        # = 4 bytes, transport_drops starts at 56.
        assert data[56] == 0x01
        assert data[57] == 0x02
        assert data[58] == 0x03
        assert data[59] == 0x04

    def test_transport_padding_zero(self):
        """Reserved _pad bytes must be emitted as zero."""
        data = pack_frame_full(
            transport={"fill_pct": 0xFF, "in_pressure": 1,
                       "transport_drops": 0, "pressure_drops": 0,
                       "packets_sent": 0},
        )
        # Trailer at offset 52: byte 52 = fill_pct, 53 = in_pressure,
        # 54..55 = _pad
        assert data[52] == 0xFF
        assert data[53] == 0x01
        assert data[54] == 0x00
        assert data[55] == 0x00


class TestForwardCompat:
    """Pre-0.9.2 venc emits 52-byte (no flags) or 64-byte (ENC_INFO) frames.
    The new parser must accept those unchanged with transport_info=None,
    so a controller running against an old venc never panics."""

    def test_pre_092_base_only_still_parses(self):
        """52-byte frame with no flags — what pre-0.9.2 venc emits when no
        encoder telemetry is wired up."""
        data = pack_frame_base(seq_count=4, frame_id=1)
        assert len(data) == 52
        frame = parse_frame(data)
        assert frame is not None
        assert frame.flags == 0
        assert frame.has_enc_info is False
        assert frame.has_transport_info is False

    def test_pre_092_ext_only_still_parses(self):
        """64-byte ENC_INFO frame — what pre-0.9.2 venc emits in the normal
        case. Must not be misread as having a transport trailer."""
        data = pack_frame(frame_size_bytes=5000)
        assert len(data) == 64
        frame = parse_frame(data)
        assert frame is not None
        assert frame.has_enc_info is True
        assert frame.has_transport_info is False
        assert frame.fill_pct == 0  # default

    def test_truncated_transport_trailer_silently_skipped(self):
        """If the transport flag is set but the packet is too short to hold
        the trailer, parser skips silently — same defensive policy as
        truncated ENC_INFO."""
        full = pack_frame_full(
            transport={"fill_pct": 80, "in_pressure": 1,
                       "transport_drops": 0, "pressure_drops": 0,
                       "packets_sent": 0},
        )
        # Drop the last 4 bytes — packets_sent is now incomplete.
        truncated = full[:-4]
        frame = parse_frame(truncated)
        assert frame is not None
        # Flag still set (we trust the producer's flags byte) but trailer
        # not decoded.
        assert frame.flags & FLAG_TRANSPORT_INFO
        assert frame.has_transport_info is False
        assert frame.fill_pct == 0


class TestNetworkByteOrder:
    """Verify network byte order (big-endian) encoding."""

    def test_subscribe_magic_big_endian(self):
        msg = pack_subscribe()
        # RTPS = 0x52545053
        assert msg[0] == 0x52  # R
        assert msg[1] == 0x54  # T
        assert msg[2] == 0x50  # P
        assert msg[3] == 0x53  # S

    def test_frame_ssrc_big_endian(self):
        data = pack_frame(ssrc=0x01020304)
        # ssrc starts at offset 8 (after header(6) + stream_id(1) + flags(1))
        assert data[8] == 0x01
        assert data[9] == 0x02
        assert data[10] == 0x03
        assert data[11] == 0x04

    def test_frame_size_bytes_big_endian(self):
        data = pack_frame(frame_size_bytes=0x00010000)
        # Encoder trailer starts at offset 52
        assert data[52] == 0x00
        assert data[53] == 0x01
        assert data[54] == 0x00
        assert data[55] == 0x00
