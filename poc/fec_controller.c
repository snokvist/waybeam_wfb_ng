/*
 * fec_controller — Simple C POC for adaptive wfb-ng FEC sizing.
 *
 * Subscribes to waybeam_venc sidecar (per-frame metadata), computes k/n
 * sized for the average frame (EWMA + bounded headroom), gates updates
 * asymmetrically (fast up, slow down), and sends CMD_SET_FEC to wfb_tx.
 *
 * Also queries wfb_tx radio params (mcs/bw/gi) and the venc HTTP API for
 * the current bitrate. If the configured bitrate exceeds the safe
 * link budget (safety * phy_mbps * k/n), reduces video0.bitrate via
 * /api/v1/set?video0.bitrate=N.
 *
 * Single thread, poll() loop. No libs beyond libc.
 *
 * Target: SigmaStar Infinity6E (armv7l / OpenIPC). Cross-build with the
 * star6e toolchain. Runs on-device; all three endpoints are 127.0.0.1.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Portable big-endian uint64 decode (avoids libc be64toh dependency). */
static inline uint64_t be64_read(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;
	return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
	       ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
	       ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
	       ((uint64_t)b[6] <<  8) |  (uint64_t)b[7];
}

/* ── Sidecar wire protocol (matches waybeam_venc/include/rtp_sidecar.h) ─── */

#define SC_MAGIC        0x52545053u   /* "RTPS" big-endian */
#define SC_VERSION      1
#define SC_MSG_SUBSCRIBE 1
#define SC_MSG_FRAME     2
#define SC_FLAG_KEYFRAME 0x01
#define SC_FLAG_ENC_INFO 0x02

#pragma pack(push, 1)
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;
	uint8_t  pad[2];
} SidecarSubscribe;   /* 8 bytes */

typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;
	uint8_t  stream_id;
	uint8_t  flags;
	uint32_t ssrc;
	uint32_t rtp_timestamp;
	uint64_t frame_id;
	uint64_t frame_ready_us;
	uint16_t seq_first;
	uint16_t seq_count;
	uint64_t capture_us;
	uint64_t last_pkt_send_us;
} SidecarFrame;       /* 52 bytes */

typedef struct {
	uint32_t frame_size_bytes;
	uint8_t  frame_type;
	uint8_t  qp;
	uint8_t  complexity;
	uint8_t  scene_change;
	uint8_t  gop_state;
	uint8_t  idr_inserted;
	uint16_t frames_since_idr;
} SidecarEncInfo;     /* 12 bytes */
#pragma pack(pop)

/* ── wfb_tx control protocol (matches poc/build/wfb-ng/src/tx_cmd.h) ───── */

#define CMD_SET_FEC    1
#define CMD_SET_RADIO  2
#define CMD_GET_RADIO  4

#pragma pack(push, 1)
typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	union {
		struct { uint8_t k, n; }                    set_fec;
		struct {
			uint8_t stbc;
			uint8_t ldpc;
			uint8_t short_gi;
			uint8_t bandwidth;
			uint8_t mcs_index;
			uint8_t vht_mode;
			uint8_t vht_nss;
		}                                            set_radio;
		struct { uint8_t pad; }                     get_radio;
	} u;
} CmdReq;

typedef struct {
	uint32_t req_id;
	uint32_t rc;
	union {
		struct {
			uint8_t stbc;
			uint8_t ldpc;
			uint8_t short_gi;
			uint8_t bandwidth;
			uint8_t mcs_index;
			uint8_t vht_mode;
			uint8_t vht_nss;
		} get_radio;
	} u;
} CmdResp;
#pragma pack(pop)

/* ── Clock ───────────────────────────────────────────────────────────── */

static uint64_t now_us(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ── Config / globals ─────────────────────────────────────────────────── */

typedef struct {
	/* Endpoints */
	char     sidecar_host[64];
	uint16_t sidecar_port;
	char     wfb_host[64];
	uint16_t wfb_port;
	char     venc_host[64];
	uint16_t venc_port;

	/* FEC sizing */
	int      mtu;            /* RTP payload budget per packet */
	int      min_k, max_k;
	int      min_n, max_n;
	float    ewma_alpha;
	float    headroom_min;
	float    headroom_max;
	float    headroom_margin;
	float    headroom_window_s;

	/* Gating */
	int      k_hyst_up;
	int      k_hyst_down;
	float    cooldown_up_s;
	float    cooldown_down_s;

	/* Link budget */
	float    safety_margin;  /* fraction of phy_mbps usable after overhead */

	/* Tick intervals */
	float    subscribe_s;
	float    radio_poll_s;
	float    bitrate_poll_s;

	/* MCS scaler */
	bool     mcs_enable;
	int      mcs_min;
	int      mcs_max;
	char     rssi_stream[256];   /* "" = disabled; text stream */
	uint16_t rssi_udp_port;      /* 0 = disabled; UDP listener */
	float    rssi_silence_s;
	float    rssi_ewma_alpha;
	float    mcs_climb_s;
	float    mcs_drop_s;
	float    mcs_cooldown_s;

	/* Post-MCS-drop FEC-parity boost */
	float    boost_s;
	float    boost_mult;

	bool     dry_run;
	int      verbose;
} Config;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

/* Monotonic timestamp prefix: "[fec t=  12.345] …"
 * Set once at startup via log_init(); every line shows seconds since start,
 * which makes it easy to see how fast we're reacting (intervals read off
 * the column directly). */
static uint64_t g_log_start_us = 0;
static void log_init(void) { g_log_start_us = now_us(); }
static double log_rel_s(void)
{
	return (double)(now_us() - g_log_start_us) / 1e6;
}

#define LOGV(cfg, fmt, ...) \
	do { if ((cfg)->verbose) \
		fprintf(stderr, "[fec t=%8.3f] " fmt "\n", log_rel_s(), ##__VA_ARGS__); \
	} while (0)
#define LOG(fmt, ...) \
	fprintf(stderr, "[fec t=%8.3f] " fmt "\n", log_rel_s(), ##__VA_ARGS__)

/* ── Redundancy curve (k → redundancy fraction, linear interp) ───────── */

typedef struct { int k; float r; } RedundancyPoint;
static const RedundancyPoint REDUNDANCY_CURVE[] = {
	{  1, 0.50f },
	{  4, 0.40f },
	{  8, 0.33f },
	{ 16, 0.30f },
	{ 32, 0.27f },
	{ 48, 0.25f },
};
static const int REDUNDANCY_CURVE_LEN =
	sizeof(REDUNDANCY_CURVE) / sizeof(REDUNDANCY_CURVE[0]);

static float interpolate_redundancy(int k)
{
	if (k <= REDUNDANCY_CURVE[0].k) return REDUNDANCY_CURVE[0].r;
	if (k >= REDUNDANCY_CURVE[REDUNDANCY_CURVE_LEN - 1].k)
		return REDUNDANCY_CURVE[REDUNDANCY_CURVE_LEN - 1].r;
	for (int i = 0; i < REDUNDANCY_CURVE_LEN - 1; i++) {
		int k0 = REDUNDANCY_CURVE[i].k;
		int k1 = REDUNDANCY_CURVE[i + 1].k;
		if (k >= k0 && k <= k1) {
			float t = (float)(k - k0) / (float)(k1 - k0);
			return REDUNDANCY_CURVE[i].r +
			       t * (REDUNDANCY_CURVE[i + 1].r - REDUNDANCY_CURVE[i].r);
		}
	}
	return REDUNDANCY_CURVE[REDUNDANCY_CURVE_LEN - 1].r;
}

/* ── Headroom ring (rolling max/avg of frame sizes) ──────────────────── */

#define RING_MAX 512

typedef struct {
	uint64_t ts_us[RING_MAX];
	uint32_t size[RING_MAX];
	int      head;
	int      count;
} HeadroomRing;

static void ring_push(HeadroomRing *r, uint64_t ts, uint32_t sz)
{
	r->ts_us[r->head] = ts;
	r->size[r->head] = sz;
	r->head = (r->head + 1) % RING_MAX;
	if (r->count < RING_MAX) r->count++;
}

static void ring_expire(HeadroomRing *r, uint64_t cutoff_us)
{
	/* Remove oldest entries with ts < cutoff. Logical deletion: we walk
	 * from oldest forward and shrink count. Simpler than a real deque; the
	 * ring is only traversed during compute_headroom() below so amortized
	 * cost is fine for ≤512 entries. */
	while (r->count > 0) {
		int oldest = (r->head - r->count + RING_MAX) % RING_MAX;
		if (r->ts_us[oldest] >= cutoff_us) break;
		r->count--;
	}
}

static float compute_headroom(const HeadroomRing *r,
                              float margin, float lo, float hi)
{
	if (r->count < 2) return lo;
	uint64_t sum = 0;
	uint32_t mx = 0;
	for (int i = 0; i < r->count; i++) {
		int idx = (r->head - 1 - i + RING_MAX) % RING_MAX;
		uint32_t v = r->size[idx];
		sum += v;
		if (v > mx) mx = v;
	}
	float avg = (float)sum / (float)r->count;
	if (avg <= 0.0f) return lo;
	float ratio = (float)mx / avg * margin;
	if (ratio < lo) ratio = lo;
	if (ratio > hi) ratio = hi;
	return ratio;
}

/* ── FEC controller core ─────────────────────────────────────────────── */

typedef struct {
	int   k, n;
	float redundancy;
	int   packets_per_frame;
	float avg_frame_size;
	float headroom;
} FecParams;

static FecParams fec_compute(float avg_size, float headroom, const Config *cfg)
{
	FecParams p = {0};
	float target = avg_size * headroom;
	int ppf = (int)ceilf(target / (float)cfg->mtu);
	if (ppf < 1) ppf = 1;

	int k = ppf;
	if (k < cfg->min_k) k = cfg->min_k;
	if (k > cfg->max_k) k = cfg->max_k;

	float r = interpolate_redundancy(k);
	if (r > 0.99f) r = 0.99f;
	int n = (int)ceilf((float)k / (1.0f - r));
	if (n < cfg->min_n) n = cfg->min_n;
	if (n > cfg->max_n) n = cfg->max_n;
	if (n <= k) n = k + 1;

	p.k = k;
	p.n = n;
	p.redundancy = r;
	p.packets_per_frame = ppf;
	p.avg_frame_size = avg_size;
	p.headroom = headroom;
	return p;
}

typedef struct {
	float     avg_frame_size;   /* EWMA, 0 = unset */
	FecParams current;
	bool      have_current;
	uint64_t  last_update_us;
	uint32_t  update_count;
	/* Post-MCS-drop parity boost: while now < boost_until_us, parity
	 * (n-k) is multiplied by cfg.boost_mult and the update is force-emitted
	 * past the usual hysteresis/cooldown gates. Zero = no active boost. */
	uint64_t  boost_until_us;
} Controller;

/* Returns true if caller should emit the new params. */
static bool controller_update(Controller *c, const Config *cfg,
                              uint32_t frame_size, HeadroomRing *ring,
                              uint64_t now, FecParams *out)
{
	/* Track frame size for headroom. */
	ring_push(ring, now, frame_size);
	uint64_t cutoff = now - (uint64_t)(cfg->headroom_window_s * 1e6);
	ring_expire(ring, cutoff);

	/* EWMA frame size */
	if (c->avg_frame_size <= 0.0f) {
		c->avg_frame_size = (float)frame_size;
	} else {
		c->avg_frame_size =
			cfg->ewma_alpha * (float)frame_size +
			(1.0f - cfg->ewma_alpha) * c->avg_frame_size;
	}

	float headroom = compute_headroom(
		ring, cfg->headroom_margin, cfg->headroom_min, cfg->headroom_max);

	FecParams cand = fec_compute(c->avg_frame_size, headroom, cfg);

	/* Post-MCS-drop boost: inflate parity so the committed n is higher.
	 * Detect the boost-expiry edge so we can force one emission to restore
	 * the natural parity instead of leaving n pinned until k next changes. */
	bool boost_active = (c->boost_until_us != 0 && now < c->boost_until_us);
	bool boost_expired_now = (c->boost_until_us != 0 && !boost_active);
	if (boost_expired_now) c->boost_until_us = 0;
	if (boost_active) {
		int parity = cand.n - cand.k;
		if (parity < 1) parity = 1;
		int boosted = cand.k + (int)ceilf((float)parity * cfg->boost_mult);
		if (boosted > cfg->max_n) boosted = cfg->max_n;
		if (boosted <= cand.k)    boosted = cand.k + 1;
		cand.n = boosted;
		/* Recompute observed redundancy from the new (k, n). */
		cand.redundancy = 1.0f - (float)cand.k / (float)cand.n;
	}

	if (!c->have_current) {
		c->current = cand;
		c->have_current = true;
		c->last_update_us = now;
		c->update_count++;
		*out = cand;
		return true;
	}

	/* Boost force-emits on every tick so parity tracks the current k.
	 * Same force-emit when boost just expired — restores natural parity. */
	if (boost_active || boost_expired_now) {
		if (cand.k == c->current.k && cand.n == c->current.n) return false;
		c->current = cand;
		c->last_update_us = now;
		c->update_count++;
		*out = cand;
		return true;
	}

	int k_delta = cand.k - c->current.k;
	float elapsed = (float)(now - c->last_update_us) / 1e6f;

	if (k_delta > 0) {
		if (k_delta < cfg->k_hyst_up) return false;
		if (elapsed < cfg->cooldown_up_s) return false;
	} else if (k_delta < 0) {
		if (-k_delta < cfg->k_hyst_down) return false;
		if (elapsed < cfg->cooldown_down_s) return false;
	} else {
		return false;
	}

	c->current = cand;
	c->last_update_us = now;
	c->update_count++;
	*out = cand;
	return true;
}

/* ── FPS estimator ───────────────────────────────────────────────────── */

typedef struct {
	uint64_t prev_ready_us;
	float    ewma_period_us;
	bool     have_prev;
} FpsEst;

static void fps_feed(FpsEst *f, uint64_t ready_us)
{
	if (!f->have_prev) {
		f->prev_ready_us = ready_us;
		f->have_prev = true;
		return;
	}
	uint64_t dt = ready_us - f->prev_ready_us;
	if (dt == 0 || dt > 1000000ULL) {
		/* Implausible gap — resync without updating EWMA. */
		f->prev_ready_us = ready_us;
		return;
	}
	if (f->ewma_period_us <= 0.0f) {
		f->ewma_period_us = (float)dt;
	} else {
		f->ewma_period_us = 0.1f * (float)dt + 0.9f * f->ewma_period_us;
	}
	f->prev_ready_us = ready_us;
}

static float fps_get(const FpsEst *f)
{
	if (f->ewma_period_us <= 0.0f) return 0.0f;
	return 1e6f / f->ewma_period_us;
}

/* ── UDP helpers ─────────────────────────────────────────────────────── */

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1) return -1;
	return 0;
}

static int sidecar_open_and_bind(void)
{
	/* Bind an ephemeral port for recv/send to the sidecar. */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in a = { .sin_family = AF_INET,
	                         .sin_addr.s_addr = htonl(INADDR_ANY),
	                         .sin_port = 0 };
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ── wfb_tx control (request/response over a single UDP socket) ──────── */

static uint32_t g_req_id = 1;

static int wfb_send_set_fec(const Config *cfg, int k, int n)
{
	struct sockaddr_in dst;
	if (resolve_ipv4(cfg->wfb_host, cfg->wfb_port, &dst) != 0) return -1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	CmdReq req = {0};
	req.req_id = htonl(g_req_id++);
	req.cmd_id = CMD_SET_FEC;
	req.u.set_fec.k = (uint8_t)k;
	req.u.set_fec.n = (uint8_t)n;

	/* wire: req_id(4) + cmd_id(1) + k(1) + n(1) = 7 bytes */
	ssize_t s = sendto(fd, &req, 7, 0,
	                   (const struct sockaddr*)&dst, sizeof(dst));
	close(fd);
	return (s == 7) ? 0 : -1;
}

/* CMD_SET_RADIO: change mcs/bw/gi/stbc/ldpc/vht_nss on the running wfb_tx.
 * Preserves stbc/ldpc/bw/vht_mode/vht_nss from the current radio state —
 * the scaler only moves mcs_index and (optionally) short_gi. */
static int wfb_send_set_radio(const Config *cfg, int mcs, int short_gi,
                              int bandwidth, int stbc, int ldpc,
                              int vht_mode, int vht_nss)
{
	struct sockaddr_in dst;
	if (resolve_ipv4(cfg->wfb_host, cfg->wfb_port, &dst) != 0) return -1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	CmdReq req = {0};
	req.req_id = htonl(g_req_id++);
	req.cmd_id = CMD_SET_RADIO;
	req.u.set_radio.stbc      = (uint8_t)stbc;
	req.u.set_radio.ldpc      = (uint8_t)ldpc;
	req.u.set_radio.short_gi  = (uint8_t)short_gi;
	req.u.set_radio.bandwidth = (uint8_t)bandwidth;
	req.u.set_radio.mcs_index = (uint8_t)mcs;
	req.u.set_radio.vht_mode  = (uint8_t)vht_mode;
	req.u.set_radio.vht_nss   = (uint8_t)vht_nss;

	/* wire: req_id(4) + cmd_id(1) + 7-byte set_radio = 12 bytes */
	ssize_t s = sendto(fd, &req, 12, 0,
	                   (const struct sockaddr*)&dst, sizeof(dst));
	close(fd);
	return (s == 12) ? 0 : -1;
}

static int wfb_get_radio(const Config *cfg, CmdResp *out)
{
	struct sockaddr_in dst;
	if (resolve_ipv4(cfg->wfb_host, cfg->wfb_port, &dst) != 0) return -1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	CmdReq req = {0};
	req.req_id = htonl(g_req_id++);
	req.cmd_id = CMD_GET_RADIO;

	/* offsetof(CmdReq, u) = 5 on packed layout (4 + 1) */
	if (sendto(fd, &req, 5, 0,
	           (const struct sockaddr*)&dst, sizeof(dst)) != 5) {
		close(fd);
		return -1;
	}

	/* Wait up to 300ms for reply. */
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int pr = poll(&p, 1, 300);
	if (pr <= 0) { close(fd); return -1; }

	ssize_t n = recv(fd, out, sizeof(*out), 0);
	close(fd);
	if (n < (ssize_t)(sizeof(uint32_t) * 2)) return -1;
	return 0;
}

/* ── MCS → PHY Mbps table (HT/VHT, NSS=1..N, MCS 0..9) ───────────────── */

/* HT MCS 0..7 PHY rate (Mbps), BW20 LGI. Higher BW/SGI/NSS scales below.
 * Source: 802.11n MCS table. */
static const float HT_20L[8] = {
	6.5f, 13.0f, 19.5f, 26.0f, 39.0f, 52.0f, 58.5f, 65.0f
};

/* VHT adds MCS 8 (78 Mbps @ BW20,NSS1,LGI) and 9 (ignored for BW20 NSS1;
 * valid at BW40+). We approximate by scaling from the HT table and adding
 * two slots. */
static const float VHT_20L_EXT[10] = {
	6.5f, 13.0f, 19.5f, 26.0f, 39.0f, 52.0f, 58.5f, 65.0f,
	78.0f, 86.7f   /* MCS9 officially N/A at BW20 NSS1 — kept for robustness */
};

static float phy_mbps(int mcs, int bw_mhz, int short_gi,
                      int vht_mode, int vht_nss)
{
	int idx_max = vht_mode ? 9 : 7;
	if (mcs < 0) mcs = 0;
	if (mcs > idx_max) mcs = idx_max;

	float base = vht_mode ? VHT_20L_EXT[mcs] : HT_20L[mcs];

	/* Bandwidth scaling relative to 20 MHz. */
	float bw_scale;
	switch (bw_mhz) {
	case 40:  bw_scale = 2.1f; break;   /* 40 MHz has ~2.08× data subcarriers */
	case 80:  bw_scale = 4.5f; break;
	case 160: bw_scale = 9.0f; break;
	default:  bw_scale = 1.0f; break;
	}

	/* SGI boost ≈ 10/9. */
	float gi_scale = short_gi ? (10.0f / 9.0f) : 1.0f;

	/* Spatial streams (VHT NSS, 1..4 typical). */
	int nss = (vht_mode && vht_nss > 0) ? vht_nss : 1;

	return base * bw_scale * gi_scale * (float)nss;
}

/* ── RSSI source (tail wfb_rx text output) ───────────────────────────── */

/*
 * wfb_rx emits per-second lines like:
 *   34001586    RX_ANT    5745:5:20    1    1082:-30:-28:-28:24:31:34
 *   ts_ms       tag       freq:mcs:bw  ant  pkts:rmin:ravg:rmax:smin:savg:smax
 *
 * Two transports:
 *   - STREAM mode: tail a file/FIFO/stdin. Line-oriented read().
 *   - UDP mode:    bind a UDP port, recv() datagrams. Each datagram contains
 *                  one or more '\n'-terminated RX_ANT lines forwarded by the
 *                  ground client (see poc/ground_rssi_forwarder.py).
 *
 * Both transports converge on the same rssi_parse_line(): take max RSSI-avg
 * across antennas in a 250 ms window, feed an EWMA.
 *
 * Stream open modes:
 *   - Regular file:   opened with O_NONBLOCK, start at end (tail -f style)
 *   - FIFO:           opened with O_NONBLOCK|O_RDONLY, no writer yet is OK
 *   - "-" (stdin):    fd 0, set non-blocking
 */

typedef enum {
	RSSI_SRC_NONE = 0,
	RSSI_SRC_STREAM,
	RSSI_SRC_UDP,
} RssiSrcKind;

typedef struct {
	int         fd;              /* -1 = disabled */
	RssiSrcKind kind;
	char        buf[4096];       /* STREAM only: line accumulator */
	size_t      buf_len;

	/* Per-window aggregate (rolled over on timeout). */
	uint64_t window_start_us;
	int      window_max_rssi;
	int      window_have_rssi;

	float    ewma_rssi;          /* dBm; 0 = unset */
	uint64_t last_update_us;
	bool     have_ewma;

	/* Stats (for troubleshooting / heartbeat). */
	uint64_t pkts_total;         /* RX_ANT lines successfully parsed */
	uint64_t bytes_total;        /* total bytes recv'd (UDP) or read (stream) */
	uint64_t first_pkt_us;       /* 0 until we see the first valid line */
	char     first_peer[48];     /* UDP only: "ip:port" of first peer */
} RssiSource;

static int rssi_open(RssiSource *s, const char *path)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->kind = RSSI_SRC_NONE;
	s->ewma_rssi = 0.0f;
	if (!path || !*path) return 0;   /* disabled */

	int fd;
	if (strcmp(path, "-") == 0) {
		fd = 0;
		int fl = fcntl(fd, F_GETFL, 0);
		if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	} else {
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) return -1;
		/* Seek to end for regular files (skip historical noise). */
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
			lseek(fd, 0, SEEK_END);
	}
	s->fd = fd;
	s->kind = RSSI_SRC_STREAM;
	return 0;
}

/* Open a UDP listener on INADDR_ANY:port. Each received datagram is parsed
 * as one or more '\n'-terminated RX_ANT text lines. */
static int rssi_open_udp(RssiSource *s, uint16_t port)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->kind = RSSI_SRC_NONE;
	s->ewma_rssi = 0.0f;

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET,
	                         .sin_addr.s_addr = htonl(INADDR_ANY),
	                         .sin_port = htons(port) };
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	s->fd = fd;
	s->kind = RSSI_SRC_UDP;
	return 0;
}

static void rssi_close(RssiSource *s)
{
	if (s->fd > 0) close(s->fd);
	s->fd = -1;
	s->kind = RSSI_SRC_NONE;
}

/* Parse one line. Returns 0 if it was an RX_ANT line we used, -1 otherwise. */
static int rssi_parse_line(RssiSource *s, const char *line, uint64_t now,
                           float alpha)
{
	/* Skip leading whitespace / timestamp, find "RX_ANT". */
	const char *tag = strstr(line, "RX_ANT");
	if (!tag) return -1;

	/* Skip past "RX_ANT" and surrounding whitespace, then skip two
	 * whitespace-delimited tokens (freq:mcs:bw, antenna index). */
	const char *p = tag + 6;
	for (int skip = 0; skip < 2; skip++) {
		while (*p == ' ' || *p == '\t') p++;
		while (*p && *p != ' ' && *p != '\t') p++;
	}
	while (*p == ' ' || *p == '\t') p++;
	if (!*p) return -1;

	/* Now at: "pkts:rmin:ravg:rmax:smin:savg:smax". We want ravg (field 2). */
	long fields[7];
	int  n_fields = 0;
	const char *q = p;
	for (int i = 0; i < 7 && *q; i++) {
		char *end;
		fields[i] = strtol(q, &end, 10);
		if (end == q) break;
		n_fields++;
		if (*end == ':') q = end + 1;
		else break;
	}
	if (n_fields < 3) return -1;

	int rssi_avg = (int)fields[2];   /* negative dBm */

	/* Stats */
	s->pkts_total++;
	if (s->first_pkt_us == 0) s->first_pkt_us = now;

	/* Aggregate over a 250 ms window: keep best (least negative). */
	if (!s->window_have_rssi || s->window_start_us == 0) {
		s->window_start_us = now;
		s->window_max_rssi = rssi_avg;
		s->window_have_rssi = 1;
	} else {
		if (rssi_avg > s->window_max_rssi)
			s->window_max_rssi = rssi_avg;
	}

	/* Window closed? Commit to EWMA. */
	if ((now - s->window_start_us) >= 250000ULL) {
		float v = (float)s->window_max_rssi;
		if (!s->have_ewma) {
			s->ewma_rssi = v;
			s->have_ewma = true;
		} else {
			s->ewma_rssi = alpha * v + (1.0f - alpha) * s->ewma_rssi;
		}
		s->last_update_us = now;
		s->window_start_us = now;
		s->window_max_rssi = rssi_avg;
	}
	return 0;
}

/* Split a NUL-terminated buffer on '\n' and feed each line to the parser.
 * Modifies the buffer (writes NULs at line breaks). */
static void rssi_feed_block(RssiSource *s, char *start, size_t len,
                            uint64_t now, float alpha)
{
	char *end = start + len;
	char *p = start;
	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl) {
			/* Consume the trailing non-newlined chunk as a complete line for
			 * UDP mode (callers guarantee terminated packets); STREAM mode's
			 * buffering handles partial lines separately. */
			if (*p) {
				char save = *end;
				*end = '\0';
				(void)rssi_parse_line(s, p, now, alpha);
				*end = save;
			}
			return;
		}
		*nl = '\0';
		(void)rssi_parse_line(s, p, now, alpha);
		p = nl + 1;
	}
}

/* Drain any pending bytes, line-split, parse RX_ANT entries. */
static void rssi_poll(RssiSource *s, uint64_t now, float alpha)
{
	if (s->fd < 0) return;

	if (s->kind == RSSI_SRC_UDP) {
		/* Datagram mode: each recv is one or more complete lines. */
		char pkt[2048];
		struct sockaddr_in peer;
		socklen_t pl;
		for (;;) {
			pl = sizeof(peer);
			ssize_t n = recvfrom(s->fd, pkt, sizeof(pkt) - 1, 0,
			                     (struct sockaddr *)&peer, &pl);
			if (n <= 0) break;
			s->bytes_total += (uint64_t)n;
			pkt[n] = '\0';
			uint64_t pkts_before = s->pkts_total;
			rssi_feed_block(s, pkt, (size_t)n, now, alpha);
			if (s->pkts_total > pkts_before && s->first_peer[0] == '\0') {
				char ip[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
				snprintf(s->first_peer, sizeof(s->first_peer),
				         "%s:%u", ip, (unsigned)ntohs(peer.sin_port));
				fprintf(stderr,
				        "[fec t=%8.3f] rssi: first packet from %s (%zd bytes)\n",
				        log_rel_s(), s->first_peer, n);
			}
		}
		return;
	}

	/* STREAM mode: line-buffered accumulator. */
	for (;;) {
		/* Paranoid bounds: cap writes to at most buf[0..sizeof-2] so the
		 * trailing NUL fits. Re-normalize buf_len on each iteration so the
		 * fortify analyzer can see the invariant. */
		if (s->buf_len > sizeof(s->buf) - 1)
			s->buf_len = 0;
		if (s->buf_len == sizeof(s->buf) - 1)
			s->buf_len = 0;   /* full; drop and resync on next newline */
		size_t want = (sizeof(s->buf) - 1) - s->buf_len;
		if (want > sizeof(s->buf))
			break;   /* unreachable; silences fortify */
		ssize_t n = read(s->fd, &s->buf[s->buf_len], want);
		if (n <= 0) break;
		s->bytes_total += (uint64_t)n;
		s->buf_len += (size_t)n;
		s->buf[s->buf_len] = '\0';

		/* Split on '\n' and parse each complete line. */
		char *start = s->buf;
		uint64_t pkts_before = s->pkts_total;
		for (;;) {
			char *nl = memchr(start, '\n', s->buf_len - (size_t)(start - s->buf));
			if (!nl) break;
			*nl = '\0';
			(void)rssi_parse_line(s, start, now, alpha);
			start = nl + 1;
		}
		if (s->pkts_total > pkts_before && s->first_peer[0] == '\0') {
			snprintf(s->first_peer, sizeof(s->first_peer), "(stream)");
			fprintf(stderr,
			        "[fec t=%8.3f] rssi: first RX_ANT line parsed from stream\n",
			        log_rel_s());
		}
		/* Shift the residual partial line to the front. */
		size_t used = (size_t)(start - s->buf);
		if (used > 0 && used < s->buf_len) {
			memmove(s->buf, start, s->buf_len - used);
			s->buf_len -= used;
		} else if (used == s->buf_len) {
			s->buf_len = 0;
		}
	}
}

static bool rssi_is_stale(const RssiSource *s, uint64_t now, float silence_s)
{
	if (!s->have_ewma) return true;
	uint64_t age = now - s->last_update_us;
	return age > (uint64_t)(silence_s * 1e6f);
}

/* ── MCS ladder + scaler policy ──────────────────────────────────────── */

typedef struct {
	int   mcs;
	float climb_dbm;   /* rssi_ewma must reach this (or above) to climb TO mcs */
	float drop_dbm;    /* rssi_ewma below this to drop FROM mcs */
} McsRung;

/* HT20, LGI, single-stream. Conservative FPV thresholds.
 * climb/drop hysteresis band is ~4 dB.
 *
 * drop_dbm for mcs=0 is unreachable (−∞) — we never drop below the floor. */
static const McsRung MCS_LADDER[8] = {
	{ 0, -200.0f, -200.0f },
	{ 1,  -82.0f,  -85.0f },
	{ 2,  -78.0f,  -82.0f },
	{ 3,  -74.0f,  -78.0f },
	{ 4,  -70.0f,  -74.0f },
	{ 5,  -66.0f,  -70.0f },
	{ 6,  -62.0f,  -66.0f },
	{ 7,  -58.0f,  -62.0f },
};

typedef struct {
	int      mcs;                /* current (last-known) MCS */
	int      pending;            /* pending target (only valid if have_pending) */
	bool     have_pending;
	uint64_t pending_since_us;   /* when this pending target was first seen */
	uint64_t last_change_us;     /* cooldown anchor */
} MCSScaler;

/* Returns the new MCS if a move is committed, or the current one otherwise.
 * Sets *moved=true iff the caller should emit CMD_SET_RADIO. */
static int mcs_scaler_tick(MCSScaler *m, const Config *cfg,
                           float rssi, uint64_t now, bool *moved)
{
	*moved = false;
	int cur = m->mcs;
	int lo  = cfg->mcs_min;
	int hi  = cfg->mcs_max;
	if (cur < lo) cur = lo;
	if (cur > hi) cur = hi;

	/* Propose a direction. */
	int target = cur;
	if (cur < hi && rssi >= MCS_LADDER[cur + 1].climb_dbm)
		target = cur + 1;
	else if (cur > lo && rssi < MCS_LADDER[cur].drop_dbm)
		target = cur - 1;

	if (target == cur) {
		/* Reset pending — candidate stopped matching. */
		m->have_pending = false;
		return cur;
	}

	/* Same pending target as before? Measure time-in-state. Otherwise reset. */
	if (!m->have_pending || m->pending != target) {
		m->have_pending = true;
		m->pending = target;
		m->pending_since_us = now;
		return cur;
	}

	float needed_s = (target > cur) ? cfg->mcs_climb_s : cfg->mcs_drop_s;
	float held_s = (float)(now - m->pending_since_us) / 1e6f;
	if (held_s < needed_s) return cur;

	/* Cooldown gate. */
	float since_change = (float)(now - m->last_change_us) / 1e6f;
	if (since_change < cfg->mcs_cooldown_s) return cur;

	/* Commit. */
	m->mcs = target;
	m->last_change_us = now;
	m->have_pending = false;
	*moved = true;
	return target;
}

/* ── venc HTTP client (tiny, blocking) ───────────────────────────────── */

/* Does a single HTTP/1.0 GET and copies the body into out[0..out_sz-1].
 * Returns number of body bytes, or -1 on error. */
static int http_get(const char *host, uint16_t port, const char *path,
                    char *out, size_t out_sz, int timeout_ms)
{
	struct sockaddr_in dst;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { close(fd); return -1; }

	struct timeval tv = { .tv_sec = timeout_ms / 1000,
	                      .tv_usec = (timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (connect(fd, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
		close(fd); return -1;
	}

	char req[512];
	int rl = snprintf(req, sizeof(req),
	                  "GET %s HTTP/1.0\r\n"
	                  "Host: %s:%u\r\n"
	                  "User-Agent: fec_controller/0.1\r\n"
	                  "Connection: close\r\n\r\n",
	                  path, host, (unsigned)port);
	if (rl <= 0 || rl >= (int)sizeof(req)) { close(fd); return -1; }
	if (send(fd, req, (size_t)rl, 0) != rl) { close(fd); return -1; }

	char buf[2048];
	size_t total = 0;
	for (;;) {
		if (total >= sizeof(buf) - 1) break;
		ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
		if (n <= 0) break;
		total += (size_t)n;
	}
	close(fd);
	if (total == 0) return -1;
	buf[total] = '\0';

	/* Find end of headers. */
	char *body = strstr(buf, "\r\n\r\n");
	if (!body) return -1;
	body += 4;
	size_t body_len = total - (size_t)(body - buf);
	if (body_len >= out_sz) body_len = out_sz - 1;
	memcpy(out, body, body_len);
	out[body_len] = '\0';
	return (int)body_len;
}

/* Minimal JSON scraper: find "\"field\":value" and parse integer. */
static int json_find_int(const char *s, const char *key, long *out)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(s, pat);
	if (!p) return -1;
	p += strlen(pat);
	while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
	char *end;
	long v = strtol(p, &end, 10);
	if (end == p) return -1;
	*out = v;
	return 0;
}

/* Returns current video0.bitrate (kbps) or -1.
 *
 * NOTE: venc /api/v1/get takes the field name as the KEY of the first
 * query parameter, not "field=NAME". So the URL is literally
 *   /api/v1/get?video0.bitrate
 * not
 *   /api/v1/get?field=video0.bitrate
 */
static long venc_get_bitrate_kbps(const Config *cfg)
{
	char body[2048];
	int n = http_get(cfg->venc_host, cfg->venc_port,
	                 "/api/v1/get?video0.bitrate",
	                 body, sizeof(body), 500);
	if (n <= 0) return -1;
	long v;
	if (json_find_int(body, "value", &v) != 0) return -1;
	return v;
}

static int venc_set_bitrate_kbps(const Config *cfg, long kbps)
{
	if (kbps < 1) kbps = 1;
	if (kbps > 200000) kbps = 200000;
	char path[96];
	snprintf(path, sizeof(path),
	         "/api/v1/set?video0.bitrate=%ld", kbps);
	char body[1024];
	int n = http_get(cfg->venc_host, cfg->venc_port,
	                 path, body, sizeof(body), 500);
	if (n <= 0) return -1;
	/* Accept any 200-ish response with an "ok" marker. */
	return (strstr(body, "\"ok\":true") || strstr(body, "\"ok\": true")) ? 0 : -1;
}

/* ── CLI / usage ─────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Endpoints:\n"
		"  --sidecar HOST:PORT  venc sidecar (default 127.0.0.1:6666)\n"
		"  --wfb HOST:PORT      wfb_tx control (default 127.0.0.1:8000)\n"
		"  --venc HOST:PORT     venc HTTP API (default 127.0.0.1:80)\n"
		"\n"
		"FEC sizing:\n"
		"  --mtu N              packet size budget (default 1446)\n"
		"  --min-k N            min k (default 1)\n"
		"  --max-k N            max k (default 48)\n"
		"\n"
		"Link budget:\n"
		"  --safety F           fraction of PHY rate usable (default 0.6)\n"
		"\n"
		"MCS scaler (opt-in):\n"
		"  --mcs-enable         enable RSSI-driven MCS scaler (default off)\n"
		"  --mcs-min N          floor MCS (default 1)\n"
		"  --mcs-max N          ceiling MCS (default 3)\n"
		"  --rssi-stream PATH   wfb_rx log/FIFO to tail (\"-\" = stdin)\n"
		"  --rssi-udp PORT      bind UDP listener for RX_ANT text lines\n"
		"                       (from poc/ground_rssi_forwarder.py over a\n"
		"                       wfb_rx uplink)\n"
		"  --rssi-silence F     stale-RSSI threshold, seconds (default 1.5)\n"
		"  --mcs-climb F        dwell seconds required to climb (default 2.0)\n"
		"  --mcs-drop F         dwell seconds required to drop (default 0.3)\n"
		"  --mcs-cooldown F     seconds between any MCS change (default 3.0)\n"
		"\n"
		"Post-MCS-drop FEC boost:\n"
		"  --boost-s F          parity boost duration after MCS drop (default 3.0)\n"
		"  --boost-mult F       (n-k) parity multiplier during boost (default 1.3)\n"
		"\n"
		"Behavior:\n"
		"  --dry-run            compute but do not call set_fec/set bitrate/set radio\n"
		"  -v, --verbose        extra logs\n"
		"  -h, --help           this message\n",
		prog);
}

static int parse_hostport(const char *s, char *host, size_t host_sz,
                          uint16_t *port)
{
	const char *colon = strrchr(s, ':');
	if (!colon) return -1;
	size_t n = (size_t)(colon - s);
	if (n >= host_sz) return -1;
	memcpy(host, s, n);
	host[n] = '\0';
	int p = atoi(colon + 1);
	if (p <= 0 || p > 65535) return -1;
	*port = (uint16_t)p;
	return 0;
}

static void config_defaults(Config *c)
{
	strcpy(c->sidecar_host, "127.0.0.1"); c->sidecar_port = 6666;
	strcpy(c->wfb_host,     "127.0.0.1"); c->wfb_port     = 8000;
	strcpy(c->venc_host,    "127.0.0.1"); c->venc_port    = 80;

	c->mtu = 1446;
	c->min_k = 1;    c->max_k = 48;
	c->min_n = 2;    c->max_n = 72;
	c->ewma_alpha = 0.05f;
	c->headroom_min = 1.05f;
	c->headroom_max = 1.40f;
	c->headroom_margin = 1.05f;
	c->headroom_window_s = 2.5f;

	c->k_hyst_up = 1;
	c->k_hyst_down = 3;
	c->cooldown_up_s = 0.1f;
	c->cooldown_down_s = 2.0f;

	c->safety_margin = 0.6f;

	c->subscribe_s = 2.0f;
	c->radio_poll_s = 1.0f;
	c->bitrate_poll_s = 1.0f;

	c->mcs_enable = false;
	c->mcs_min = 1;
	c->mcs_max = 3;
	c->rssi_stream[0] = '\0';
	c->rssi_udp_port = 0;
	c->rssi_silence_s = 1.5f;
	c->rssi_ewma_alpha = 0.3f;
	c->mcs_climb_s = 2.0f;
	c->mcs_drop_s  = 0.3f;
	c->mcs_cooldown_s = 3.0f;

	c->boost_s = 3.0f;
	c->boost_mult = 1.3f;

	c->dry_run = false;
	c->verbose = 0;
}

/* ── Main loop ───────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	Config cfg;
	config_defaults(&cfg);

	enum {
		OPT_SIDECAR = 256, OPT_WFB, OPT_VENC, OPT_MTU, OPT_MINK, OPT_MAXK,
		OPT_SAFETY, OPT_DRY_RUN,
		OPT_MCS_ENABLE, OPT_MCS_MIN, OPT_MCS_MAX,
		OPT_RSSI_STREAM, OPT_RSSI_UDP, OPT_RSSI_SILENCE,
		OPT_MCS_CLIMB, OPT_MCS_DROP, OPT_MCS_COOLDOWN,
		OPT_BOOST_S, OPT_BOOST_MULT,
	};
	static const struct option longopts[] = {
		{"sidecar",       required_argument, 0, OPT_SIDECAR},
		{"wfb",           required_argument, 0, OPT_WFB},
		{"venc",          required_argument, 0, OPT_VENC},
		{"mtu",           required_argument, 0, OPT_MTU},
		{"min-k",         required_argument, 0, OPT_MINK},
		{"max-k",         required_argument, 0, OPT_MAXK},
		{"safety",        required_argument, 0, OPT_SAFETY},
		{"dry-run",       no_argument,       0, OPT_DRY_RUN},
		{"mcs-enable",    no_argument,       0, OPT_MCS_ENABLE},
		{"mcs-min",       required_argument, 0, OPT_MCS_MIN},
		{"mcs-max",       required_argument, 0, OPT_MCS_MAX},
		{"rssi-stream",   required_argument, 0, OPT_RSSI_STREAM},
		{"rssi-udp",      required_argument, 0, OPT_RSSI_UDP},
		{"rssi-silence",  required_argument, 0, OPT_RSSI_SILENCE},
		{"mcs-climb",     required_argument, 0, OPT_MCS_CLIMB},
		{"mcs-drop",      required_argument, 0, OPT_MCS_DROP},
		{"mcs-cooldown",  required_argument, 0, OPT_MCS_COOLDOWN},
		{"boost-s",       required_argument, 0, OPT_BOOST_S},
		{"boost-mult",    required_argument, 0, OPT_BOOST_MULT},
		{"verbose",       no_argument,       0, 'v'},
		{"help",          no_argument,       0, 'h'},
		{0, 0, 0, 0},
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
		switch (ch) {
		case OPT_SIDECAR:
			if (parse_hostport(optarg, cfg.sidecar_host,
			                   sizeof(cfg.sidecar_host), &cfg.sidecar_port) != 0) {
				fprintf(stderr, "invalid --sidecar\n"); return 1;
			}
			break;
		case OPT_WFB:
			if (parse_hostport(optarg, cfg.wfb_host,
			                   sizeof(cfg.wfb_host), &cfg.wfb_port) != 0) {
				fprintf(stderr, "invalid --wfb\n"); return 1;
			}
			break;
		case OPT_VENC:
			if (parse_hostport(optarg, cfg.venc_host,
			                   sizeof(cfg.venc_host), &cfg.venc_port) != 0) {
				fprintf(stderr, "invalid --venc\n"); return 1;
			}
			break;
		case OPT_MTU:          cfg.mtu            = atoi(optarg); break;
		case OPT_MINK:         cfg.min_k          = atoi(optarg); break;
		case OPT_MAXK:         cfg.max_k          = atoi(optarg); break;
		case OPT_SAFETY:       cfg.safety_margin  = (float)atof(optarg); break;
		case OPT_DRY_RUN:      cfg.dry_run        = true; break;
		case OPT_MCS_ENABLE:   cfg.mcs_enable     = true; break;
		case OPT_MCS_MIN:      cfg.mcs_min        = atoi(optarg); break;
		case OPT_MCS_MAX:      cfg.mcs_max        = atoi(optarg); break;
		case OPT_RSSI_STREAM:
			strncpy(cfg.rssi_stream, optarg, sizeof(cfg.rssi_stream) - 1);
			cfg.rssi_stream[sizeof(cfg.rssi_stream) - 1] = '\0';
			break;
		case OPT_RSSI_UDP: {
			int p = atoi(optarg);
			if (p <= 0 || p > 65535) {
				fprintf(stderr, "invalid --rssi-udp (1..65535)\n");
				return 1;
			}
			cfg.rssi_udp_port = (uint16_t)p;
			break;
		}
		case OPT_RSSI_SILENCE: cfg.rssi_silence_s = (float)atof(optarg); break;
		case OPT_MCS_CLIMB:    cfg.mcs_climb_s    = (float)atof(optarg); break;
		case OPT_MCS_DROP:     cfg.mcs_drop_s     = (float)atof(optarg); break;
		case OPT_MCS_COOLDOWN: cfg.mcs_cooldown_s = (float)atof(optarg); break;
		case OPT_BOOST_S:      cfg.boost_s        = (float)atof(optarg); break;
		case OPT_BOOST_MULT:   cfg.boost_mult     = (float)atof(optarg); break;
		case 'v':              cfg.verbose        = 1; break;
		case 'h': default: usage(argv[0]); return (ch == 'h') ? 0 : 1;
		}
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	log_init();
	LOG("sidecar=%s:%u wfb=%s:%u venc=%s:%u mtu=%d safety=%.2f dry_run=%d",
	    cfg.sidecar_host, cfg.sidecar_port,
	    cfg.wfb_host,     cfg.wfb_port,
	    cfg.venc_host,    cfg.venc_port,
	    cfg.mtu, cfg.safety_margin, cfg.dry_run ? 1 : 0);

	int sfd = sidecar_open_and_bind();
	if (sfd < 0) { perror("sidecar socket"); return 1; }

	struct sockaddr_in sidecar_dst;
	if (resolve_ipv4(cfg.sidecar_host, cfg.sidecar_port, &sidecar_dst) != 0) {
		fprintf(stderr, "bad sidecar address\n"); return 1;
	}

	Controller ctrl = {0};
	HeadroomRing ring = {0};
	FpsEst fps = {0};

	uint64_t next_subscribe_us = 0;
	uint64_t next_radio_us     = 0;
	uint64_t next_bitrate_us   = 0;
	uint64_t next_heartbeat_us = 0;

	/* Last known radio state (for reporting + link budget). */
	struct {
		bool valid;
		int  short_gi, bandwidth, mcs, vht_mode, vht_nss, stbc, ldpc;
		float phy_mbps;
	} radio = {0};

	/* MCS scaler + RSSI source (enabled via --mcs-enable + --rssi-stream). */
	MCSScaler scaler = {0};
	RssiSource rssi_src;
	memset(&rssi_src, 0, sizeof(rssi_src));
	rssi_src.fd = -1;

	/* Open the RSSI source if requested. Source and scaler are independent:
	 * a source can be open (heartbeat shows rssi + rx stats) without the
	 * scaler acting on it. The scaler itself is gated behind --mcs-enable. */
	bool src_requested = (cfg.rssi_stream[0] != '\0') || (cfg.rssi_udp_port != 0);
	bool src_both_set  = (cfg.rssi_stream[0] != '\0') && (cfg.rssi_udp_port != 0);

	if (src_both_set) {
		LOG("rssi: --rssi-stream and --rssi-udp are mutually exclusive; ignoring both");
		cfg.rssi_stream[0] = '\0';
		cfg.rssi_udp_port = 0;
		src_requested = false;
	}

	if (src_requested && cfg.rssi_udp_port != 0) {
		if (rssi_open_udp(&rssi_src, cfg.rssi_udp_port) != 0) {
			LOG("rssi: UDP bind failed on :%u (%s); source disabled",
			    cfg.rssi_udp_port, strerror(errno));
		} else {
			LOG("rssi: listening on udp:%u (awaiting first RX_ANT packet)",
			    cfg.rssi_udp_port);
		}
	} else if (src_requested) {
		if (rssi_open(&rssi_src, cfg.rssi_stream) != 0) {
			LOG("rssi: stream open failed: %s (%s); source disabled",
			    cfg.rssi_stream, strerror(errno));
		} else {
			LOG("rssi: tailing %s", cfg.rssi_stream);
		}
	}

	if (cfg.mcs_enable) {
		if (!src_requested) {
			LOG("mcs: --mcs-enable requires --rssi-stream or --rssi-udp; scaler disabled");
			cfg.mcs_enable = false;
		} else if (rssi_src.fd < 0) {
			LOG("mcs: RSSI source failed to open; scaler disabled");
			cfg.mcs_enable = false;
		} else {
			LOG("mcs: scaler enabled, ladder=[%d..%d]",
			    cfg.mcs_min, cfg.mcs_max);
		}
	} else if (src_requested && rssi_src.fd >= 0) {
		LOG("rssi: source active but --mcs-enable not set; scaler will NOT adjust MCS");
	}

	while (!g_stop) {
		uint64_t now = now_us();

		/* --- Subscribe keepalive --- */
		if (now >= next_subscribe_us) {
			SidecarSubscribe s = {0};
			s.magic = htonl(SC_MAGIC);
			s.version = SC_VERSION;
			s.msg_type = SC_MSG_SUBSCRIBE;
			sendto(sfd, &s, sizeof(s), 0,
			       (struct sockaddr*)&sidecar_dst, sizeof(sidecar_dst));
			LOGV(&cfg, "sent SUBSCRIBE");
			next_subscribe_us = now + (uint64_t)(cfg.subscribe_s * 1e6);
		}

		/* --- Drain RSSI stream (non-blocking) --- */
		if (rssi_src.fd >= 0)
			rssi_poll(&rssi_src, now, cfg.rssi_ewma_alpha);

		/* --- 5 s heartbeat (always on) --- */
		if (now >= next_heartbeat_us) {
			if (ctrl.have_current && radio.valid) {
				int k = ctrl.current.k, n = ctrl.current.n;
				float fps_hz = fps_get(&fps);
				bool boost = (ctrl.boost_until_us != 0 && now < ctrl.boost_until_us);

				/* RSSI chunk: show whenever a source is configured, not just
				 * when the scaler is enabled — so the user can see whether
				 * feedback is arriving at all. */
				char rssi_buf[96];
				if (rssi_src.fd >= 0) {
					static uint64_t hb_prev_pkts = 0;
					uint64_t delta = rssi_src.pkts_total - hb_prev_pkts;
					hb_prev_pkts = rssi_src.pkts_total;
					char mcs_part[16] = "";
					if (cfg.mcs_enable)
						snprintf(mcs_part, sizeof(mcs_part), " mcs=%d", scaler.mcs);
					if (rssi_src.have_ewma) {
						snprintf(rssi_buf, sizeof(rssi_buf),
						         " rssi=%.1f%s rx=%llu(+%llu/5s)",
						         rssi_src.ewma_rssi, mcs_part,
						         (unsigned long long)rssi_src.pkts_total,
						         (unsigned long long)delta);
					} else {
						snprintf(rssi_buf, sizeof(rssi_buf),
						         " rssi=(none)%s rx=%llu(+%llu/5s)",
						         mcs_part,
						         (unsigned long long)rssi_src.pkts_total,
						         (unsigned long long)delta);
					}
				} else {
					rssi_buf[0] = '\0';
				}
				LOG("hb: k=%d n=%d avg=%.1fkB fps=%.1f phy=%.1fMbps upd=%u%s%s",
				    k, n,
				    ctrl.avg_frame_size / 1024.0f, fps_hz, radio.phy_mbps,
				    ctrl.update_count, rssi_buf,
				    boost ? " BOOST" : "");
			}
			next_heartbeat_us = now + 5000000ULL;
		}

		/* --- Poll radio params --- */
		if (now >= next_radio_us) {
			CmdResp resp;
			if (wfb_get_radio(&cfg, &resp) == 0) {
				radio.valid     = true;
				radio.stbc      = resp.u.get_radio.stbc;
				radio.ldpc      = resp.u.get_radio.ldpc;
				radio.short_gi  = resp.u.get_radio.short_gi;
				radio.bandwidth = resp.u.get_radio.bandwidth;
				radio.mcs       = resp.u.get_radio.mcs_index;
				radio.vht_mode  = resp.u.get_radio.vht_mode;
				radio.vht_nss   = resp.u.get_radio.vht_nss;
				radio.phy_mbps  = phy_mbps(radio.mcs, radio.bandwidth,
				                           radio.short_gi, radio.vht_mode,
				                           radio.vht_nss);
				/* Seed scaler with current on-air MCS on first sync. */
				if (cfg.mcs_enable && scaler.last_change_us == 0) {
					scaler.mcs = radio.mcs;
					scaler.last_change_us = now;
				}
				LOGV(&cfg, "radio: mcs=%d bw=%d gi=%s vht=%d nss=%d phy=%.1f Mbps",
				     radio.mcs, radio.bandwidth,
				     radio.short_gi ? "short" : "long",
				     radio.vht_mode, radio.vht_nss, radio.phy_mbps);
			} else {
				LOGV(&cfg, "radio: get_radio timed out");
			}

			/* --- MCS scaler tick (piggybacks on radio poll cadence) --- */
			if (cfg.mcs_enable && radio.valid) {
				if (rssi_is_stale(&rssi_src, now, cfg.rssi_silence_s)) {
					LOGV(&cfg, "mcs: rssi stale (>%.1fs), freezing at mcs=%d",
					     cfg.rssi_silence_s, scaler.mcs);
				} else {
					bool moved = false;
					int old_mcs = scaler.mcs;
					int new_mcs = mcs_scaler_tick(&scaler, &cfg,
					                               rssi_src.ewma_rssi, now, &moved);
					if (moved) {
						bool is_drop = (new_mcs < old_mcs);
						LOG("mcs: %d -> %d (rssi=%.1f dBm) %s%s",
						    old_mcs, new_mcs, rssi_src.ewma_rssi,
						    is_drop ? "DROP" : "CLIMB",
						    cfg.dry_run ? " [DRY-RUN]" : "");
						if (!cfg.dry_run) {
							if (wfb_send_set_radio(&cfg, new_mcs,
							                       radio.short_gi, radio.bandwidth,
							                       radio.stbc, radio.ldpc,
							                       radio.vht_mode, radio.vht_nss) != 0)
								LOG("mcs: set_radio send failed");
						}
						if (is_drop && cfg.boost_s > 0.0f) {
							ctrl.boost_until_us = now + (uint64_t)(cfg.boost_s * 1e6f);
							LOG("fec: parity boost armed for %.1fs (mult=%.2f)",
							    cfg.boost_s, cfg.boost_mult);
						}
					}
				}
			}

			next_radio_us = now + (uint64_t)(cfg.radio_poll_s * 1e6);
		}

		/* --- Bitrate budget check --- */
		if (now >= next_bitrate_us && radio.valid && ctrl.have_current) {
			/* Post-FEC goodput = phy_mbps * k/n. Safe video budget =
			 * goodput * safety_margin. Both sides in kbps. */
			float k = (float)ctrl.current.k;
			float n = (float)ctrl.current.n;
			float post_fec_kbps = radio.phy_mbps * 1000.0f * (k / n);
			long safe_kbps = (long)(post_fec_kbps * cfg.safety_margin);

			long cur = venc_get_bitrate_kbps(&cfg);
			if (cur <= 0) {
				LOGV(&cfg, "bitrate: venc HTTP query failed (is /api/v1/get reachable?)");
			} else {
				LOGV(&cfg, "bitrate: cur=%ld safe=%ld (phy=%.1f k/n=%d/%d)",
				     cur, safe_kbps, radio.phy_mbps,
				     ctrl.current.k, ctrl.current.n);
				if (cur > safe_kbps) {
					LOG("clamp bitrate %ld -> %ld kbps (phy=%.1f Mbps, k/n=%d/%d, safety=%.2f)",
					    cur, safe_kbps, radio.phy_mbps,
					    ctrl.current.k, ctrl.current.n, cfg.safety_margin);
					if (!cfg.dry_run) {
						if (venc_set_bitrate_kbps(&cfg, safe_kbps) != 0)
							LOG("bitrate set failed");
					}
				}
			}
			next_bitrate_us = now + (uint64_t)(cfg.bitrate_poll_s * 1e6);
		}

		/* --- Poll sidecar for one FRAME (or wait up to 100 ms) --- */
		struct pollfd pfd = { .fd = sfd, .events = POLLIN };
		int pr = poll(&pfd, 1, 100);
		if (pr < 0) { if (errno == EINTR) continue; perror("poll"); break; }
		if (pr == 0) continue;

		uint8_t buf[128];
		struct sockaddr_in src; socklen_t sl = sizeof(src);
		ssize_t nrd = recvfrom(sfd, buf, sizeof(buf), 0,
		                       (struct sockaddr*)&src, &sl);
		if (nrd < (ssize_t)sizeof(SidecarFrame)) continue;

		SidecarFrame *f = (SidecarFrame*)buf;
		if (ntohl(f->magic) != SC_MAGIC) continue;
		if (f->version != SC_VERSION) continue;
		if (f->msg_type != SC_MSG_FRAME) continue;

		uint64_t ready = be64_read(&f->frame_ready_us);
		fps_feed(&fps, ready);

		/* We need frame_size_bytes from the ENC_INFO trailer. Without it
		 * this POC can't drive k — venc must be built with enc-info
		 * telemetry (sidecar sets RTP_SIDECAR_FLAG_ENC_INFO). */
		if (!(f->flags & SC_FLAG_ENC_INFO)) continue;
		if (nrd < (ssize_t)(sizeof(SidecarFrame) + sizeof(SidecarEncInfo))) continue;

		SidecarEncInfo *e = (SidecarEncInfo*)(buf + sizeof(SidecarFrame));
		uint32_t frame_size = ntohl(e->frame_size_bytes);
		if (frame_size == 0) continue;

		FecParams next_params;
		bool emit = controller_update(&ctrl, &cfg, frame_size, &ring,
		                              now_us(), &next_params);
		if (emit) {
			LOG("FEC %s: k=%d n=%d (avg=%.0fB hd=%.2f ppf=%d red=%.2f fps=%.1f)",
			    ctrl.update_count == 1 ? "init" : "update",
			    next_params.k, next_params.n,
			    next_params.avg_frame_size, next_params.headroom,
			    next_params.packets_per_frame, next_params.redundancy,
			    fps_get(&fps));
			if (!cfg.dry_run) {
				if (wfb_send_set_fec(&cfg, next_params.k, next_params.n) != 0)
					LOG("set_fec send failed");
			}
		}
	}

	close(sfd);
	rssi_close(&rssi_src);
	LOG("stopped after %u FEC updates", ctrl.update_count);
	return 0;
}
