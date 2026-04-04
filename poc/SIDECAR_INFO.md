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
| FRAME | venc -> probe | 52 B base, 64 B with trailer | Per-frame timing + RTP sequence info, plus optional encoder telemetry |
| SYNC_REQ | probe -> venc | 16 B | NTP-style clock offset request |
| SYNC_RESP | venc -> probe | 32 B | Clock offset response (t1, t2, t3) |

All messages share a common 6-byte header: 4-byte magic (`0x52545053` =
"RTPS"), 1-byte version, 1-byte message type. Fields are network byte order.

Subscription expires after 5 seconds without any probe message. Both
SUBSCRIBE and SYNC_REQ refresh the expiry timer.

When Star6E adaptive encoder control is enabled, `FRAME` appends a 12-byte
trailer carrying `frame_size_bytes`, `frame_type`, `qp`, `complexity`,
`scene_change`, `idr_inserted`, and `frames_since_idr`.
Maruko and timing-only Star6E runs keep sending the original 52-byte frame.

Link-control / FEC usage:
- RTP video keeps using `outgoing.server` as usual.
- Set `outgoing.sidecarPort` to expose sidecar metadata on a separate UDP port.
- Base timing fields are available whenever the sidecar is enabled.
- The extra encoder trailer requires Star6E with `video0.scene_threshold>0`.
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
