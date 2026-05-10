# venc_proxy — ground-side HTTP shim for waybeam_venc

`venc_proxy` is a small C binary that runs on the ground host and
exposes the same HTTP surface a GCS would normally call against
`waybeam_venc` on the vehicle. Each accepted call is translated into a
WCMD binary frame and shoved at `127.0.0.1:6600` where `wfb_tx_native`
forwards it across the radio uplink. On the vehicle,
`link_controller` already demultiplexes WCMD off its rx_ant socket
(see [`CMD_PROXY.md`](CMD_PROXY.md)) and replays the request as a real
HTTP GET against venc.

```
caller (curl / GCS)
       │  HTTP GET /api/v1/set?... | /request/idr
       ▼
   venc_proxy (ground)
       │  WCMD_REQ over UDP -> 127.0.0.1:6600
       ▼
   wfb_tx_native -u 6600
       │  ─── radio ───▶
       ▼
   wfb_rx -u 5801 (vehicle)
       │
       ▼
   link_controller (WCMD demux)
       │  HTTP GET /api/v1/...
       ▼
   waybeam_venc
```

From the caller's point of view it looks exactly like talking to venc.
That's the whole point: drop-in compatibility for tools that already
speak the venc API.

## Why a ground-side shim at all?

The wfb-ng radio uplink piggyback in `link_controller` only accepts the
fixed-size 16-byte WCMD format — it deliberately doesn't expose an HTTP
parser, range vocabulary, or query string surface to the air. This
proxy sits on the ground side so callers can keep using the venc HTTP
URL they already know, while the wire across the radio stays the
binary, whitelisted, validated WCMD format.

## Whitelisted surface

| Method | Path | WCMD key |
|--------|------|----------|
| GET    | `/api/v1/set?video0.bitrate=<kbps>`           | `WCMD_KEY_BITRATE_KBPS`  |
| GET    | `/api/v1/set?video0.fps=<fps>`                | `WCMD_KEY_FPS`           |
| GET    | `/api/v1/set?outgoing.maxPayloadSize=<bytes>` | `WCMD_KEY_PAYLOAD_BYTES` |
| GET    | `/request/idr`                                | `WCMD_KEY_FORCE_IDR`     |
| GET    | `/health`                                     | (local liveness probe)   |

`/api/v1/set` accepts multiple keys in a single query string. The
proxy emits one WCMD frame per recognised key. Unknown keys in a
multi-key set are ignored; the request still returns `200 OK` as long
as at least one key was dispatched.

Anything else returns `404`. All validation, range clamping, and
per-key rate limiting are enforced **on the vehicle** by the WCMD
proxy in `link_controller` — `venc_proxy` is intentionally a dumb
translator and adds no policy of its own.

## Replies are synthetic

The radio uplink is one-way at the wire level (see "Topologies" in
`CMD_PROXY.md`), so the WCMD_RESP that link_controller sends never
makes it back to the ground host. `venc_proxy` therefore replies
**immediately** with a fixed venc-shape JSON body:

| Path                | Body              |
|---------------------|-------------------|
| `/api/v1/set?...`   | `{"ok":true}`     |
| `/request/idr`      | `{"idr":true}`    |
| malformed query     | `400 {"ok":false}`|

This means **a 200 response from venc_proxy means "the WCMD frame
made it onto the local UDP socket"**, not "the vehicle accepted the
change". For end-to-end confirmation, query the vehicle's
`/cmd/status` endpoint over a sideband (eth or a second radio link)
or watch `/api/v1/config` change.

## Build & run

```sh
# native host build (default)
make -f Makefile.venc_proxy

# run on the ground host alongside wfb_tx_native -u 6600
sudo ./build/venc_proxy
# ...or, on a non-root dev box:
./build/venc_proxy --listen 8080

# fire requests like you would at venc
curl 'http://127.0.0.1/api/v1/set?video0.bitrate=8000'
curl 'http://127.0.0.1/api/v1/set?video0.fps=60&outgoing.maxPayloadSize=1400'
curl 'http://127.0.0.1/request/idr'
```

Port 80 needs `CAP_NET_BIND_SERVICE` (or root). Either run as root, or:

```sh
sudo setcap 'cap_net_bind_service=+ep' build/venc_proxy
```

## Options

| Flag                    | Default     | Notes                                     |
|-------------------------|-------------|-------------------------------------------|
| `--listen ADDR[:PORT]`  | `0.0.0.0:80`| Bind interface and TCP port               |
| `--upstream-port N`     | `6600`      | UDP port on `127.0.0.1` for WCMD frames   |
| `--dry-run`             | off         | Parse and log; never call `sendto()`      |
| `-v, --verbose`         | off         | Log each accepted request to stderr       |
| `-h, --help`            |             | Help                                      |

`--upstream-port` must match `wfb_tx_native -u <port>` on the same
host. The destination IP is hard-coded to `127.0.0.1` — the wire path
to the vehicle is always via the local `wfb_tx_native` instance, never
via direct routed IP from this proxy. (For ethernet topologies you
can talk WCMD directly to the vehicle by skipping the proxy entirely
and using the Python sender in `CMD_PROXY.md`.)

## Limits

- One-shot `Connection: close` per request. Keep-alive isn't worth
  the complexity for a tool that runs at human-pace request rates.
- No TLS. This is a localhost / LAN tool by design; the WCMD frames
  on the radio are the actual trust boundary.
- No batching: `/api/v1/set?a=...&b=...` emits N independent WCMD
  frames. The vehicle applies them in order; bitrate+payload are NOT
  applied atomically the way `venc_apply()` does in `link_controller`.
  If you need the atomic combo, drive bitrate via the FEC controller
  or use a sideband HTTP path direct to venc.
- HTTP request head must fit in 4096 bytes. Bodies are ignored
  (only GET is supported anyway).
