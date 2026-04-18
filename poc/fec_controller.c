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
#define CMD_GET_RADIO  4

#pragma pack(push, 1)
typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	union {
		struct { uint8_t k, n; }                    set_fec;
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

	bool     dry_run;
	int      verbose;
} Config;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

#define LOGV(cfg, fmt, ...) \
	do { if ((cfg)->verbose) fprintf(stderr, "[fec] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOG(fmt, ...) fprintf(stderr, "[fec] " fmt "\n", ##__VA_ARGS__)

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

	if (!c->have_current) {
		c->current = cand;
		c->have_current = true;
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
		"Behavior:\n"
		"  --dry-run            compute but do not call set_fec/set bitrate\n"
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
	};
	static const struct option longopts[] = {
		{"sidecar", required_argument, 0, OPT_SIDECAR},
		{"wfb",     required_argument, 0, OPT_WFB},
		{"venc",    required_argument, 0, OPT_VENC},
		{"mtu",     required_argument, 0, OPT_MTU},
		{"min-k",   required_argument, 0, OPT_MINK},
		{"max-k",   required_argument, 0, OPT_MAXK},
		{"safety",  required_argument, 0, OPT_SAFETY},
		{"dry-run", no_argument,       0, OPT_DRY_RUN},
		{"verbose", no_argument,       0, 'v'},
		{"help",    no_argument,       0, 'h'},
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
		case OPT_MTU:     cfg.mtu     = atoi(optarg); break;
		case OPT_MINK:    cfg.min_k   = atoi(optarg); break;
		case OPT_MAXK:    cfg.max_k   = atoi(optarg); break;
		case OPT_SAFETY:  cfg.safety_margin = (float)atof(optarg); break;
		case OPT_DRY_RUN: cfg.dry_run = true; break;
		case 'v':         cfg.verbose = 1; break;
		case 'h': default: usage(argv[0]); return (ch == 'h') ? 0 : 1;
		}
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

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

	/* Last known radio state (for reporting + link budget). */
	struct {
		bool valid;
		int  short_gi, bandwidth, mcs, vht_mode, vht_nss;
		float phy_mbps;
	} radio = {0};

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

		/* --- Poll radio params --- */
		if (now >= next_radio_us) {
			CmdResp resp;
			if (wfb_get_radio(&cfg, &resp) == 0) {
				radio.valid     = true;
				radio.short_gi  = resp.u.get_radio.short_gi;
				radio.bandwidth = resp.u.get_radio.bandwidth;
				radio.mcs       = resp.u.get_radio.mcs_index;
				radio.vht_mode  = resp.u.get_radio.vht_mode;
				radio.vht_nss   = resp.u.get_radio.vht_nss;
				radio.phy_mbps  = phy_mbps(radio.mcs, radio.bandwidth,
				                           radio.short_gi, radio.vht_mode,
				                           radio.vht_nss);
				LOGV(&cfg, "radio: mcs=%d bw=%d gi=%s vht=%d nss=%d phy=%.1f Mbps",
				     radio.mcs, radio.bandwidth,
				     radio.short_gi ? "short" : "long",
				     radio.vht_mode, radio.vht_nss, radio.phy_mbps);
			} else {
				LOGV(&cfg, "radio: get_radio timed out");
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
	LOG("stopped after %u FEC updates", ctrl.update_count);
	return 0;
}
