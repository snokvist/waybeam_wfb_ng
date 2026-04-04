"""Tests for FECControllerService and FPSEstimator — sidecar protocol only."""

import asyncio
import math
import socket
import pytest

from fec_controller.config import ControllerConfig
from fec_controller.protocol import (
    pack_frame,
    pack_frame_base,
    pack_subscribe,
    FRAME_TYPE_I,
    FRAME_TYPE_P,
)
from fec_controller.service import FECControllerService, FPSEstimator, _SidecarProtocol


class TestFPSEstimator:

    def test_default_fps_before_data(self):
        est = FPSEstimator(default_fps=60.0)
        assert est.fps == 60.0

    def test_single_sample_returns_default(self):
        est = FPSEstimator(default_fps=60.0)
        fps = est.update(1_000_000)
        assert fps == 60.0

    def test_two_samples_gives_fps(self):
        est = FPSEstimator(alpha=1.0)  # instant tracking
        est.update(0)
        fps = est.update(16_667)  # ~60 fps interval in us
        assert fps == pytest.approx(60.0, rel=0.01)

    def test_120fps_estimation(self):
        est = FPSEstimator(alpha=1.0)
        est.update(0)
        fps = est.update(8_333)  # ~120 fps interval in us
        assert fps == pytest.approx(120.0, rel=0.01)

    def test_ewma_smoothing(self):
        est = FPSEstimator(alpha=0.05)
        for i in range(200):
            est.update(i * 16_667)
        assert est.fps == pytest.approx(60.0, rel=0.02)

    def test_ignores_zero_interval(self):
        est = FPSEstimator(default_fps=30.0)
        est.update(1000)
        fps = est.update(1000)  # same timestamp = zero interval
        assert fps == 30.0

    def test_converges_after_fps_change(self):
        est = FPSEstimator(alpha=0.1)
        # Start at 60fps
        for i in range(50):
            est.update(i * 16_667)
        assert est.fps == pytest.approx(60.0, rel=0.05)
        # Switch to 120fps
        base_us = 50 * 16_667
        for i in range(200):
            est.update(base_us + i * 8_333)
        assert est.fps == pytest.approx(120.0, rel=0.05)


class TestServiceFrameHandling:

    def _make_service(self, **kwargs):
        config = ControllerConfig()
        return FECControllerService(config, dry_run=True, **kwargs)

    def test_handle_frame_with_enc_info(self):
        service = self._make_service()
        for i in range(10):
            data = pack_frame(
                frame_ready_us=i * 16_667,
                frame_size_bytes=5000,
                seq_count=4,
            )
            service._handle_frame(data)
        assert service._frame_count == 10
        assert service.controller.get_current() is not None

    def test_handle_frame_base_only_uses_seq_count(self):
        """Without enc_info, service estimates frame_size = seq_count * MTU."""
        service = self._make_service()
        for i in range(5):
            data = pack_frame_base(
                frame_ready_us=i * 8_333,
                seq_count=7,
            )
            service._handle_frame(data)
        p = service.controller.get_current()
        assert p is not None
        # 7 * 1446 = 10122 -> k should be around 7-8 at low headroom
        assert p.k >= 5

    def test_frame_size_from_enc_trailer(self):
        service = self._make_service()
        for i in range(5):
            data = pack_frame(
                frame_ready_us=i * 8_333,
                frame_size_bytes=20000,
                seq_count=14,
            )
            service._handle_frame(data)
        p = service.controller.get_current()
        assert p is not None
        assert p.k >= 10

    def test_handle_packet_routes_frame(self):
        service = self._make_service()
        data = pack_frame(frame_ready_us=100_000, frame_size_bytes=5000)
        service._handle_packet(data)
        assert service._frame_count == 1

    def test_handle_packet_ignores_subscribe(self):
        service = self._make_service()
        service._handle_packet(pack_subscribe())
        assert service._frame_count == 0

    def test_handle_packet_ignores_garbage(self):
        service = self._make_service()
        service._handle_packet(b"\x00" * 64)
        assert service._frame_count == 0

    def test_handle_packet_ignores_short_data(self):
        service = self._make_service()
        service._handle_packet(b"\x00\x01")
        assert service._frame_count == 0

    def test_dry_run_does_not_open_socket(self):
        service = self._make_service()
        data = pack_frame(frame_ready_us=100_000, frame_size_bytes=5000)
        service._handle_frame(data)
        assert service.wfb_tx._sock is None

    def test_fps_estimation_through_service(self):
        """Verify the service derives correct fps from frame_ready_us."""
        service = self._make_service()
        # Send 120fps frames
        for i in range(100):
            data = pack_frame(
                frame_ready_us=i * 8_333,
                frame_size_bytes=5000,
            )
            service._handle_frame(data)
        assert service.fps_estimator.fps == pytest.approx(120.0, rel=0.05)

    def test_i_frame_type_processed(self):
        """I-frames with larger sizes should increase headroom."""
        service = self._make_service()
        for i in range(60):
            is_iframe = i % 30 == 0
            size = 12000 if is_iframe else 5000
            ftype = FRAME_TYPE_I if is_iframe else FRAME_TYPE_P
            data = pack_frame(
                frame_ready_us=i * 16_667,
                frame_size_bytes=size,
                frame_type=ftype,
            )
            service._handle_frame(data)
        # After seeing I-frame spikes, headroom should be above floor
        hr = service.controller.headroom_tracker.headroom
        assert hr > 1.05

    def test_full_frame_roundtrip_pipeline(self):
        """End-to-end: pack FRAME → service handler → controller produces params."""
        service = self._make_service()
        mtu = service.config.mtu
        frame_size = 30000
        seq_count = math.ceil(frame_size / mtu)

        for i in range(20):
            data = pack_frame(
                ssrc=0xDEADBEEF,
                rtp_timestamp=i * 90000,
                frame_id=i,
                frame_ready_us=i * 8_333,
                seq_first=(i * seq_count) & 0xFFFF,
                seq_count=seq_count,
                capture_us=max(0, i * 8_333 - 4000),
                last_pkt_send_us=i * 8_333 + 300,
                frame_size_bytes=frame_size,
                frame_type=FRAME_TYPE_P,
                qp=26,
                complexity=128,
            )
            service._handle_frame(data)

        p = service.controller.get_current()
        assert p is not None
        # 30000 bytes at headroom ~1.05, MTU 1446 -> ~22 packets -> k~22
        assert 18 <= p.k <= 26
        assert p.n > p.k


class TestServiceIntegration:

    @pytest.mark.asyncio
    async def test_udp_listener_receives_sidecar_frames(self):
        """Full integration: send real sidecar FRAME packets over UDP."""
        config = ControllerConfig()
        service = FECControllerService(config, stat_port=0, dry_run=True)

        loop = asyncio.get_event_loop()

        transport, protocol = await loop.create_datagram_endpoint(
            lambda: _SidecarProtocol(service._handle_packet),
            local_addr=("127.0.0.1", 0),
        )
        actual_port = transport.get_extra_info("sockname")[1]

        try:
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for i in range(5):
                data = pack_frame(
                    ssrc=0x01020304,
                    rtp_timestamp=i * 90000,
                    frame_id=i,
                    frame_ready_us=i * 16_667,
                    seq_first=i * 4,
                    seq_count=4,
                    capture_us=max(0, i * 16_667 - 4000),
                    last_pkt_send_us=i * 16_667 + 300,
                    frame_size_bytes=8000,
                    frame_type=FRAME_TYPE_P,
                )
                sender.sendto(data, ("127.0.0.1", actual_port))
            sender.close()

            await asyncio.sleep(0.05)

            assert service._frame_count == 5
            p = service.controller.get_current()
            assert p is not None
            assert p.k > 0
        finally:
            transport.close()

    @pytest.mark.asyncio
    async def test_udp_base_frames_without_trailer(self):
        """Integration with base-only FRAME messages (no encoder trailer)."""
        config = ControllerConfig()
        service = FECControllerService(config, stat_port=0, dry_run=True)

        loop = asyncio.get_event_loop()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _SidecarProtocol(service._handle_packet),
            local_addr=("127.0.0.1", 0),
        )
        actual_port = transport.get_extra_info("sockname")[1]

        try:
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for i in range(5):
                data = pack_frame_base(
                    frame_ready_us=i * 16_667,
                    seq_count=6,
                )
                sender.sendto(data, ("127.0.0.1", actual_port))
            sender.close()

            await asyncio.sleep(0.05)

            assert service._frame_count == 5
            p = service.controller.get_current()
            assert p is not None
        finally:
            transport.close()
