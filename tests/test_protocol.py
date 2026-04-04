"""Tests for sidecar wire protocol (rtp_sidecar.h)."""

import struct
import pytest

from fec_controller.protocol import (
    FRAME_BASE_SIZE,
    FRAME_EXT_SIZE,
    ENC_TRAILER_SIZE,
    SUBSCRIBE_SIZE,
    SIDECAR_MAGIC,
    SIDECAR_VERSION,
    MSG_SUBSCRIBE,
    MSG_FRAME,
    FLAG_KEYFRAME,
    FLAG_ENC_INFO,
    FRAME_TYPE_P,
    FRAME_TYPE_I,
    FRAME_TYPE_IDR,
    SidecarFrame,
    pack_subscribe,
    parse_header,
    parse_frame,
    pack_frame,
    pack_frame_base,
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

    def test_flag_values(self):
        assert FLAG_KEYFRAME == 0x01
        assert FLAG_ENC_INFO == 0x02

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
