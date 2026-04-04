"""
waybeam-hub adaptive FEC controller for wfb-ng.

Dynamically adjusts wfb-ng FEC parameters (k/n) based on real-time
video frame statistics from the encoding pipeline via the sidecar protocol.

Architecture:
  video encoder -> sidecar (FRAME messages) -> fec_controller -> wfb_tx (UDP control port)
"""

from fec_controller.protocol import (
    FRAME_BASE_SIZE,
    FRAME_EXT_SIZE,
    SIDECAR_MAGIC,
    SIDECAR_VERSION,
    MSG_SUBSCRIBE,
    MSG_FRAME,
    FLAG_KEYFRAME,
    FLAG_ENC_INFO,
    FRAME_TYPE_P,
    FRAME_TYPE_I,
    FRAME_TYPE_IDR,
    SidecarFrame,
    pack_subscribe,
    pack_frame,
    pack_frame_base,
    parse_frame,
    parse_header,
)
from fec_controller.config import ControllerConfig
from fec_controller.headroom import HeadroomTracker
from fec_controller.controller import FECParams, FECController
from fec_controller.wfb_control import WfbTxControl
from fec_controller.service import FECControllerService, FPSEstimator

__all__ = [
    "FRAME_BASE_SIZE",
    "FRAME_EXT_SIZE",
    "SIDECAR_MAGIC",
    "SIDECAR_VERSION",
    "MSG_SUBSCRIBE",
    "MSG_FRAME",
    "FLAG_KEYFRAME",
    "FLAG_ENC_INFO",
    "FRAME_TYPE_P",
    "FRAME_TYPE_I",
    "FRAME_TYPE_IDR",
    "SidecarFrame",
    "pack_subscribe",
    "pack_frame",
    "pack_frame_base",
    "parse_frame",
    "parse_header",
    "ControllerConfig",
    "HeadroomTracker",
    "FECParams",
    "FECController",
    "WfbTxControl",
    "FECControllerService",
    "FPSEstimator",
]
