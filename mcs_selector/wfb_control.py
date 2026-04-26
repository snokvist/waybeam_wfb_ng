"""
wfb_tx UDP control client for CMD_SET_RADIO / CMD_GET_RADIO.

CMD_SET_RADIO rewrites the entire radiotap header — bandwidth, stbc,
ldpc, short_gi, vht_mode, vht_nss must all be supplied alongside
mcs_index. This client pulls those via CMD_GET_RADIO at startup and
re-reads after every successful SET so the next SET can vary mcs_index
in isolation.

The wfb_tx control loop responds with cmd_resp_t (req_id + rc, both
network byte order) plus a command-specific payload. We require:
  - rc == 0
  - req_id matches the outstanding request

Network errors and timeouts are non-fatal: get() returns None,
set_mcs() returns False, and the caller decides whether to retry.
"""

import logging
import random
import socket

from mcs_selector.protocol import (
    RadioParams,
    pack_get_radio,
    pack_set_radio,
    parse_get_radio_response,
    parse_set_radio_response,
)

log = logging.getLogger("mcs_sel.ctrl")


class WfbTxRadioControl:

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 8000,
        response_timeout_s: float = 0.5,
    ):
        self.host = host
        self.port = port
        self.response_timeout_s = response_timeout_s
        self._sock: socket.socket | None = None
        # Random seed so concurrent runs / restarts don't collide on
        # req_id 0/1/2 — wfb_tx checks req_id echo and a stale reply
        # could otherwise be accepted.
        self._req_id: int = random.randint(0, 0xFFFFFFFF)
        # Cached params from the most recent successful GET. The
        # selector reads this to build its next SET.
        self.cached: RadioParams | None = None

    def connect(self) -> None:
        if self._sock is not None:
            return
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Connect-ed UDP so recv only returns datagrams from the
        # control endpoint we care about. Drops random spam from other
        # senders without us having to filter.
        self._sock.connect((self.host, self.port))
        self._sock.settimeout(self.response_timeout_s)

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None

    def _next_req_id(self) -> int:
        self._req_id = (self._req_id + 1) & 0xFFFFFFFF
        return self._req_id

    # ------------------------------------------------------------------ GET

    def get_radio(self) -> RadioParams | None:
        """Issue CMD_GET_RADIO and return the parsed RadioParams.
        Refreshes self.cached on success. Returns None on error."""
        self.connect()
        assert self._sock is not None
        req_id = self._next_req_id()
        try:
            self._sock.send(pack_get_radio(req_id))
            data = self._sock.recv(64)
        except OSError as e:
            log.warning("CMD_GET_RADIO failed: %s", e)
            return None

        try:
            resp_id, rc, params = parse_get_radio_response(data)
        except ValueError as e:
            log.warning("CMD_GET_RADIO parse error: %s", e)
            return None

        if resp_id != req_id:
            log.warning("CMD_GET_RADIO req_id mismatch: %d != %d", resp_id, req_id)
            return None
        if rc != 0:
            log.warning("CMD_GET_RADIO rc=%d (non-zero)", rc)
            return None

        self.cached = params
        log.debug(
            "CMD_GET_RADIO ok: stbc=%d ldpc=%d sgi=%d bw=%d mcs=%d vht=%d nss=%d",
            params.stbc, params.ldpc, params.short_gi, params.bandwidth,
            params.mcs_index, params.vht_mode, params.vht_nss,
        )
        return params

    # ------------------------------------------------------------------ SET

    def set_mcs(self, mcs_index: int) -> bool:
        """Send CMD_SET_RADIO with `mcs_index`, preserving every other
        field from `self.cached`. Re-reads on success so subsequent
        sends carry the latest applied values.

        Returns True only on a clean rc=0 response with matching
        req_id. False if cached params are missing, the network fails,
        or wfb_tx rejects the request.
        """
        if self.cached is None:
            log.error("set_mcs called before get_radio() populated cache")
            return False

        self.connect()
        assert self._sock is not None
        req_id = self._next_req_id()
        params = self.cached.with_mcs(mcs_index)
        try:
            self._sock.send(pack_set_radio(req_id, params))
            data = self._sock.recv(64)
        except OSError as e:
            log.warning("CMD_SET_RADIO failed: %s", e)
            return False

        try:
            resp_id, rc = parse_set_radio_response(data)
        except ValueError as e:
            log.warning("CMD_SET_RADIO parse error: %s", e)
            return False

        if resp_id != req_id:
            log.warning("CMD_SET_RADIO req_id mismatch: %d != %d", resp_id, req_id)
            return False
        if rc != 0:
            log.warning("CMD_SET_RADIO rc=%d — wfb_tx rejected mcs=%d", rc, mcs_index)
            return False

        log.info("CMD_SET_RADIO ok: mcs=%d (bw=%d sgi=%d stbc=%d)",
                 mcs_index, params.bandwidth, params.short_gi, params.stbc)
        # Read back so the next SET reflects whatever wfb_tx actually
        # latched (e.g. operator changed bandwidth out-of-band).
        self.get_radio()
        return True
