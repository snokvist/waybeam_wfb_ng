## RTP Timing Sidecar

An optional out-of-band UDP channel that sends per-frame timing metadata
alongside the RTP video stream. Set `outgoing.sidecarPort=0` to disable it.

### Purpose

When enabled, the sidecar provides frame-level diagnostics for the entire
sender-side pipeline:

```
capture_us → [encode] → frame_ready_us → [packetise+send] → last_pkt_send_us
                                                              ↕ (network)
                                                        recv_last_us (probe)
```

This enables measurement of:
- **Encode duration** — time from sensor capture to encoder output
- **Send spread** — time to packetise and hand all RTP packets to the kernel
- **One-way latency** — frame-ready on venc to first-packet-received on ground
  (requires clock synchronisation)
- **Frame intervals** — jitter and regularity of both sender and receiver clocks
- **RTP packet counts and gaps** — per-frame packet accounting
- **Encoded frame size / type / QP** — when Star6E scene detection is active
- **Scene detection state** — complexity, scene-change flag, IDR decision, frames-since-IDR

### Enabling

Set the sidecar port in the configuration:

```sh
curl "http://<device-ip>:<port>/api/v1/set?outgoing.sidecar_port=6666"
```

Or in `/etc/venc.json`:

```json
"outgoing": { "sidecarPort": 6666 }
```

A pipeline restart is required after changing this setting. The sidecar
socket is silent until a probe subscribes — zero network overhead when no
probe is connected.

When the sidecar is disabled (port 0), no socket is created and there is
no runtime overhead.

### Wire Protocol

The sidecar uses a simple binary UDP protocol:

| Message | Direction | Size | Purpose |
|---------|-----------|------|---------|
| SUBSCRIBE | probe -> venc | 8 B | Start/refresh metadata subscription |
| FRAME | venc -> probe | 52, 64, 68, or 80 B | Per-frame timing + RTP seq, optional ENC_INFO trailer (12 B), optional TRANSPORT_INFO trailer (16 B) |
| SYNC_REQ | probe -> venc | 16 B | NTP-style clock offset request |
| SYNC_RESP | venc -> probe | 32 B | Clock offset response (t1, t2, t3) |

All messages share a common 6-byte header: 4-byte magic (`0x52545053` =
"RTPS"), 1-byte version, 1-byte message type. Fields are network byte order.

Subscription expires after 5 seconds without any probe message. Both
SUBSCRIBE and SYNC_REQ refresh the expiry timer.

FRAME has up to two optional trailers, gated by independent flag bits in the
base frame's `flags` byte. They appear in fixed order (ENC_INFO first, then
TRANSPORT_INFO), so the offset of the second trailer depends on whether the
first is present:

- `FLAG_ENC_INFO` (`0x02`) — 12-byte encoder telemetry trailer:
  `frame_size_bytes`, `frame_type`, `qp`, `complexity`, `scene_change`,
  `gop_state`, `idr_inserted`, `frames_since_idr`. Star6E with
  `video0.scene_threshold>0`. Maruko and timing-only Star6E runs omit it.
- `FLAG_TRANSPORT_INFO` (`0x04`) — 16-byte producer-output telemetry trailer:
  `fill_pct`, `in_pressure`, `transport_drops`, `pressure_drops`,
  `packets_sent`. Emitted when the active output transport reports a queue
  fill model (shm:// always; unix:// and udp:// in venc 0.9.2+). When
  ENC_INFO is absent, this trailer slides up to land directly at offset 52
  (total packet size 68); when both are present it lives at offset 64
  (total 80). `fill_pct` / `in_pressure` / `pressure_drops` are
  authoritative across all transports; `transport_drops` / `packets_sent`
  are authoritative for shm:// in v0.9.2 (unix:// / udp:// emit 0 pending
  socket-side counter instrumentation).

Forward-compat: probes that don't recognise a flag simply read the base
frame (and any trailer they do recognise) and ignore the trailing bytes.
No protocol version bump.

Link-control / FEC usage:
- RTP video keeps using `outgoing.server` as usual.
- Set `outgoing.sidecarPort` to expose sidecar metadata on a separate UDP port.
- Base timing fields are available whenever the sidecar is enabled.
- The extra encoder trailer requires Star6E with `video0.scene_threshold>0`.
- The transport trailer requires venc 0.9.2+ with backpressure-aware output
  (`outgoing.backpressure=true`); link_controller surfaces the values via
  `/status` (`transport.*`) and gates a one-tick FEC suppression on
  `fill_pct ≥ 80` or any new `pressure_drops` (`fec.skip_on_backpressure`,
  default on).
- The sender tracks one active sidecar subscriber at a time; the most recent
  probe or consumer to subscribe receives the frame metadata.

### Reference Probe

A host-native reference probe is included at `tools/rtp_timing_probe.c`.
It listens for RTP on one port and communicates with the venc sidecar on
another, correlating frames by (SSRC, RTP timestamp).

Build (no cross-compiler needed):

```sh
make rtp_timing_probe
```

Usage:

```sh
./rtp_timing_probe --venc-ip <device-ip> [--rtp-port 5600] [--sidecar-port 6666] [--stats]
```

Without `--stats`, the probe outputs tab-separated frame records to stdout
(one line per frame) suitable for piping to analysis tools. The TSV includes
columns for all timing fields, sequence numbers, gaps, intervals, estimated
latency, and optional encoder-feedback fields when the sidecar trailer is
present. For timing-only frames, the encoder-feedback columns print `-`.

With `--stats`, a summary is printed to stderr on exit:

```
=== Timing Probe Summary ===

Duration:             20.0 s
Frames:               936 (46.8 fps)
RTP packets:          8484 (9.1 avg/frame)
RTP gaps:             0

--- Send spread (frame_ready -> last_pkt_send) ---
  Mean:    294 us
  P50:     265 us
  P95:     331 us
  P99:     1710 us

--- Encode duration (capture -> frame_ready) ---
  Mean:    4254 us

--- Clock sync ---
  Samples:  8
  Best RTT: 347 us
```

The probe uses burst-then-coast clock synchronisation: 8 fast samples at
200 ms intervals, then one sample every 10 seconds. Only the sample with
the lowest RTT is used for offset estimation.

### Sidecar Overhead

At 90 fps with an active subscriber:
- **venc -> probe**: ~90 frame packets/s (52 B each) + sync responses
- **probe -> venc**: ~0.5 subscribe/s + ~0.1 sync/s
- **Bandwidth**: ~40 kbps total (both directions)
- **CPU**: single `poll()` per frame + one `sendto()` per frame

When no probe is subscribed, the sidecar socket exists but no packets
are sent.

### Adaptive FEC Controller (sidecar consumer)

The `fec_controller` module (`fec_controller/`) subscribes to the sidecar
and uses FRAME messages to dynamically adjust wfb-ng FEC parameters.

It consumes the same sidecar channel as `rtp_timing_probe` — the venc
tracks one subscriber at a time, so only one consumer can be active.
If both the probe and FEC controller need to run simultaneously, a
local relay/multiplexer would be needed (not yet implemented).

See the top-level `README.md` for controller architecture and usage.
