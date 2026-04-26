"""End-to-end test for the async rx_ant listener.

Sends real wire-format datagrams over loopback UDP, verifies that the
listener parses + dispatches them to the user callback. Uses
asyncio.run() directly to avoid a pytest-asyncio dependency.
"""

import asyncio
import socket

from mcs_selector.protocol import AntStats, PktStats, RxAntDatagram, encode_rx_ant
from mcs_selector.stats_input import serve_rx_ant


def _sample(rssi=-55, seq=1):
    return RxAntDatagram(
        ts_ms=1000 + seq, seq=seq, interval_ms=100,
        ant=[AntStats(5745, 3, 20, "0", 200, rssi - 2, rssi, rssi + 2, 18, 22, 26)],
        pkt=PktStats(205, 280000, 0, 0, 200, 195, 5, 0, 0, 195, 273000),
    )


def test_listener_dispatches_real_datagrams():
    async def _run():
        received: list[RxAntDatagram] = []
        done = asyncio.Event()

        async def cb(d: RxAntDatagram):
            received.append(d)
            if len(received) == 3:
                done.set()

        transport, protocol = await serve_rx_ant("127.0.0.1", 0, cb)
        try:
            port = transport.get_extra_info("sockname")[1]
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for i in range(3):
                sender.sendto(encode_rx_ant(_sample(rssi=-50 - i, seq=i + 1)),
                              ("127.0.0.1", port))
            sender.close()

            await asyncio.wait_for(done.wait(), timeout=2.0)
        finally:
            transport.close()

        assert protocol.received == 3
        assert protocol.parse_errors == 0
        assert [r.seq for r in received] == [1, 2, 3]
        assert [r.ant[0].rssi_avg for r in received] == [-50, -51, -52]

    asyncio.run(_run())


def test_listener_ignores_unrelated_payloads():
    async def _run():
        received: list[RxAntDatagram] = []

        async def cb(d):
            received.append(d)

        transport, protocol = await serve_rx_ant("127.0.0.1", 0, cb)
        try:
            port = transport.get_extra_info("sockname")[1]
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sender.sendto(b'{"type":"tx_ant","ver":1}\n', ("127.0.0.1", port))
            sender.sendto(b'not json at all', ("127.0.0.1", port))
            sender.sendto(encode_rx_ant(_sample()), ("127.0.0.1", port))
            sender.close()
            await asyncio.sleep(0.2)
        finally:
            transport.close()

        assert len(received) == 1  # only the rx_ant got dispatched
        assert protocol.ignored == 1
        assert protocol.parse_errors == 1

    asyncio.run(_run())
