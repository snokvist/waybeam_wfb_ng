"""Tests for WfbTxControl — binary command protocol (tx_cmd.h)."""

import struct
import socket
import pytest

from fec_controller.wfb_control import (
    WfbTxControl,
    CMD_SET_FEC,
    WFB_FEC_TIMEOUT_KEEP,
    _REQ_SET_FEC,
)


class TestWfbTxControl:

    def test_send_fec_format(self):
        """Verify the binary CMD_SET_FEC packet layout."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(8, 12) is True

            data, addr = receiver.recvfrom(256)
            assert len(data) == 9
            req_id, cmd_id, k, n, fec_timeout_ms = _REQ_SET_FEC.unpack(data)
            assert cmd_id == CMD_SET_FEC
            assert k == 8
            assert n == 12
            # Default omits fec_timeout — wire carries the "keep" sentinel.
            assert fec_timeout_ms == WFB_FEC_TIMEOUT_KEEP
        finally:
            ctrl.close()
            receiver.close()

    def test_send_fec_auto_connects(self):
        """send_fec should auto-connect if not already connected."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(4, 7) is True
            data, _ = receiver.recvfrom(256)
            req_id, cmd_id, k, n, fec_timeout_ms = _REQ_SET_FEC.unpack(data)
            assert cmd_id == CMD_SET_FEC
            assert k == 4
            assert n == 7
        finally:
            ctrl.close()
            receiver.close()

    def test_send_fec_with_timeout(self):
        """fec_timeout_ms reaches the wire when explicitly passed."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(8, 12, fec_timeout_ms=16) is True
            data, _ = receiver.recvfrom(256)
            _, _, k, n, fec_timeout_ms = _REQ_SET_FEC.unpack(data)
            assert k == 8
            assert n == 12
            assert fec_timeout_ms == 16
        finally:
            ctrl.close()
            receiver.close()

    def test_send_fec_timeout_clamped(self):
        """Explicit fec_timeout_ms outside [0, 65534] is clamped — high
        values land on 0xFFFE (max usable), not 0xFFFF which is reserved
        for the keep-current sentinel."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            # Out-of-range high → 0xFFFE (max), NOT 0xFFFF (sentinel).
            ctrl.send_fec(8, 12, fec_timeout_ms=999_999)
            data, _ = receiver.recvfrom(256)
            assert _REQ_SET_FEC.unpack(data)[4] == 0xFFFE
            # Largest valid explicit value preserved verbatim.
            ctrl.send_fec(8, 12, fec_timeout_ms=0xFFFE)
            data, _ = receiver.recvfrom(256)
            assert _REQ_SET_FEC.unpack(data)[4] == 0xFFFE
            # Negative → 0 (disable).
            ctrl.send_fec(8, 12, fec_timeout_ms=-5)
            data, _ = receiver.recvfrom(256)
            assert _REQ_SET_FEC.unpack(data)[4] == 0
        finally:
            ctrl.close()
            receiver.close()

    def test_close_idempotent(self):
        ctrl = WfbTxControl()
        ctrl.close()
        ctrl.close()  # Should not raise

    def test_send_fec_various_params(self):
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            for k, n in [(1, 2), (16, 23), (48, 64)]:
                ctrl.send_fec(k, n)
                data, _ = receiver.recvfrom(256)
                parsed = _REQ_SET_FEC.unpack(data)
                assert parsed[1] == CMD_SET_FEC
                assert parsed[2] == k
                assert parsed[3] == n
        finally:
            ctrl.close()
            receiver.close()

    def test_req_id_increments(self):
        """req_id should increment with each send."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            ctrl.send_fec(8, 12)
            data1, _ = receiver.recvfrom(256)
            req_id1 = _REQ_SET_FEC.unpack(data1)[0]

            ctrl.send_fec(8, 12)
            data2, _ = receiver.recvfrom(256)
            req_id2 = _REQ_SET_FEC.unpack(data2)[0]

            assert req_id2 == req_id1 + 1
        finally:
            ctrl.close()
            receiver.close()

    def test_send_fec_overflow_clamped(self):
        """k/n > 255 clamped to 255 (u8 wire format)."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(300, 400) is True
            data, _ = receiver.recvfrom(256)
            parsed = _REQ_SET_FEC.unpack(data)
            assert parsed[2] == 255  # k clamped
            assert parsed[3] == 255  # n clamped
        finally:
            ctrl.close()
            receiver.close()

    def test_send_fec_negative_clamped(self):
        """Negative k/n clamped to 0."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(-1, -5) is True
            data, _ = receiver.recvfrom(256)
            parsed = _REQ_SET_FEC.unpack(data)
            assert parsed[2] == 0
            assert parsed[3] == 0
        finally:
            ctrl.close()
            receiver.close()

    def test_reconnect_after_close(self):
        """Close then send_fec should reconnect automatically."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            ctrl.send_fec(8, 12)
            receiver.recvfrom(256)  # drain
            ctrl.close()
            assert ctrl._sock is None
            # Should reconnect
            assert ctrl.send_fec(4, 7) is True
            data, _ = receiver.recvfrom(256)
            parsed = _REQ_SET_FEC.unpack(data)
            assert parsed[2] == 4
            assert parsed[3] == 7
        finally:
            ctrl.close()
            receiver.close()

    def test_network_byte_order(self):
        """Verify packet is in network (big-endian) byte order."""
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            ctrl.send_fec(8, 12, fec_timeout_ms=16)
            data, _ = receiver.recvfrom(256)
            # req_id=1 in big-endian: 0x00000001
            assert data[:4] == b"\x00\x00\x00\x01"
            # cmd_id at offset 4
            assert data[4] == CMD_SET_FEC
            # k, n at offsets 5, 6
            assert data[5] == 8
            assert data[6] == 12
            # fec_timeout_ms at offsets 7..8, big-endian uint16
            assert data[7:9] == b"\x00\x10"
        finally:
            ctrl.close()
            receiver.close()
