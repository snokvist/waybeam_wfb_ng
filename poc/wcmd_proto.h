#ifndef WCMD_PROTO_H
#define WCMD_PROTO_H

/*
 * waybeam_venc command proxy — wire protocol.
 *
 * A small binary command channel multiplexed onto the rx_ant UDP listener
 * (default :5801, production :6600) of poc/link_controller. The ground
 * station already pushes wfb_rx -Y JSON to that port; this protocol uses
 * a 4-byte magic prefix ("WCMD") that does not collide with JSON, so the
 * link_controller dispatcher can tell binary command frames apart from
 * rx_ant JSON without parsing.
 *
 * link_controller acts as the proxy: it receives CMD_REQ datagrams,
 * validates and clamps the value, and translates the request to a single
 * HTTP GET against the local venc /api/v1 surface. The HTTP outcome is
 * returned to the originating socket as a CMD_RESP.
 *
 * Design parallels rtp_sidecar.h: 6-byte common header, network byte
 * order, #pragma pack(push,1) so on-the-wire size is fixed and stable.
 *
 *   ground-side cmd injector              vehicle link_controller
 *   ─────────────────────────             ─────────────────────────
 *      WCMD_REQ(key,value)  ──UDP──▶      multiplex with rx_ant JSON
 *                                         clamp + rate-limit + dispatch
 *                                         GET /api/v1/set?key=value
 *      WCMD_RESP(status)    ◀──UDP──      reply to recvfrom() peer
 *
 * Forward-compat: probes that don't recognise a future key/status enum
 * value treat it as UNKNOWN_KEY / OK respectively. Trailing bytes are
 * ignored; never bump the version for additive fields.
 */

#include <stdint.h>

#define WCMD_MAGIC          0x57434D44u  /* "WCMD" big-endian */
#define WCMD_VERSION        1

/* Message types */
#define WCMD_MSG_REQ        1   /* injector → proxy */
#define WCMD_MSG_RESP       2   /* proxy   → injector */

/*
 * Whitelisted command keys.
 *
 * The proxy refuses anything not in this enum. New keys may be added in
 * later versions without bumping WCMD_VERSION; older proxies reply with
 * WCMD_STATUS_UNKNOWN_KEY and the injector can fall back accordingly.
 */
#define WCMD_KEY_BITRATE_KBPS    1   /* video0.bitrate, units = kbps */
#define WCMD_KEY_FPS             2   /* video0.fps */
#define WCMD_KEY_PAYLOAD_BYTES   3   /* outgoing.maxPayloadSize */
#define WCMD_KEY_FORCE_IDR       4   /* GET /request/idr (value ignored) */

/* Status codes returned in CMD_RESP */
#define WCMD_STATUS_OK           0
#define WCMD_STATUS_DISABLED     1   /* cmd subsystem off (cmd.enabled=false) */
#define WCMD_STATUS_UNKNOWN_KEY  2   /* key not recognised by this version  */
#define WCMD_STATUS_KEY_BLOCKED  3   /* key disabled in cmd.allow_keys_mask */
#define WCMD_STATUS_OUT_OF_RANGE 4   /* value clamped or rejected           */
#define WCMD_STATUS_RATE_LIMITED 5   /* per-key min interval not elapsed    */
#define WCMD_STATUS_HTTP_ERROR   6   /* venc unreachable or returned !ok    */
#define WCMD_STATUS_BAD_FORMAT   7   /* malformed datagram                  */
#define WCMD_STATUS_NOT_PERMITTED 8  /* peer rejected (loopback-only mode)  */

#pragma pack(push, 1)

/**
 * Command request — injector → proxy, 16 bytes.
 *
 * The proxy never replies on the rx_ant socket if the magic check fails,
 * so a stray JSON datagram is silently passed through to the JSON
 * parsers downstream (current parsers reject non-rx_ant frames already).
 */
typedef struct {
	uint32_t magic;        /* WCMD_MAGIC                                  */
	uint8_t  version;      /* WCMD_VERSION                                */
	uint8_t  msg_type;     /* WCMD_MSG_REQ                                */
	uint16_t seq;          /* injector correlation id; echoed in resp    */
	uint8_t  key;          /* WCMD_KEY_*                                  */
	uint8_t  flags;        /* reserved, must be 0                         */
	uint16_t _pad;         /* reserved, must be 0                         */
	int32_t  value;        /* signed; ignored for FORCE_IDR               */
} WcmdReq;                 /* 16 bytes */

/**
 * Command response — proxy → injector, 16 bytes.
 *
 * Sent to the recvfrom() source address of the request. status reflects
 * proxy validation; http_status is the HTTP response code from venc, or
 * 0 if no HTTP request was issued (request rejected before dispatch).
 * applied_value is the value the proxy actually applied after clamping;
 * for FORCE_IDR it is 0.
 */
typedef struct {
	uint32_t magic;        /* WCMD_MAGIC                                  */
	uint8_t  version;      /* WCMD_VERSION                                */
	uint8_t  msg_type;     /* WCMD_MSG_RESP                               */
	uint16_t seq;          /* echo of WcmdReq.seq                         */
	uint8_t  status;       /* WCMD_STATUS_*                               */
	uint8_t  key;          /* echo of WcmdReq.key                         */
	uint16_t http_status;  /* venc HTTP response code, or 0               */
	int32_t  applied_value;/* post-clamp value; 0 for FORCE_IDR           */
} WcmdResp;                /* 16 bytes */

#pragma pack(pop)

#endif /* WCMD_PROTO_H */
