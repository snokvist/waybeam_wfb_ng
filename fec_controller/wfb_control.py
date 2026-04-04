"""
wfb_tx UDP control interface.

Sends FEC updates to wfb_tx via its UDP control socket using the binary
command protocol defined in tx_cmd.h.

Wire format (CMD_SET_FEC request, 7 bytes packed):
    uint32_t req_id      (network byte order)
    uint8_t  cmd_id      (1 = CMD_SET_FEC)
    uint8_t  k
    uint8_t  n

On receiving CMD_SET_FEC, wfb_tx flushes the current FEC block, calls
init_session(k, n) which generates a new session key, and immediately
bursts (n - k + 1) session announce packets so the receiver converges
without waiting for the next periodic announce.
"""

import struct
import socket
import logging

log = logging.getLogger("fec_ctrl")

# Command IDs (tx_cmd.h)
CMD_SET_FEC = 1

# Packed request: uint32_t req_id, uint8_t cmd_id, uint8_t k, uint8_t n
_REQ_SET_FEC = struct.Struct("!IBBB")  # 7 bytes


class WfbTxControl:

    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        self.host = host
        self.port = port
        self._sock: socket.socket | None = None
        self._req_id: int = 0

    def connect(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_fec(self, k: int, n: int) -> bool:
        """Send CMD_SET_FEC to wfb_tx. Returns True on successful send.

        k and n are clamped to [0, 255] (uint8 wire format).
        """
        if not self._sock:
            self.connect()
        try:
            k = max(0, min(255, k))
            n = max(0, min(255, n))
            self._req_id = (self._req_id + 1) & 0xFFFFFFFF
            pkt = _REQ_SET_FEC.pack(self._req_id, CMD_SET_FEC, k, n)
            self._sock.sendto(pkt, (self.host, self.port))
            log.info("Sent CMD_SET_FEC: k=%d n=%d -> %s:%d", k, n, self.host, self.port)
            return True
        except OSError as e:
            log.error("Failed to send FEC update: %s", e)
            return False

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None
