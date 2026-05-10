"""Tests for WfbTxRadioControl — exercises the real wire format end-to-end
by running a tiny in-process UDP echo server that speaks the cmd_resp_t
protocol from src/tx_cmd.h.
"""

import socket
import struct
import threading

import pytest

from mcs_selector.protocol import (
    CMD_GET_RADIO,
    CMD_SET_RADIO,
    RadioParams,
)
from mcs_selector.wfb_control import WfbTxRadioControl


class FakeWfbTx:
    """Minimal in-process wfb_tx control responder.

    Reads cmd_req_t off a UDP socket, echoes the req_id with rc=0, and
    appends the appropriate response payload. State is just a single
    RadioParams that GET reads and SET overwrites.
    """

    def __init__(self, params: RadioParams):
        self.params = params
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(2.0)
        self.set_calls: list[RadioParams] = []
        self._stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self._stop:
            try:
                data, addr = self.sock.recvfrom(64)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(data) < 5:
                continue
            req_id = struct.unpack("!I", data[:4])[0]
            cmd_id = data[4]
            if cmd_id == CMD_GET_RADIO:
                body = struct.pack(
                    "!IIBBBBBBB",
                    req_id, 0,
                    self.params.stbc, self.params.ldpc, self.params.short_gi,
                    self.params.bandwidth, self.params.mcs_index,
                    self.params.vht_mode, self.params.vht_nss,
                )
                self.sock.sendto(body, addr)
            elif cmd_id == CMD_SET_RADIO:
                if len(data) != 12:
                    self.sock.sendto(struct.pack("!II", req_id, 1), addr)
                    continue
                _, _, stbc, ldpc, sgi, bw, mcs, vht, nss = struct.unpack(
                    "!IBBBBBBBB", data
                )
                self.params = RadioParams(stbc, ldpc, sgi, bw, mcs, vht, nss)
                self.set_calls.append(self.params)
                self.sock.sendto(struct.pack("!II", req_id, 0), addr)

    def stop(self):
        self._stop = True
        self.sock.close()


@pytest.fixture
def fake_tx():
    tx = FakeWfbTx(RadioParams(stbc=1, ldpc=1, short_gi=0,
                               bandwidth=20, mcs_index=3,
                               vht_mode=0, vht_nss=1))
    yield tx
    tx.stop()


class TestGetRadio:

    def test_populates_cache(self, fake_tx):
        ctrl = WfbTxRadioControl("127.0.0.1", fake_tx.port, response_timeout_s=1.0)
        try:
            params = ctrl.get_radio()
            assert params is not None
            assert params.bandwidth == 20
            assert params.mcs_index == 3
            assert params.stbc == 1
            assert ctrl.cached == params
        finally:
            ctrl.close()

    def test_timeout_returns_none(self):
        # Bind a port but don't run the responder — request will time out.
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        ctrl = WfbTxRadioControl("127.0.0.1", port, response_timeout_s=0.1)
        try:
            assert ctrl.get_radio() is None
        finally:
            ctrl.close()


class TestSetMcs:

    def test_preserves_other_radio_fields(self, fake_tx):
        ctrl = WfbTxRadioControl("127.0.0.1", fake_tx.port, response_timeout_s=1.0)
        try:
            ctrl.get_radio()
            assert ctrl.set_mcs(7) is True
            applied = fake_tx.set_calls[-1]
            # Only mcs_index should change; everything else must match
            # what GET returned.
            assert applied.mcs_index == 7
            assert applied.bandwidth == 20
            assert applied.stbc == 1
            assert applied.ldpc == 1
            assert applied.short_gi == 0
            assert applied.vht_mode == 0
            assert applied.vht_nss == 1
        finally:
            ctrl.close()

    def test_set_without_get_fails_loudly(self, fake_tx):
        ctrl = WfbTxRadioControl("127.0.0.1", fake_tx.port, response_timeout_s=1.0)
        try:
            assert ctrl.set_mcs(5) is False
            assert fake_tx.set_calls == []
        finally:
            ctrl.close()

    def test_readback_after_set(self, fake_tx):
        ctrl = WfbTxRadioControl("127.0.0.1", fake_tx.port, response_timeout_s=1.0)
        try:
            ctrl.get_radio()
            ctrl.set_mcs(9)
            # The post-SET readback should have refreshed cached.mcs_index.
            assert ctrl.cached is not None
            assert ctrl.cached.mcs_index == 9
        finally:
            ctrl.close()
