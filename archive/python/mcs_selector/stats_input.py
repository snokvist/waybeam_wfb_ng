"""
Async UDP listener for wfb_rx -Y datagrams.

One datagram per `-l` interval (typ. 100 ms for 10 Hz). Single-line UTF-8
JSON, newline-terminated. Parser tolerates unrelated payloads (different
`type` or wrong `ver`) by discarding them; only schema violations within a
real rx_ant frame raise.
"""

import asyncio
import logging
from typing import Awaitable, Callable

from mcs_selector.protocol import RxAntDatagram, parse_rx_ant

log = logging.getLogger("mcs_sel.input")

OnDatagram = Callable[[RxAntDatagram], Awaitable[None]]


class RxAntListener(asyncio.DatagramProtocol):

    def __init__(self, on_datagram: OnDatagram, loop: asyncio.AbstractEventLoop):
        self.on_datagram = on_datagram
        self._loop = loop
        self.received: int = 0
        self.parse_errors: int = 0
        self.ignored: int = 0

    def connection_made(self, transport):  # noqa: D401
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            d = parse_rx_ant(data)
        except ValueError as e:
            self.parse_errors += 1
            # Log first few then summarise — bad senders shouldn't spam.
            if self.parse_errors <= 5:
                log.warning("rx_ant parse error from %s: %s", addr, e)
            return

        if d is None:
            self.ignored += 1
            return

        self.received += 1
        # Schedule the user callback on the running loop. We don't await
        # here because asyncio.DatagramProtocol callbacks are sync.
        asyncio.ensure_future(self.on_datagram(d), loop=self._loop)


async def serve_rx_ant(
    host: str,
    port: int,
    on_datagram: OnDatagram,
    loop: asyncio.AbstractEventLoop | None = None,
) -> tuple[asyncio.DatagramTransport, RxAntListener]:
    """Bind a UDP listener on (host, port) and dispatch parsed datagrams
    to `on_datagram`. Returns the transport + protocol instance so the
    caller can close them on shutdown."""
    loop = loop or asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: RxAntListener(on_datagram, loop),
        local_addr=(host, port),
    )
    log.info("rx_ant listener bound %s:%d", host, port)
    return transport, protocol
