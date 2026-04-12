"""Tests for the encoder simulator."""

import math

import pytest

from fec_controller.encoder_sim import EncoderSim, SizeProfile


def test_step_yields_sequential_frames():
    enc = EncoderSim(fps=60, profile=SizeProfile(base=4000, i_mult=4.0))
    f0 = enc.step(payload=1400)
    f1 = enc.step(payload=1400)
    assert f0.frame_id == 0
    assert f1.frame_id == 1
    assert f1.time_s == pytest.approx(1.0 / 60, rel=1e-6)


def test_idr_cadence():
    enc = EncoderSim(
        fps=60,
        profile=SizeProfile(base=2000, i_mult=5.0, gop_interval=30, jitter_sigma=0.0),
    )
    idrs = [enc.step(1400).is_idr for _ in range(60)]
    assert idrs[0] is True
    assert idrs[30] is True
    # Non-IDR positions are all False
    for i, is_idr in enumerate(idrs):
        if i % 30 != 0:
            assert is_idr is False, f"frame {i} unexpectedly IDR"


def test_packetisation_count_matches_size():
    enc = EncoderSim(
        fps=60,
        profile=SizeProfile(base=5000, i_mult=1.0, jitter_sigma=0.0),
    )
    for _ in range(20):
        f = enc.step(payload=1200)
        expected = math.ceil(f.size_bytes / 1200)
        assert len(f.packets) == expected
        assert sum(f.packets) == f.size_bytes
        # All but last are full payload
        for p in f.packets[:-1]:
            assert p == 1200


def test_packetisation_single_small_frame():
    enc = EncoderSim(
        fps=60,
        profile=SizeProfile(base=500, i_mult=1.0, jitter_sigma=0.0),
    )
    f = enc.step(payload=1400)
    assert len(f.packets) == 1
    assert f.packets[0] == f.size_bytes


def test_bitrate_event_shifts_base():
    profile = SizeProfile(
        base=2000, i_mult=1.0, jitter_sigma=0.0,
        bitrate_events=[(0.5, 8000)],
    )
    enc = EncoderSim(fps=60, profile=profile)
    before = enc.step(1400)
    # Advance to t=0.5s (30 frames)
    for _ in range(30):
        enc.step(1400)
    after = enc.step(1400)
    assert before.size_bytes < after.size_bytes


def test_deterministic_under_same_seed():
    a = EncoderSim(fps=60, seed=42)
    b = EncoderSim(fps=60, seed=42)
    for _ in range(50):
        fa = a.step(1400)
        fb = b.step(1400)
        assert fa.size_bytes == fb.size_bytes
        assert fa.is_idr == fb.is_idr
        assert fa.packets == fb.packets


def test_rejects_invalid_payload():
    enc = EncoderSim(fps=60)
    with pytest.raises(ValueError):
        enc.step(payload=0)


def test_rejects_invalid_fps():
    with pytest.raises(ValueError):
        EncoderSim(fps=0)
