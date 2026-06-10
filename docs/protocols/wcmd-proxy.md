# venc Command Proxy

A small binary command channel multiplexed onto the existing `link_controller`
rx_ant UDP listener (default `127.0.0.1:5801`, production `:6600`). The
ground station already pushes `wfb_rx -Y` rx_ant JSON across that port; the
proxy sniffs a 4-byte magic prefix (`"WCMD"`) ahead of the JSON parser to
peel binary command frames off the same socket and translate each one into
a single HTTP `GET` against the local venc `/api/v1` surface.

The proxy lives entirely inside `link_controller` — no new processes, no
new sockets, no patches to `waybeam_venc`.

```
ground host                       vehicle (link_controller + venc)
─────────────                     ─────────────────────────────────

cmd injector ──UDP──▶ rx_ant socket ──┬─▶  rx_ant JSON   ──▶ MCS scorer
                                       └─▶  WCMD_REQ      ──▶ proxy ──▶ HTTP /api/v1
                                                         ◀── reply
WCMD_RESP    ◀──UDP──  reply via sendto()
```

## Wire format

All multi-byte fields are network byte order. See
[`wcmd_proto.h`](wcmd_proto.h) for the canonical layout.

### Common header (6 bytes)

| Offset | Type     | Field   | Value             |
|--------|----------|---------|-------------------|
| 0      | uint32   | magic   | `0x57434D44` (`"WCMD"`) |
| 4      | uint8    | version | `1`               |
| 5      | uint8    | msg_type| `1` REQ or `2` RESP |

### `WCMD_MSG_REQ` (16 bytes total)

| Offset | Type   | Field   | Notes |
|--------|--------|---------|-------|
| 6      | uint16 | seq     | Client correlation id; echoed in reply |
| 8      | uint8  | key     | `WCMD_KEY_*` (see below) |
| 9      | uint8  | flags   | Reserved, must be 0 |
| 10     | uint16 | _pad    | Reserved, must be 0 |
| 12     | int32  | value   | Signed; ignored for `FORCE_IDR` |

### `WCMD_MSG_RESP` (16 bytes total)

| Offset | Type   | Field         | Notes |
|--------|--------|---------------|-------|
| 6      | uint16 | seq           | Echo of request |
| 8      | uint8  | status        | `WCMD_STATUS_*` |
| 9      | uint8  | key           | Echo of request |
| 10     | uint16 | http_status   | venc HTTP code (200/400) or 0 if no HTTP issued |
| 12     | int32  | applied_value | Post-clamp value the proxy used; `0` for `FORCE_IDR` |

### Keys

| Key | Value | Maps to |
|-----|-------|---------|
| `WCMD_KEY_BITRATE_KBPS`  | 1 | `GET /api/v1/set?video0.bitrate=<kbps>` |
| `WCMD_KEY_FPS`           | 2 | `GET /api/v1/set?video0.fps=<fps>` |
| `WCMD_KEY_PAYLOAD_BYTES` | 3 | `GET /api/v1/set?outgoing.maxPayloadSize=<bytes>` |
| `WCMD_KEY_FORCE_IDR`     | 4 | `GET /request/idr` (value ignored) |

Anything not in this table comes back as `WCMD_STATUS_UNKNOWN_KEY`. New
keys may be added in later revisions without bumping `WCMD_VERSION`;
older proxies simply respond with `UNKNOWN_KEY` and the injector falls
back accordingly.

### Status codes

| Status | Value | Meaning |
|--------|-------|---------|
| `OK`             | 0 | venc accepted the change (or dry-run) |
| `DISABLED`       | 1 | `cmd.enabled=false` (or no rx_ant socket) |
| `UNKNOWN_KEY`    | 2 | Key not recognised by this version |
| `KEY_BLOCKED`    | 3 | Key blocked by `cmd.allow_keys_mask` |
| `OUT_OF_RANGE`   | 4 | Value rejected after clamp attempt |
| `RATE_LIMITED`   | 5 | Same key applied < `cmd.rate_limit_ms` ago |
| `HTTP_ERROR`     | 6 | venc unreachable or returned non-`ok` body |
| `BAD_FORMAT`     | 7 | Reserved. The demux currently drops malformed frames silently rather than dispatching, so this status is never emitted today. The counter `rejected_format` in `/cmd/status` mirrors that and stays at 0; both fields are kept for forward-compat. |
| `NOT_PERMITTED`  | 8 | Peer rejected (`cmd.loopback_only=true`; see "Topologies" below for the radio-uplink caveat) |

`http_status` carries the parsed numeric HTTP status code from venc's
response (200, 400, 500, …) or 0 if no HTTP request was issued. **Branch
on `status` first.** venc returns HTTP `200 OK` even when it rejects a
`/set` (the body carries `"ok":false`), so the combination
`http_status=200` + `status=HTTP_ERROR` means "venc was reachable but
refused the value".

### Validation pipeline (in order)

1. Magic + version + length — silently passed through to the JSON parser
   if it doesn't match (so a stray rx_ant frame is never mishandled).
2. `cmd.enabled` — `DISABLED` if off.
3. `cmd.loopback_only` — `NOT_PERMITTED` if set and source IP ≠ 127.0.0.1.
4. Key in range — `UNKNOWN_KEY` otherwise.
5. `cmd.allow_keys_mask` — `KEY_BLOCKED` if the bit for this key is clear.
6. Range clamp — `OUT_OF_RANGE` if no plausible window. Otherwise the
   value is clamped to `[lo, hi]` and the clamped value is reported back
   as `applied_value`. For `BITRATE_KBPS` the window is the FEC
   subsystem's `bitrate_min_kbps` / `bitrate_max_kbps` if both are set,
   otherwise `cmd.bitrate_min_kbps` / `cmd.bitrate_max_kbps`.
7. Rate limit — `RATE_LIMITED` if the same key was applied less than
   `cmd.rate_limit_ms` ago. The `applied_value` field still reflects the
   post-clamp value the request *would have* applied.
8. HTTP `GET` — `HTTP_ERROR` if the request fails or venc returns no
   `"ok":true` (or `"idr":true` for `FORCE_IDR`).

`--dry-run` skips step 8 and reports `OK` with `http_status=200`.

## Tunables

All under the `cmd.*` namespace, live-mutable via the existing
`/set?key=value` API. Defaults shown.

| Key | Default | Purpose |
|-----|---------|---------|
| `cmd.enabled`         | `true` | Master switch (auto-off when no rx_ant socket) |
| `cmd.loopback_only`   | `false` | Refuse non-127.0.0.1 peers |
| `cmd.rate_limit_ms`   | `100` | Min ms between same-key applies (0 disables) |
| `cmd.allow_keys_mask` | `0x0F` | Bitmask: 1=br, 2=fps, 4=pl, 8=idr |
| `cmd.bitrate_min_kbps`| `100` | Bitrate floor (used when FEC range unset) |
| `cmd.bitrate_max_kbps`| `25000` | Bitrate ceiling (used when FEC range unset) |
| `cmd.fps_min`         | `1`   | FPS floor |
| `cmd.fps_max`         | `240` | FPS ceiling |
| `cmd.payload_min`     | `576` | RTP payload floor (matches venc API floor) |
| `cmd.payload_max`     | `1474`| RTP payload ceiling (1500 MTU minus headroom) |

## REST status

```sh
curl -s http://127.0.0.1:8765/cmd/status
```

Returns counters (recv / accepted / rejected_*), per-key last-apply ages,
and the last request's status / key / value / http_status.

## Recipe — drive the proxy from a host

A reference Python sender lives at `/tmp/wcmd_send.py` after running the
smoke test. The minimal end-to-end pattern is:

```python
import socket, struct
WCMD_MAGIC, WCMD_VERSION, WCMD_MSG_REQ = 0x57434D44, 1, 1
KEY_BITRATE_KBPS = 1
pkt = struct.pack("!IBBHBBHi",
    WCMD_MAGIC, WCMD_VERSION, WCMD_MSG_REQ,
    1,                # seq
    KEY_BITRATE_KBPS, # key
    0, 0,             # flags, _pad
    8000)             # value (kbps)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(pkt, ("192.168.1.13", 6600))
data, _ = s.recvfrom(64)   # blocks; consider settimeout()
```

Decode the 16-byte response with the same struct format.

## Topologies

### Direct ethernet — vehicle reachable on a routed IP

```
ground ─ (IP) ─▶ vehicle:rx_ant_port ─▶ link_controller demux
                                           │
                                           └─▶ WCMD_RESP back to source IP
```

The peer address visible to the proxy is the actual ground-host IP. Both
WCMD_REQ and WCMD_RESP traverse the IP path, so the response always
makes it back to the sender (subject to normal UDP loss).

### One-way wfb-ng radio uplink (typical FPV deployment)

```
ground host                         vehicle
─────────────                       ─────────────────────────────
WCMD_REQ ──▶ wfb_tx_native -u 6600 ─radio─▶ wfb_rx -u 5801
                                              │ (decapsulated UDP
                                              │  with src 127.0.0.1)
                                              ▼
                            link_controller --stats 0.0.0.0:5801
                                              │
WCMD_RESP ◀ ─ ─ ─ ─ ─ no return path ─ ─ ─ ─ ─┘
```

Two consequences of this topology that operators must keep in mind:

1. **No return path.** `wfb_tx`/`wfb_rx` is one-way at the wire level.
   The proxy still calls `sendto()` on the rx_ant socket, but the
   datagram lands on the vehicle's loopback (where `wfb_rx` is the
   sender) and never crosses the radio. Verify command landing via
   `curl /cmd/status` or `/api/v1/config` over a sideband (eth or a
   second radio link), not by waiting on `recvfrom`.

2. **`cmd.loopback_only` does NOT gate radio peers.** Because
   `wfb_rx -u <port>` decapsulates radio frames into local UDP
   datagrams, every radio-uplinked WCMD frame appears to the proxy
   with peer `127.0.0.1:<ephemeral>`. With `loopback_only=true` they
   are accepted as if they were on-host. The trust boundary in this
   topology is the wfb-ng drone key + stream id, not the IP path.
   Use `cmd.allow_keys_mask` and per-key clamps as the real safety
   boundary.

For deployments that mix both topologies (e.g. ethernet provisioning +
radio uplink), keep `loopback_only=false` and rely on
`cmd.allow_keys_mask` plus the FEC `bitrate_min/max` window to bound
what an attacker can do.

## Failsafe tuning (rx_ant uplink)

The proxy reuses the MCS rx_ant socket, so MCS scoring and the cmd
proxy are co-located on the same port. That means the MCS subsystem's
**failsafe watchdog** observes the same uplink the proxy depends on,
and a lossy uplink can flap MCS while still landing WCMD frames.

The watchdog forces bucket 0 (lowest MCS in the active range) when no
rx_ant datagram has arrived for `mcs.failsafe_timeout_s` (default
`0.5 s`). On a wfb-ng `-k 1 -n 2` (50 % parity) uplink with 10 Hz rx_ant
emit cadence, fragmented JSON datagrams lose ~5 consecutive frames as
a single "bad burst", which produces 0.5–2.0 s rx_ant gaps on an
otherwise-healthy radio link. With the default threshold each burst
trips the failsafe → MCS=1 → bitrate pre-drop → SET_RADIO → recovery
within ~3 s. The `commit_count` and `ordered drop` lines in the log
show this clearly.

**Recommended starting point** for typical FPV uplinks (set via
`--failsafe` at launch, or live via `/set`):

```
mcs.failsafe_timeout_s = 2.0    (was 0.5)
```

(Recovery is immediate on the next rx_ant datagram under the unified
PER-probe law — the legacy `failsafe_recovery_consecutive` /
`--recover-consec` unfreeze counter was removed 2026-06-10.)

The bundled `vehicle/init/S99wfb` template wires this as the
`WFB_FAILSAFE_S=2.0` env var. If your uplink is
even lossier (counted in `wfb.log`: `grep -c 'no rx_ant' /tmp/wfb.log`
shows the trip rate), raise `WFB_FAILSAFE_S` further or tighten the
wfb-ng parity ratio of the rx_ant return path.

The trade-off is direct: a longer failsafe window means a longer
"blind" interval before the controller protects against a real ground
outage. For bench operation 2–5 s is fine; for long-range flights you
want to weigh that against how quickly you'd want MCS to drop on a
genuine link failure.

## Security notes

- The proxy only acts when `cmd.enabled` is true AND the MCS rx_ant
  socket is open — so it inherits the rx_ant peer surface.
- The whitelist (`cmd.allow_keys_mask`) is the primary safety boundary;
  set it to 0 to make the proxy purely diagnostic without disabling the
  rx_ant listener.
- All values are clamped before HTTP dispatch — a malicious client
  cannot push the venc bitrate, fps, or payload outside the configured
  range. For bitrate, the FEC subsystem's `bitrate_min/max` window
  takes precedence when set, so the proxy and FEC controller agree on
  the safe envelope.
- The proxy issues exactly one HTTP `GET` per accepted request. The
  per-key rate limiter (`cmd.rate_limit_ms`, default 100 ms) absorbs
  accidental floods so they don't re-amplify onto the venc HTTP server.
- See "Topologies" above for the radio-uplink caveat on
  `cmd.loopback_only`.

## Dispatch trade-offs

- Magic prefix (`"WCMD"`) instead of a separate UDP port: keeps the
  port table unchanged; doesn't conflict with rx_ant JSON because the
  prefix never appears at offset 0 of valid JSON.
- Binary instead of JSON: 16 B / packet (vs ~80 B JSON), no parser, no
  allocator, no string truncation handling on a 2 KB datagram buffer.
- Whitelisted keys instead of opaque `/set?...` passthrough: the proxy
  guarantees venc can never be asked to do anything outside the four
  documented operations, so no scoped-credentials or key-firewalling
  story is required on the ground side.

## Limits

- 1 dispatch per datagram, no batch encoding.
- 32 datagrams drained per `poll()` wakeup (shared with rx_ant); a
  flood spills into the next iteration.
- HTTP timeout is 500 ms (inherited from `http_get`).
- One outstanding HTTP request at a time per `link_controller` instance
  (single-threaded `poll()` loop).
