"""Wire-format tests for mcs_selector.protocol.

Byte-exact assertions against the structs from upstream wfb-ng
src/tx_cmd.h and the rx_ant ver 1 schema in poc/SHM_HOWTO.md.
"""

import json
import struct

import pytest

from mcs_selector.protocol import (
    CMD_GET_RADIO,
    CMD_SET_RADIO,
    RadioParams,
    encode_rx_ant,
    pack_get_radio,
    pack_set_radio,
    parse_get_radio_response,
    parse_rx_ant,
    parse_set_radio_response,
)


class TestSetRadioWire:

    def test_packet_size_and_layout(self):
        params = RadioParams(
            stbc=1, ldpc=1, short_gi=0,
            bandwidth=20, mcs_index=5,
            vht_mode=0, vht_nss=1,
        )
        pkt = pack_set_radio(req_id=0xDEADBEEF, params=params)
        assert len(pkt) == 12

        req_id, cmd_id, stbc, ldpc, sgi, bw, mcs, vht, nss = struct.unpack(
            "!IBBBBBBBB", pkt
        )
        assert req_id == 0xDEADBEEF
        assert cmd_id == CMD_SET_RADIO == 2
        assert stbc == 1
        assert ldpc == 1
        assert sgi == 0
        assert bw == 20
        assert mcs == 5
        assert vht == 0
        assert nss == 1

    def test_bools_are_zero_or_one(self):
        # Even with truthy ints, the wire byte must be exactly 0/1.
        params = RadioParams(
            stbc=255, ldpc=42, short_gi=99,
            bandwidth=40, mcs_index=11,
            vht_mode=7, vht_nss=2,
        )
        pkt = pack_set_radio(req_id=0, params=params)
        _, _, stbc, ldpc, sgi, _, _, vht, _ = struct.unpack("!IBBBBBBBB", pkt)
        assert stbc == 255  # uint8, kept as-is
        assert ldpc == 1    # bool, normalised
        assert sgi == 1
        assert vht == 1

    def test_req_id_wraps(self):
        # 0x100000000 wraps to 0; helpful when the req_id counter rolls.
        pkt = pack_set_radio(
            req_id=0x100000005,
            params=RadioParams(0, 0, 0, 20, 0, 0, 1),
        )
        req_id = struct.unpack("!I", pkt[:4])[0]
        assert req_id == 5


class TestGetRadioWire:

    def test_request_size_and_layout(self):
        pkt = pack_get_radio(req_id=0x12345678)
        assert len(pkt) == 5
        req_id, cmd_id = struct.unpack("!IB", pkt)
        assert req_id == 0x12345678
        assert cmd_id == CMD_GET_RADIO == 4

    def test_response_parse(self):
        # cmd_resp_t: req_id(!I) rc(!I) then cmd_get_radio body.
        body = struct.pack("!IIBBBBBBB", 0xCAFEBABE, 0, 1, 1, 0, 20, 7, 0, 1)
        assert len(body) == 15
        req_id, rc, params = parse_get_radio_response(body)
        assert req_id == 0xCAFEBABE
        assert rc == 0
        assert params == RadioParams(
            stbc=1, ldpc=1, short_gi=0,
            bandwidth=20, mcs_index=7,
            vht_mode=0, vht_nss=1,
        )

    def test_response_size_mismatch_rejected(self):
        with pytest.raises(ValueError):
            parse_get_radio_response(b"\x00" * 14)
        with pytest.raises(ValueError):
            parse_get_radio_response(b"\x00" * 16)


class TestSetRadioResponse:

    def test_header_only(self):
        # CMD_SET_RADIO returns just the 8-byte header (rc + req_id).
        data = struct.pack("!II", 0xAA, 0)
        req_id, rc = parse_set_radio_response(data)
        assert req_id == 0xAA
        assert rc == 0

    def test_truncated_rejected(self):
        with pytest.raises(ValueError):
            parse_set_radio_response(b"\x00\x00\x00")


class TestRxAntParser:

    def test_canonical_example(self):
        """Verbatim from poc/SHM_HOWTO.md."""
        sample = (
            b'{"ts_ms":26279557,"type":"rx_ant","ver":1,"seq":2,"interval_ms":1000,'
            b'"ant":[{"freq":5745,"mcs":5,"bw":20,"id":"1","pkts":2286,'
            b'"rssi":{"min":-28,"avg":-27,"max":-26},'
            b'"snr":{"min":26,"avg":31,"max":35}},'
            b'{"freq":5745,"mcs":5,"bw":20,"id":"0","pkts":2286,'
            b'"rssi":{"min":-38,"avg":-37,"max":-36},'
            b'"snr":{"min":26,"avg":32,"max":38}}],'
            b'"pkt":{"all":2308,"bytes":2953783,"dec_err":21,"session":1,"data":2286,'
            b'"uniq":2286,"fec_recovered":49,"lost":0,"bad":0,'
            b'"outgoing":1252,"outgoing_bytes":1699183}}'
        )
        d = parse_rx_ant(sample)
        assert d is not None
        assert d.ts_ms == 26279557
        assert d.seq == 2
        assert d.interval_ms == 1000
        assert len(d.ant) == 2
        assert d.ant[0].rssi_avg == -27
        assert d.ant[1].rssi_avg == -37
        assert d.pkt.fec_recovered == 49
        assert d.pkt.lost == 0
        assert d.pkt.data == 2286

    def test_unknown_type_returns_none(self):
        # Forward-compatible: unrelated stats datagrams must not raise.
        assert parse_rx_ant(b'{"type":"tx_ant","ver":1}') is None

    def test_wrong_version_returns_none(self):
        assert parse_rx_ant(b'{"type":"rx_ant","ver":99}') is None

    def test_invalid_json_raises(self):
        with pytest.raises(ValueError):
            parse_rx_ant(b'{"type":"rx_ant",ver:1}')

    def test_schema_violation_raises(self):
        # missing pkt.data — the parser will use 0; missing rssi block on
        # an antenna IS a schema violation, though.
        bad = b'{"ts_ms":1,"type":"rx_ant","ver":1,"ant":[{"freq":5,"mcs":1,"bw":20,"id":"x","pkts":1}],"pkt":{}}'
        with pytest.raises(ValueError):
            parse_rx_ant(bad)

    def test_round_trip_via_encode(self):
        from mcs_selector.protocol import AntStats, PktStats, RxAntDatagram

        d = RxAntDatagram(
            ts_ms=1000, seq=1, interval_ms=100,
            ant=[AntStats(5745, 3, 20, "0", 200, -55, -50, -45, 18, 22, 26)],
            pkt=PktStats(205, 280000, 0, 0, 200, 195, 5, 0, 0, 195, 273000),
        )
        wire = encode_rx_ant(d)
        assert wire.endswith(b"\n")
        # Must round-trip cleanly through the real parser.
        parsed = parse_rx_ant(wire)
        assert parsed is not None
        assert parsed.ant[0].rssi_avg == -50
        assert parsed.pkt.fec_recovered == 5
