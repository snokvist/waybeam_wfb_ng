"""Wire-format tests for the boundary-probe PER record (Phase 4).

Producer = ground/gs_supervisor.c probe_window_flush(); consumer =
vehicle/link_controller.c probe ingest. The frozen schema and the
consumer contract live in tests/protocols/_proto/probe_protocol.py.
"""

import json

import pytest

from tests.protocols._proto.probe_protocol import (
    PROBE_MCS_MAX,
    ProbeRecord,
    encode_probe,
    parse_probe,
    per_milli,
)


def make(mcs=7, recv=18, lost=2, rssi=-55):
    acc = recv + lost
    return ProbeRecord(
        ts_ms=1781121846077, radio_port=50, mcs=mcs,
        per=(lost / acc) if acc else None,
        recv=recv, lost=lost, accounted=acc,
        rssi=rssi, window_s=0.5,
    )


class TestProbeRecordWire:

    def test_roundtrip(self):
        rec = make()
        out = parse_probe(encode_probe(rec))
        assert out == rec

    def test_single_line_newline_terminated(self):
        raw = encode_probe(make())
        assert raw.endswith(b"\n")
        assert raw.count(b"\n") == 1

    def test_demux_token_is_compact(self):
        # The vehicle demux is strstr-based: the literal '"type":"probe"'
        # (no space after the colon) must appear in every record.
        assert b'"type":"probe"' in encode_probe(make())

    def test_consumer_required_keys_present(self):
        obj = json.loads(encode_probe(make()).decode())
        for key in ("mcs", "accounted", "lost"):
            assert isinstance(obj[key], int)

    def test_rssi_null_allowed(self):
        rec = make(rssi=None)
        raw = encode_probe(rec)
        assert b'"rssi":null' in raw
        assert parse_probe(raw).rssi is None

    def test_unknown_extra_keys_tolerated(self):
        # Schema is extend-only: consumers must ignore unknown keys.
        obj = json.loads(encode_probe(make()).decode())
        obj["snr"] = 25
        out = parse_probe(json.dumps(obj, separators=(",", ":")).encode())
        assert out is not None and out.mcs == 7

    @pytest.mark.parametrize("missing", ["mcs", "accounted", "lost"])
    def test_missing_required_key_rejected(self, missing):
        obj = json.loads(encode_probe(make()).decode())
        del obj[missing]
        assert parse_probe(json.dumps(obj).encode()) is None

    @pytest.mark.parametrize("mcs", [-1, PROBE_MCS_MAX, 255])
    def test_out_of_range_mcs_rejected(self, mcs):
        obj = json.loads(encode_probe(make()).decode())
        obj["mcs"] = mcs
        assert parse_probe(json.dumps(obj).encode()) is None

    def test_wrong_type_rejected(self):
        obj = json.loads(encode_probe(make()).decode())
        obj["type"] = "rx_ant"
        assert parse_probe(json.dumps(obj).encode()) is None

    def test_garbage_rejected(self):
        assert parse_probe(b"\xff\xfe not json") is None
        assert parse_probe(b"[1,2,3]") is None


class TestPerMilliDerivation:
    """Mirror of the link_controller.c integer derivation:
    (lost*1000 + accounted/2) / accounted, -1 when accounted == 0."""

    def test_exact_values(self):
        assert per_milli(20, 0) == 0
        assert per_milli(20, 2) == 100      # 10% -> probe_fail threshold
        assert per_milli(1000, 20) == 20    # 2%  -> probe_clean threshold
        assert per_milli(20, 20) == 1000

    def test_round_half_up(self):
        # 1/3 -> 333.33 -> 333 ; 2/3 -> 666.67 -> 667
        assert per_milli(3, 1) == 333
        assert per_milli(3, 2) == 667
        # half exactly: 1/2000 = 0.5‰ -> rounds to 1
        assert per_milli(2000, 1) == 1

    def test_invalid_when_no_traffic(self):
        # accounted == 0 must map to -1 (INVALID -> hold), never 0 or fail.
        assert per_milli(0, 0) == -1

    def test_soak_observed_case(self):
        # The MCS-7-cliff case from the 2026-06-10 soak: all-lost windows
        # produce no record at all (producer never emits accounted == 0),
        # so the consumer sees staleness, not a fake-clean 0.
        rec = make(recv=0, lost=0)
        assert rec.accounted == 0
        assert per_milli(rec.accounted, rec.lost) == -1
