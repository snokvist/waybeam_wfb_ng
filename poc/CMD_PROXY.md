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
| `HTTP_ERROR`     | 6 | venc unreachable or returned non-`ok` |
| `BAD_FORMAT`     | 7 | Malformed datagram (reserved) |
| `NOT_PERMITTED`  | 8 | Peer rejected (`cmd.loopback_only=true`) |

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

## Security notes

- The proxy only acts when `cmd.enabled` is true AND the MCS rx_ant
  socket is open — so it inherits the rx_ant peer surface.
- The whitelist (`cmd.allow_keys_mask`) is the primary safety boundary;
  set it to 0 to make the proxy purely diagnostic without disabling the
  rx_ant listener.
- For vehicle-local-only setups, set `cmd.loopback_only=true`.
- All values are clamped before HTTP dispatch — a malicious client
  cannot push the venc bitrate or payload outside the configured range.
- The proxy issues exactly one HTTP `GET` per accepted request. The
  rate limiter (`cmd.rate_limit_ms`) prevents accidental floods from
  re-amplifying onto the venc HTTP server.

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
