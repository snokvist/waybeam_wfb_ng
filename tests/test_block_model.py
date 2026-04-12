"""Tests for the FEC block wire-cost model."""

import math

import pytest

from fec_controller.block_model import make_block, pack_frame_into_blocks


def test_single_full_block_no_padding():
    """k equal-sized packets -> zero source padding."""
    s = make_block([1000, 1000, 1000, 1000], fec_k=4, redundancy=0.25)
    assert s.k == 4
    # n = ceil(4 / 0.75) = 6
    assert s.n == 6
    assert s.source_padding == 0
    assert s.recovery_bytes == (6 - 4) * 1000
    assert s.wire_bytes == 4000 + 2000


def test_variable_sized_packets_padded_to_largest():
    """wfb-ng per-block largest-packet padding rule."""
    s = make_block([1000, 1200, 800], fec_k=3, redundancy=0.33)
    # n = ceil(3 / 0.67) = 5
    assert s.k == 3
    assert s.largest_packet == 1200
    assert s.source_bytes == 3000
    assert s.source_padding == 3 * 1200 - 3000  # 600
    assert s.recovery_bytes == 2 * 1200


def test_spill_into_next_block():
    """Packets past fec_k belong to the next block."""
    stats = pack_frame_into_blocks(
        [1000] * 10, fec_k=4, redundancy=0.33,
    )
    assert len(stats) == 3
    assert stats[0].k == 4
    assert stats[1].k == 4
    assert stats[2].k == 2


def test_zero_redundancy():
    s = make_block([1000, 1000], fec_k=2, redundancy=0.0)
    assert s.k == 2
    # With redundancy=0 we still emit at least one recovery symbol
    assert s.n == 3
    assert s.recovery_bytes == 1000


def test_redundancy_clamped_high():
    """redundancy near 1.0 doesn't blow up."""
    s = make_block([1000], fec_k=1, redundancy=0.9999)
    assert s.k == 1
    assert s.n >= 2
    assert s.wire_bytes == s.n * 1000


def test_large_p_on_small_frame_wastes_on_recovery():
    """When P is way above actual frame content, recovery is the waste driver."""
    # Frame is 600 bytes in one packet; P would have made this packet=600
    # no padding on sources (single source), but recovery symbol is 600.
    s = make_block([600], fec_k=16, redundancy=0.25)
    assert s.k == 1
    assert s.source_padding == 0
    assert s.recovery_bytes == (s.n - 1) * 600


def test_make_block_rejects_empty():
    with pytest.raises(ValueError):
        make_block([], fec_k=4, redundancy=0.25)


def test_make_block_rejects_bad_fec_k():
    with pytest.raises(ValueError):
        make_block([1000], fec_k=0, redundancy=0.25)


def test_make_block_rejects_negative_redundancy():
    with pytest.raises(ValueError):
        make_block([1000], fec_k=1, redundancy=-0.1)


def test_pack_frame_empty_returns_empty():
    assert pack_frame_into_blocks([], fec_k=4, redundancy=0.25) == []


def test_wire_bytes_sum_matches_components():
    s = make_block([800, 1200, 1100, 1400], fec_k=4, redundancy=0.3)
    assert s.wire_bytes == s.source_bytes + s.source_padding + s.recovery_bytes
    # Equivalent form: n * largest_packet
    assert s.wire_bytes == s.n * s.largest_packet
