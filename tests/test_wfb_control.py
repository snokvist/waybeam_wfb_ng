"""Tests for WfbTxControl."""

import socket
import threading
import pytest

from fec_controller.wfb_control import WfbTxControl


class TestWfbTxControl:

    def test_send_fec_format(self):
        """Verify the command format sent over UDP."""
        # Set up a UDP receiver
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        receiver.settimeout(2.0)

        try:
            ctrl = WfbTxControl("127.0.0.1", port)
            assert ctrl.send_fec(8, 12) is True

            data, addr = receiver.recvfrom(256)
            assert data == b"set_fec 8 12\n"
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
            # Don't call connect() explicitly
            assert ctrl.send_fec(4, 7) is True
            data, _ = receiver.recvfrom(256)
            assert data == b"set_fec 4 7\n"
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
                assert data == f"set_fec {k} {n}\n".encode()
        finally:
            ctrl.close()
            receiver.close()
