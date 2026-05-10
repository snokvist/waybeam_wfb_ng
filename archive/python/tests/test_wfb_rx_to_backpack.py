"""Tests for ground/scripts/wfb_rx_to_backpack.py — the wfb_rx -> Backpack PTR bridge."""

from __future__ import annotations

import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest


# Load the script as a module (it lives under ground/scripts/, not on
# the package path).  parents[3] = repo root from
# archive/python/tests/test_*.py.
_SPEC = importlib.util.spec_from_file_location(
    "wfb_rx_to_backpack",
    Path(__file__).resolve().parents[3] / "ground" / "scripts" / "wfb_rx_to_backpack.py",
)
assert _SPEC is not None and _SPEC.loader is not None
mod = importlib.util.module_from_spec(_SPEC)
# Register before exec so @dataclass can resolve cls.__module__ (Python 3.13).
sys.modules["wfb_rx_to_backpack"] = mod
_SPEC.loader.exec_module(mod)


# ---- tick conversion --------------------------------------------------------

@pytest.mark.parametrize("us,expected", [
    (1000, 191),
    (1100, 351),
    (1500, 992),
    (1900, 1632),
    (2000, 1792),
    (500, 191),    # under-clamp
    (3000, 1792),  # over-clamp
])
def test_us_to_crsf_tick(us, expected):
    assert mod.us_to_crsf_tick(us) == expected


# ---- encode_pct -------------------------------------------------------------

@pytest.mark.parametrize("pct,expected", [
    (0.0, 1000),
    (50.0, 1500),
    (100.0, 2000),
    (-5.0, 1000),     # under-clamp
    (200.0, 2000),    # over-clamp
])
def test_encode_pct(pct, expected):
    assert mod.encode_pct(pct) == expected


# ---- encode_adapters --------------------------------------------------------

@pytest.mark.parametrize("n,expected", [
    (0, 1100),
    (1, 1300),
    (2, 1500),
    (3, 1700),
    (4, 1900),
    (5, 2000),    # saturates
    (10, 2000),   # well over
    (-1, 1100),   # negative clamps to n=0
])
def test_encode_adapters_never_collides_with_failsafe(n, expected):
    got = mod.encode_adapters(n)
    assert got == expected
    assert got != mod.FAILSAFE_VALUE  # critical: vehicle decoder ambiguity


# ---- encode_rssi ------------------------------------------------------------

@pytest.mark.parametrize("dbm,expected", [
    (-100.0, 1000),
    (-50.0, 1500),
    (0.0, 2000),
    (-150.0, 1000),  # under-clamp
    (10.0, 2000),    # over-clamp
])
def test_encode_rssi_default_range(dbm, expected):
    assert mod.encode_rssi(dbm, -100.0, 0.0) == expected


def test_encode_rssi_custom_range():
    # -80..-20 -> 1000..2000, mid -50 -> 1500
    assert mod.encode_rssi(-50.0, -80.0, -20.0) == 1500
    assert mod.encode_rssi(-80.0, -80.0, -20.0) == 1000
    assert mod.encode_rssi(-20.0, -80.0, -20.0) == 2000


# ---- parse_addr -------------------------------------------------------------

@pytest.mark.parametrize("spec,host,port", [
    ("0.0.0.0:5801", "0.0.0.0", 5801),
    ("127.0.0.1:25801", "127.0.0.1", 25801),
    (":5801", "0.0.0.0", 5801),
    ("5801", "0.0.0.0", 5801),
    ("[::]:5801", "::", 5801),
    ("[::1]:1234", "::1", 1234),
])
def test_parse_addr_ok(spec, host, port):
    assert mod.parse_addr(spec) == (host, port)


@pytest.mark.parametrize("bad", [
    "",
    "127.0.0.1:abc",
    "127.0.0.1:99999",   # out of range
    "127.0.0.1:0",       # 0 invalid
    "[::1",              # unmatched bracket
    "[::1]xyz",          # garbage after bracket
])
def test_parse_addr_rejects(bad):
    import argparse
    with pytest.raises(argparse.ArgumentTypeError):
        mod.parse_addr(bad)


# ---- positive_float ---------------------------------------------------------

def test_positive_float_rejects_zero_and_negative():
    import argparse
    with pytest.raises(argparse.ArgumentTypeError):
        mod.positive_float("0")
    with pytest.raises(argparse.ArgumentTypeError):
        mod.positive_float("-1.5")
    with pytest.raises(argparse.ArgumentTypeError):
        mod.positive_float("not-a-number")


def test_positive_float_accepts_positive():
    assert mod.positive_float("1.5") == 1.5
    assert mod.positive_float("30") == 30.0


# ---- slot map + build_ptr ---------------------------------------------------

def test_parse_slot_map_default():
    sm = mod.parse_slot_map("sel=pan,val=roll,rssi=tilt")
    assert sm.forward == {"sel": "pan", "val": "roll", "rssi": "tilt"}
    assert sm.inverse == {"pan": "sel", "roll": "val", "tilt": "rssi"}


def test_parse_slot_map_remap():
    sm = mod.parse_slot_map("rssi=pan,sel=roll,val=tilt")
    assert sm.forward == {"rssi": "pan", "sel": "roll", "val": "tilt"}
    assert sm.inverse == {"pan": "rssi", "roll": "sel", "tilt": "val"}


@pytest.mark.parametrize("bad", [
    "sel=pan,val=roll",                    # missing rssi
    "sel=pan,val=pan,rssi=tilt",           # duplicate slot
    "foo=pan,val=roll,rssi=tilt",          # bad logical
    "sel=elbow,val=roll,rssi=tilt",        # bad slot
])
def test_parse_slot_map_rejects(bad):
    import argparse
    with pytest.raises(argparse.ArgumentTypeError):
        mod.parse_slot_map(bad)


def test_build_ptr_default_mapping():
    sm = mod.parse_slot_map("sel=pan,val=roll,rssi=tilt")
    assert mod.build_ptr(1100, 1500, 1700, sm) == (1100, 1500, 1700)


def test_build_ptr_with_remap():
    sm = mod.parse_slot_map("rssi=pan,sel=roll,val=tilt")
    # logical sel=1100, val=1500, rssi=1700 -> pan=rssi=1700, roll=sel=1100, tilt=val=1500
    assert mod.build_ptr(1100, 1500, 1700, sm) == (1700, 1100, 1500)


# ---- multiplex --------------------------------------------------------------

def test_multiplex_rotates():
    mux = mod.Multiplex()
    seq = [mux.next() for _ in range(7)]
    assert seq[0:3] == [mod.SEL_LOSS, mod.SEL_FEC, mod.SEL_ADAPTERS]
    assert seq[3:6] == [mod.SEL_LOSS, mod.SEL_FEC, mod.SEL_ADAPTERS]
    assert seq[6] == mod.SEL_LOSS


# ---- parse_rx_ant -----------------------------------------------------------

def _frame(**overrides):
    base = {
        "type": "rx_ant",
        "ver": 1,
        "pkt": {"uniq": 1000, "lost": 50, "fec_recovered": 20, "adapters": 2},
        "rx_ant": [
            {"freq": 5765, "rssi": {"avg": -65, "max": -58}},
            {"freq": 5765, "rssi": {"avg": -72, "max": -66}},
        ],
    }
    base.update(overrides)
    return json.dumps(base).encode()


def test_parse_rx_ant_basic():
    s = mod.parse_rx_ant(_frame())
    assert s is not None
    assert s.uniq == 1000
    assert s.lost == 50
    assert s.fec_recovered == 20
    assert s.adapters == 2
    assert s.rssi_best_avg == -65.0
    assert s.loss_pct == pytest.approx(5.0)
    assert s.fec_pct == pytest.approx(2.0)


def test_parse_rx_ant_compact_and_spaced_both_work():
    obj = json.loads(_frame().decode())
    compact = json.dumps(obj, separators=(",", ":")).encode()
    spaced = json.dumps(obj, indent=2).encode()
    assert mod.parse_rx_ant(compact) is not None
    assert mod.parse_rx_ant(spaced) is not None


def test_parse_rx_ant_rejects_wrong_type():
    bad = json.dumps({"type": "tx_stats", "ver": 1, "pkt": {}}).encode()
    assert mod.parse_rx_ant(bad) is None


def test_parse_rx_ant_rejects_wrong_version():
    bad = _frame(ver=2)
    assert mod.parse_rx_ant(bad) is None


def test_parse_rx_ant_rejects_garbage():
    assert mod.parse_rx_ant(b"not json") is None
    assert mod.parse_rx_ant(b"") is None


def test_parse_rx_ant_falls_back_to_pkt_data():
    f = _frame(pkt={"data": 800, "lost": 10})
    s = mod.parse_rx_ant(f)
    assert s is not None
    assert s.uniq == 800   # picked up via pkt.data fallback
    assert s.lost == 10


def test_parse_rx_ant_picks_best_avg_across_antennas():
    f = _frame(rx_ant=[
        {"rssi": {"avg": -90, "max": -85}},
        {"rssi": {"avg": -60, "max": -55}},   # best
        {"rssi": {"avg": -75, "max": -70}},
    ])
    s = mod.parse_rx_ant(f)
    assert s is not None
    assert s.rssi_best_avg == -60.0


def test_parse_rx_ant_returns_none_rssi_when_missing():
    f = _frame(rx_ant=[])
    s = mod.parse_rx_ant(f)
    assert s is not None
    assert s.rssi_best_avg is None


def test_loss_pct_zero_uniq_safe():
    s = mod.RxAntStats(uniq=0, lost=0)
    assert s.loss_pct == 0.0
    assert s.fec_pct == 0.0


# ---- MSP framing ------------------------------------------------------------

def test_msp_encode_ptr_centre():
    """PTR(centre) tick payload should match Backpack-android Kotlin reference."""
    payload = struct.pack(
        "<hhh",
        mod.us_to_crsf_tick(1500),
        mod.us_to_crsf_tick(1500),
        mod.us_to_crsf_tick(1500),
    )
    frame = mod.msp_encode(mod.MSP_ELRS_BACKPACK_SET_PTR, payload)
    # Header: $X< flag=0 func=0x0383 LE len=6 LE
    assert frame[:8] == b"$X<\x00\x83\x03\x06\x00"
    # 992 LE = e0 03
    assert frame[8:14] == b"\xe0\x03\xe0\x03\xe0\x03"
    # CRC byte at end (computed); just sanity-check length.
    assert len(frame) == 8 + 6 + 1


def test_msp_encode_round_trip_via_local_parser():
    payload = bytes([1])
    frame = mod.msp_encode(mod.MSP_ELRS_BACKPACK_SET_HEAD_TRACKING, payload)
    # function = byte[4] | byte[5]<<8
    assert frame[4] | (frame[5] << 8) == mod.MSP_ELRS_BACKPACK_SET_HEAD_TRACKING
    assert frame[6] | (frame[7] << 8) == 1  # length
    assert frame[8] == 1  # payload
