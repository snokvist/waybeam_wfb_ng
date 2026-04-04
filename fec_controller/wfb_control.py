"""
wfb_tx UDP control interface.

Sends FEC updates to wfb_tx via its UDP control socket.
wfb_tx prints "LISTEN_UDP_CONTROL <port>" on startup.
Protocol: text commands over UDP, fire-and-forget.
"""

import socket
import logging

log = logging.getLogger("fec_ctrl")


class WfbTxControl:

    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        self.host = host
        self.port = port
        self._sock: socket.socket | None = None

    def connect(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_fec(self, k: int, n: int) -> bool:
        """Send set_fec command. Returns True on success."""
        if not self._sock:
            self.connect()
        try:
            cmd = f"set_fec {k} {n}\n"
            self._sock.sendto(cmd.encode(), (self.host, self.port))
            log.info("Sent FEC update: k=%d n=%d -> %s:%d", k, n, self.host, self.port)
            return True
        except OSError as e:
            log.error("Failed to send FEC update: %s", e)
            return False

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None
