"""
FEC block wire-cost model.

wfb-ng Reed-Solomon encodes each block at the size of the largest source
packet in that block; every source packet smaller than that largest is
padded up for RS input, and every recovery symbol is emitted at that
padded size. This model reproduces that cost so the benchmark can
compare fixed-P vs variable-P policies on equal footing.

Assumptions (document them loudly so the numbers don't mislead):

- One FEC block == one emitted frame, unless the frame's packet count
  exceeds fec_k at the chosen payload, in which case the frame spills
  into a second block.
- Source packets carry exactly the RTP payload bytes (no RTP header
  accounted for; the comparison is like-for-like so it cancels).
- No channel loss, no retries. The model is for wire-cost accounting,
  not link reliability.
"""

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class BlockStats:
    k: int
    n: int
    largest_packet: int
    source_bytes: int         # sum of actual packet sizes
    source_padding: int       # bytes added to source packets for RS
    recovery_bytes: int       # (n - k) * largest_packet
    wire_bytes: int           # source_bytes + source_padding + recovery_bytes
    packets_in_block: int
    source_packets: int
    spilled_source_packets: int


def make_block(
    packets: list[int],
    fec_k: int,
    redundancy: float,
) -> BlockStats:
    """Pack up-to-fec_k packets into a block and compute wire cost.

    Args:
        packets: sizes (bytes) of source RTP payloads, in emit order.
        fec_k: block source capacity. Packets beyond fec_k spill to the
            next block (caller is responsible for handling them).
        redundancy: fraction in [0, 1). n = ceil(k / (1 - redundancy)).
            Clamped at 0.99 to avoid division by zero.

    Returns:
        BlockStats for the first block. The caller should consume
        `stats.source_packets` from the head of `packets` and pass the
        remainder to `make_block` again for any spill.
    """
    if fec_k < 1:
        raise ValueError(f"fec_k must be >= 1, got {fec_k}")
    if not packets:
        raise ValueError("packets must be non-empty")
    if redundancy < 0.0:
        raise ValueError(f"redundancy must be >= 0, got {redundancy}")

    block_sources = packets[:fec_k]
    k = len(block_sources)
    effective_red = min(redundancy, 0.99)
    n = max(k + 1, math.ceil(k / (1.0 - effective_red))) if effective_red > 0 else k + 1

    largest = max(block_sources)
    source_bytes = sum(block_sources)
    source_padding = k * largest - source_bytes
    recovery_bytes = (n - k) * largest
    wire_bytes = source_bytes + source_padding + recovery_bytes
    spilled = max(0, len(packets) - fec_k)

    return BlockStats(
        k=k,
        n=n,
        largest_packet=largest,
        source_bytes=source_bytes,
        source_padding=source_padding,
        recovery_bytes=recovery_bytes,
        wire_bytes=wire_bytes,
        packets_in_block=n,
        source_packets=k,
        spilled_source_packets=spilled,
    )


def pack_frame_into_blocks(
    packets: list[int],
    fec_k: int,
    redundancy: float,
) -> list[BlockStats]:
    """Pack a full frame's packets into 1+ blocks, capped at fec_k each."""
    if not packets:
        return []
    blocks: list[BlockStats] = []
    remaining = packets
    while remaining:
        block = make_block(remaining, fec_k, redundancy)
        blocks.append(block)
        remaining = remaining[block.source_packets:]
    return blocks
