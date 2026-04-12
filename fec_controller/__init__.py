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
from fec_controller.payload_sizer import (
    Decision,
    MAX_PAYLOAD_HARD_CAP,
    choose_payload_size,
)
from fec_controller.frame_size_percentile import FrameSizePercentile
from fec_controller.link_budget import LinkBudgetEstimator
from fec_controller.encoder_sim import EncodedFrame, EncoderSim, SizeProfile
from fec_controller.block_model import BlockStats, make_block, pack_frame_into_blocks
from fec_controller.payload_benchmark import (
    BenchmarkConfig as PayloadBenchmarkConfig,
    PolicyStats,
    compare_policies,
    format_report,
)

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
    # Variable-payload sizer + supporting sim components
    "Decision",
    "MAX_PAYLOAD_HARD_CAP",
    "choose_payload_size",
    "FrameSizePercentile",
    "LinkBudgetEstimator",
    "EncodedFrame",
    "EncoderSim",
    "SizeProfile",
    "BlockStats",
    "make_block",
    "pack_frame_into_blocks",
    "PayloadBenchmarkConfig",
    "PolicyStats",
    "compare_policies",
    "format_report",
]
