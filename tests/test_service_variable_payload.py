"""Tests for the sizer wired into FECControllerService (read-only observer)."""

import math

import pytest

from fec_controller.config import ControllerConfig
from fec_controller.link_budget import LinkBudgetEstimator
from fec_controller.protocol import pack_frame
from fec_controller.service import FECControllerService


def _make_service(enable: bool, **overrides) -> FECControllerService:
    cfg = ControllerConfig(
        enable_variable_payload=enable,
        min_payload=800,
        mtu_override=3000,
        target_fec_k=8,
        pps_budget_fallback=3000.0,
    )
    for k, v in overrides.items():
        setattr(cfg, k, v)
    return FECControllerService(
        config=cfg,
        stat_port=0,
        wfb_control_port=0,
        dry_run=True,
    )


def _feed(service: FECControllerService, frame_size: int, count: int, fps: int = 60) -> None:
    """Feed N simulated FRAME packets into the service's handler."""
    period_us = 1_000_000 // fps
    for i in range(count):
        wire = pack_frame(
            ssrc=0x1234,
            rtp_timestamp=i * period_us,
            frame_id=i,
            frame_ready_us=i * period_us,
            seq_count=max(1, math.ceil(frame_size / 1400)),
            capture_us=max(0, i * period_us - 4000),
            last_pkt_send_us=i * period_us + 300,
            frame_size_bytes=frame_size,
        )
        service._handle_packet(wire)


def test_sizer_disabled_is_inert():
    svc = _make_service(enable=False)
    assert svc.frame_size_percentile is None
    assert svc.link_budget is None
    _feed(svc, 8000, 100)
    assert svc.current_payload_decision() is None


def test_sizer_enabled_emits_decision_after_warmup():
    svc = _make_service(enable=True)
    assert svc.frame_size_percentile is not None
    assert svc.link_budget is not None

    _feed(svc, 8000, 60)  # well past percentile min_samples (default 8)
    d = svc.current_payload_decision()
    assert d is not None
    assert d.payload >= 800
    assert d.payload <= 3000
    # target_pf = min(3000/60, 8) = 8; raw = 8000/8 = 1000 -> payload 1000
    assert d.packets_per_frame <= 8


def test_sizer_respects_external_link_budget():
    """observe_link_budget() swaps the estimator's current value; subsequent
    decisions should reflect the new budget."""
    svc = _make_service(enable=True)

    # Initial warmup with default fallback (3000 pps)
    _feed(svc, 8000, 60)
    d_before = svc.current_payload_decision()
    assert d_before is not None

    # Tighten the budget to 600 pps — target_pf should shrink
    for _ in range(5):
        svc.observe_link_budget(600.0)
    _feed(svc, 8000, 60)
    d_after = svc.current_payload_decision()
    assert d_after is not None
    # With budget 600/60=10 pf and fec_k=8, target_pf stays at 8 (fec_k
    # dominates). Budget drop has to be tighter than fec_k to move P.
    assert d_after.pps_budget <= 600.0


def test_sizer_grows_payload_under_very_tight_budget():
    """Budget below fec_k pulls target_pf below fec_k and grows P."""
    svc = _make_service(enable=True, target_fec_k=16)

    _feed(svc, 14000, 60)
    d_loose = svc.current_payload_decision()

    # Drop budget so budget_pf=5 (300 pps / 60 fps) < fec_k=16
    for _ in range(10):
        svc.observe_link_budget(300.0)
    _feed(svc, 14000, 60)
    d_tight = svc.current_payload_decision()

    assert d_tight is not None and d_loose is not None
    assert d_tight.payload >= d_loose.payload


def test_sizer_legacy_path_still_runs():
    """Enabling the sizer must not break the legacy FECController flow."""
    svc = _make_service(enable=True)
    _feed(svc, 10000, 100)
    legacy = svc.controller.get_current()
    assert legacy is not None
    assert legacy.k >= 1
    # Sizer also has an opinion
    assert svc.current_payload_decision() is not None


def test_injected_estimator_is_respected():
    """Caller can inject a preconfigured LinkBudgetEstimator."""
    est = LinkBudgetEstimator(fallback=9999.0)
    cfg = ControllerConfig(enable_variable_payload=True, target_fec_k=8)
    svc = FECControllerService(
        config=cfg,
        stat_port=0,
        wfb_control_port=0,
        dry_run=True,
        link_budget_estimator=est,
    )
    assert svc.link_budget is est
