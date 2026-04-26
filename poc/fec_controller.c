/*
 * fec_controller — Simple C POC for adaptive wfb-ng FEC sizing.
 *
 * Subscribes to waybeam_venc sidecar (per-frame metadata), computes k/n
 * sized for the average frame (EWMA + bounded headroom), and sends
 * CMD_SET_FEC to wfb_tx.
 *
 * Polls wfb_tx radio params (mcs/bw/gi) on a 1 Hz cadence to compute the
 * safe link budget (safety * phy_mbps * k/n) and asserts video0.bitrate
 * via /api/v1/set?video0.bitrate=N when the budget changes.
 *
 * The controller is purely reactive: it does not touch MCS itself. If MCS
 * or venc.bitrate change externally, it detects the change on the next
 * radio poll, arms a settle window so the EWMA can re-converge, and
 * resumes normal sizing. An MCS drop additionally arms a brief parity
 * boost while the EWMA catches up to the new (larger) frame-size
 * distribution.
 *
 * FEC sizing is asymmetric: k can climb after a short cooldown
 * (cfg.cooldown_up_s) and shrinks only after the candidate has held a
 * lower value for cfg.k_down_dwell_s — anti-bounce on a content tick.
 *
 * Single thread, poll() loop. No libs beyond libc.
 *
 * Target: SigmaStar Infinity6E (armv7l / OpenIPC). Cross-build with the
 * star6e toolchain. Runs on-device; both endpoints are 127.0.0.1.
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
	float    ppf_deadband_frac;     /* slack as a fraction of MTU at the
	                                 * (current_ppf±1)*MTU boundaries.
	                                 * Suppresses ppf flips driven by EWMA
	                                 * jitter near a bucket edge — only a
	                                 * sustained shift past the deadband
	                                 * actually moves k. */

	/* Gating */
	int      k_hyst_up;
	float    cooldown_up_s;
	float    k_down_dwell_s;         /* candidate must hold below current
	                                  * k for this many seconds before a
	                                  * down-commit is allowed; pure anti-
	                                  * bounce. Reset whenever the
	                                  * candidate changes target or rises
	                                  * back to current. Designed to be
	                                  * longer than mcs_settle_s so an MCS
	                                  * edge-trigger always wins the race. */
	float    startup_grace_s;        /* suppress emits for the first N sec */

	/* Link budget */
	float    safety_margin;  /* fraction of phy_mbps usable after overhead */
	long     bitrate_min_kbps;       /* floor (default 1000) */
	long     bitrate_max_kbps;       /* ceiling; 0 = unlimited (default) */
	float    bitrate_tolerance;      /* fraction (default 0.15 = 15%) */
	float    bitrate_grace_s;        /* suppress FEC emits for N s after
	                                  * a bitrate write; short window to
	                                  * absorb tiny venc transients */
	float    mcs_settle_s;           /* suppress FEC emits for N s after
	                                  * a detected (external) MCS change;
	                                  * longer than bitrate_grace_s
	                                  * because an MCS change roughly
	                                  * doubles bitrate and the EWMA
	                                  * takes 4-5 s to fully track the
	                                  * new frame-size distribution */

	/* Tick intervals */
	float    subscribe_s;
	float    radio_poll_s;           /* radio + bitrate-assert cadence
	                                  * (used only when wfb_stats_port == 0;
	                                  * stats-subscribe mode is event-driven
	                                  * by incoming datagrams). */

	/* wfb_tx stats subscriber (-Y on the wfb_tx side). When set, bind a
	 * UDP listener on 127.0.0.1:wfb_stats_port and drive radio.* + the
	 * bitrate assertion from incoming JSON datagrams instead of polling
	 * CMD_GET_RADIO. Lower latency on radio changes (per-datagram instead
	 * of up-to-1s) and decoupled from wfb_tx process lifecycle: if wfb_tx
	 * is restarted, datagrams just resume; no reconnection needed.
	 *
	 * 0 = disabled (fall back to CMD_GET_RADIO polling). */
	uint16_t wfb_stats_port;

	/* Post-MCS-drop FEC-parity boost (armed on detected external MCS-down) */
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

/* current_ppf == 0 means "no current params yet" (skip deadband). */
static FecParams fec_compute(float avg_size, float headroom, const Config *cfg,
                             int current_ppf)
{
	FecParams p = {0};
	float target = avg_size * headroom;
	int ppf = (int)ceilf(target / (float)cfg->mtu);
	if (ppf < 1) ppf = 1;

	/* Boundary deadband: once we have a steady current_ppf, only step out
	 * of the bucket when target crosses the next bucket's edge plus slack.
	 * This kills the ppf ping-pong driven by ~5% EWMA + ~10% headroom
	 * jitter that compound to ~15% target swings near a quantization edge.
	 * Slack is symmetric so up- and down-noise are filtered identically. */
	if (current_ppf > 0 && cfg->ppf_deadband_frac > 0.0f) {
		float slack = (float)cfg->mtu * cfg->ppf_deadband_frac;
		float up_edge   = (float)(current_ppf + 1) * (float)cfg->mtu + slack;
		float down_edge = (float)(current_ppf - 1) * (float)cfg->mtu - slack;
		if (ppf > current_ppf && target < up_edge)        ppf = current_ppf;
		else if (ppf < current_ppf && target > down_edge) ppf = current_ppf;
	}

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
	uint64_t  start_us;         /* first call into controller_update */
	uint32_t  update_count;
	/* Post-MCS-drop parity boost: while now < boost_until_us, parity
	 * (n-k) is multiplied by cfg.boost_mult. Force-emit is edge-triggered
	 * — once on boost entry (to inflate n), once on expiry (to restore) —
	 * not on every tick; otherwise tiny EWMA wobbles in k flood CMD_SET_FEC
	 * at per-frame rates. Zero = no active boost. */
	uint64_t  boost_until_us;
	bool      was_boosting;
	/* Post-change quiet window: after we write video0.bitrate or detect
	 * an external MCS change we give venc a moment to converge before
	 * reacting to its new output. Armed for bitrate_grace_s on bitrate
	 * writes and for mcs_settle_s on detected MCS changes (extended-only,
	 * never shortened). Edge-triggered commit fires exactly once when
	 * the window expires. */
	uint64_t  bitrate_grace_until_us;
	bool      was_in_grace;
	/* k-down anti-bounce: a candidate proposing cand.k < current.k must
	 * hold the same target for cfg.k_down_dwell_s before commit. Any
	 * change in candidate target (lower or higher) restarts the timer;
	 * any commit clears it. */
	uint64_t  k_down_pending_since_us;
	int       k_down_pending_target;
} Controller;

/* Arm the "post-change" settling window, extending only (never shortening)
 * any in-flight window. We do NOT reset the EWMA — its old value is always
 * closer to the new steady state than zero would be, and zeroing it out
 * just causes it to re-seed from whichever transitional frame happens to
 * land first, which empirically extends convergence rather than shortening
 * it. Letting it drift over the settle window works better. */
static void controller_arm_settle(Controller *c, uint64_t now, float secs)
{
	uint64_t new_end = now + (uint64_t)(secs * 1e6f);
	if (new_end > c->bitrate_grace_until_us)
		c->bitrate_grace_until_us = new_end;
}

/* Commit a candidate as the new current params. All emit paths in
 * controller_update() funnel through here. Returns true (caller uses the
 * return value to forward to the outer caller). Any commit clears the
 * pending k-down dwell — a fresh down candidate has to re-arm. */
static bool controller_commit(Controller *c, const FecParams *cand,
                              uint64_t now, FecParams *out)
{
	c->current = *cand;
	c->last_update_us = now;
	c->update_count++;
	c->k_down_pending_since_us = 0;
	*out = *cand;
	return true;
}

/* Arm the post-MCS-drop parity boost; caller still logs its own reason. */
static void controller_arm_boost(Controller *c, const Config *cfg, uint64_t now)
{
	if (cfg->boost_s > 0.0f)
		c->boost_until_us = now + (uint64_t)(cfg->boost_s * 1e6f);
}

/* Returns true if caller should emit the new params. */
static bool controller_update(Controller *c, const Config *cfg,
                              uint32_t frame_size, HeadroomRing *ring,
                              uint64_t now, FecParams *out)
{
	/* Track first tick for startup grace window. */
	if (c->start_us == 0) c->start_us = now;

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

	int cur_ppf = c->have_current ? c->current.packets_per_frame : 0;
	FecParams cand = fec_compute(c->avg_frame_size, headroom, cfg, cur_ppf);

	/* Post-MCS-drop boost: inflate parity so the committed n is higher.
	 * Force-emit is EDGE-triggered (entry / exit) — not per-tick — so that
	 * tiny EWMA wobbles in k don't flood CMD_SET_FEC during the window. */
	bool boost_active = (c->boost_until_us != 0 && now < c->boost_until_us);
	bool boost_expired_now = (c->boost_until_us != 0 && !boost_active);
	if (boost_expired_now) c->boost_until_us = 0;
	bool boost_entry = (boost_active && !c->was_boosting);
	c->was_boosting = boost_active;

	if (boost_active) {
		int parity = cand.n - cand.k;
		if (parity < 1) parity = 1;
		int boosted = cand.k + (int)ceilf((float)parity * cfg->boost_mult);
		if (boosted > cfg->max_n) boosted = cfg->max_n;
		if (boosted <= cand.k)    boosted = cand.k + 1;
		cand.n = boosted;
		cand.redundancy = 1.0f - (float)cand.k / (float)cand.n;
	}

	/* Grace windows during which we suppress non-edge FEC emits. The
	 * bitrate_grace_until_us timestamp is the earliest time we may emit
	 * again; it's set (extended only, never shortened) by both our
	 * bitrate writes (short window) and detected external MCS changes
	 * (long window, since the EWMA takes longer to track the big
	 * frame-size jump caused by a phy-rate change).
	 *
	 * Detect the settle-just-ended edge: at that moment we force one
	 * commit (can go up or down) so the post-change k reflects the new
	 * steady state immediately, bypassing the k-down dwell. */
	bool in_startup_grace = (now - c->start_us) <
	                        (uint64_t)(cfg->startup_grace_s * 1e6f);
	bool in_settle_grace  = (c->bitrate_grace_until_us != 0 &&
	                         now < c->bitrate_grace_until_us);
	bool in_grace = in_startup_grace || in_settle_grace;
	bool settle_just_ended = c->was_in_grace && !in_settle_grace;
	c->was_in_grace = in_settle_grace;

	/* First commit: held off through startup grace, then emitted when the
	 * EWMA has had a couple of seconds to settle. */
	if (!c->have_current) {
		if (in_grace) return false;
		c->have_current = true;
		return controller_commit(c, &cand, now, out);
	}

	/* Boost edges (entry / expiry): commit exactly once in either case,
	 * so inflated parity lands on entry and natural parity restores on
	 * expiry. Idempotent on (k,n) — no-op if nothing actually changed. */
	if (boost_entry || boost_expired_now) {
		if (cand.k == c->current.k && cand.n == c->current.n) return false;
		return controller_commit(c, &cand, now, out);
	}

	/* In any settling window (startup or post-MCS/bitrate), suppress
	 * every non-edge emit so the EWMA can converge without us hammering
	 * wfb_tx. */
	if (in_grace) return false;

	/* Settle-just-ended edge: fire one commit (either direction allowed)
	 * so the post-change k reflects the new steady state. */
	if (settle_just_ended) {
		if (cand.k == c->current.k && cand.n == c->current.n) return false;
		return controller_commit(c, &cand, now, out);
	}

	int k_delta = cand.k - c->current.k;

	/* k-down: candidate must hold the same lower target for k_down_dwell_s
	 * before we commit. Any change in target (down further OR back up)
	 * restarts the dwell, which biases toward stable k and prevents
	 * thrashing on EWMA jitter. Mirror semantics of the (now-removed)
	 * MCS scaler dwell. */
	if (k_delta < 0) {
		if (c->k_down_pending_since_us == 0 ||
		    c->k_down_pending_target != cand.k) {
			c->k_down_pending_since_us = now;
			c->k_down_pending_target   = cand.k;
			return false;
		}
		float held_s = (float)(now - c->k_down_pending_since_us) / 1e6f;
		if (held_s < cfg->k_down_dwell_s) return false;
		return controller_commit(c, &cand, now, out);
	}

	/* k unchanged or up: cancel any pending k-down. */
	c->k_down_pending_since_us = 0;

	if (k_delta == 0) return false;
	if (k_delta < cfg->k_hyst_up) return false;
	float elapsed = (float)(now - c->last_update_us) / 1e6f;
	if (elapsed < cfg->cooldown_up_s) return false;

	return controller_commit(c, &cand, now, out);
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

/* Bind a UDP listener on INADDR_ANY:port for wfb_tx -Y stats datagrams.
 * Non-blocking so the main loop's poll() can drain it without stalling. */
static int wfb_stats_open_listener(uint16_t port)
{
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
	return fd;
}

/* Minimal "find an integer field by name" JSON scraper. Targeted at the
 * well-known wfb_tx tx_stats schema where every key we care about is a
 * unique string in the document — we don't need a real parser, and a
 * 30-line strstr+strtol is robust enough for known producers.
 *
 * Looks for "key" followed by optional whitespace, ':', whitespace, then
 * an integer (possibly negative). Returns 0 on success, -1 if the key
 * isn't found or the value isn't a parseable integer. */
static int json_get_int(const char *s, const char *key, long *out)
{
	char pat[64];
	int pl = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (pl <= 0 || pl >= (int)sizeof(pat)) return -1;
	const char *p = strstr(s, pat);
	if (!p) return -1;
	p += pl;
	while (*p == ' ' || *p == '\t' || *p == ':') p++;
	char *end;
	long v = strtol(p, &end, 10);
	if (end == p) return -1;
	*out = v;
	return 0;
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

typedef struct {
	bool  valid;
	int   short_gi, bandwidth, mcs, vht_mode, vht_nss, stbc, ldpc;
	float phy_mbps;
} RadioState;

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

/* Apply a fresh radio observation: update radio.*, detect external
 * changes vs the previous snapshot, log them, arm the settle window
 * (mcs_settle_s) on any change, arm parity boost on MCS-down.
 *
 * Shared by both the legacy CMD_GET_RADIO polling path and the
 * --wfb-stats-port subscriber path. The stats subscriber additionally
 * passes obs_fec_k/n; the polling path passes -1 (CMD_GET_RADIO doesn't
 * carry FEC sizes). When obs_fec_k differs from last_written_fec_k it's
 * an external set_fec — currently logged but not acted on (the next
 * controller_update() will re-assert our value if needed). */
static void radio_apply_observation(RadioState *radio, Controller *ctrl,
                                    const Config *cfg,
                                    int new_mcs, int new_bw, int new_gi,
                                    int new_stbc, int new_ldpc,
                                    int new_vht_mode, int new_vht_nss,
                                    int obs_fec_k, int obs_fec_n,
                                    int *last_written_fec_k,
                                    int *last_written_fec_n,
                                    uint64_t now)
{
	bool was_valid = radio->valid;
	int  prev_mcs  = radio->mcs;
	int  prev_bw   = radio->bandwidth;
	int  prev_gi   = radio->short_gi;
	int  prev_vht  = radio->vht_mode;
	int  prev_nss  = radio->vht_nss;

	radio->valid     = true;
	radio->stbc      = new_stbc;
	radio->ldpc      = new_ldpc;
	radio->short_gi  = new_gi;
	radio->bandwidth = new_bw;
	radio->mcs       = new_mcs;
	radio->vht_mode  = new_vht_mode;
	radio->vht_nss   = new_vht_nss;
	radio->phy_mbps  = phy_mbps(radio->mcs, radio->bandwidth,
	                            radio->short_gi, radio->vht_mode,
	                            radio->vht_nss);

	if (was_valid) {
		bool mcs_changed = (radio->mcs != prev_mcs);
		bool any_changed = mcs_changed
		    || radio->bandwidth != prev_bw
		    || radio->short_gi  != prev_gi
		    || radio->vht_mode  != prev_vht
		    || radio->vht_nss   != prev_nss;
		if (any_changed) {
			LOG("radio: external change mcs %d->%d bw %d->%d gi %d->%d vht %d->%d nss %d->%d (phy=%.1fMbps)",
			    prev_mcs, radio->mcs,
			    prev_bw, radio->bandwidth,
			    prev_gi, radio->short_gi,
			    prev_vht, radio->vht_mode,
			    prev_nss, radio->vht_nss,
			    radio->phy_mbps);
			controller_arm_settle(ctrl, now, cfg->mcs_settle_s);
			if (mcs_changed && radio->mcs < prev_mcs) {
				controller_arm_boost(ctrl, cfg, now);
				if (cfg->boost_s > 0.0f)
					LOG("fec: parity boost armed for %.1fs (mult=%.2f) [external MCS drop]",
					    cfg->boost_s, cfg->boost_mult);
			}
		}
	}

	/* External set_fec detection (stats-subscribe path only). When
	 * polling, obs_fec_k == -1 and we skip this. We only log a CHANGE,
	 * which means there must be a previously-written reference to
	 * compare against; the very first observation just seeds
	 * last_written silently. */
	if (obs_fec_k > 0 && obs_fec_n > 0) {
		if (*last_written_fec_k > 0 && *last_written_fec_n > 0 &&
		    (obs_fec_k != *last_written_fec_k ||
		     obs_fec_n != *last_written_fec_n))
		{
			LOG("fec: external change observed k=%d n=%d (controller last wrote k=%d n=%d)",
			    obs_fec_k, obs_fec_n,
			    *last_written_fec_k, *last_written_fec_n);
		}
		*last_written_fec_k = obs_fec_k;
		*last_written_fec_n = obs_fec_n;
	}
}

/* Forward decl — defined later in the venc HTTP client section. */
static int venc_set_bitrate_kbps(const Config *cfg, long kbps);

/* Compute safe_kbps from current radio + ctrl, clamp to
 * [bitrate_min_kbps, bitrate_max_kbps], and assert via venc HTTP if it
 * differs from last_written_kbps by more than --bitrate-tol. Arms the
 * post-write grace window on a successful (or dry-run) write. Shared by
 * both paths. */
static void bitrate_assert(const RadioState *radio, Controller *ctrl,
                           const Config *cfg, long *last_written_kbps,
                           uint64_t now)
{
	if (!radio->valid || !ctrl->have_current) return;

	float k = (float)ctrl->current.k;
	float n = (float)ctrl->current.n;
	float post_fec_kbps = radio->phy_mbps * 1000.0f * (k / n);
	long safe_kbps = (long)(post_fec_kbps * cfg->safety_margin);

	long target = safe_kbps;
	if (cfg->bitrate_max_kbps > 0 && target > cfg->bitrate_max_kbps)
		target = cfg->bitrate_max_kbps;
	if (target < cfg->bitrate_min_kbps)
		target = cfg->bitrate_min_kbps;

	long ref = (*last_written_kbps > 0) ? *last_written_kbps : 0;
	long dev = ref - target;
	long adev = dev < 0 ? -dev : dev;
	long tol = (long)((double)target * (double)cfg->bitrate_tolerance);
	if (tol < 1) tol = 1;

	LOGV(cfg, "bitrate: target=%ld last_written=%ld safe=%ld min=%ld max=%ld (phy=%.1f k/n=%d/%d)",
	     target, *last_written_kbps, safe_kbps,
	     cfg->bitrate_min_kbps, cfg->bitrate_max_kbps,
	     radio->phy_mbps, ctrl->current.k, ctrl->current.n);

	if (*last_written_kbps < 0 || adev > tol) {
		const char *dir = (*last_written_kbps < 0)
		    ? "init"
		    : ((ref > target) ? "down" : "up");
		LOG("bitrate %s %ld -> %ld kbps (phy=%.1f Mbps, k/n=%d/%d, safe=%ld)",
		    dir, *last_written_kbps, target, radio->phy_mbps,
		    ctrl->current.k, ctrl->current.n, safe_kbps);
		if (!cfg->dry_run) {
			if (venc_set_bitrate_kbps(cfg, target) != 0)
				LOG("bitrate: set failed");
			else
				*last_written_kbps = target;
		} else {
			*last_written_kbps = target;
		}
		if (cfg->bitrate_grace_s > 0.0f) {
			uint64_t new_end =
			    now + (uint64_t)(cfg->bitrate_grace_s * 1e6f);
			if (new_end > ctrl->bitrate_grace_until_us)
				ctrl->bitrate_grace_until_us = new_end;
		}
	}
}

/* Parse one wfb_tx tx_stats JSON datagram and forward into
 * radio_apply_observation() + bitrate_assert(). Returns 0 if the
 * datagram looked sane and was consumed, -1 otherwise.
 *
 * Tracks the producer's monotonic "seq" counter (additive in v1) via
 * *last_seq (in/out, -1 = unset on first call). seq going up by more
 * than 1 means the loopback dropped one or more datagrams; seq going
 * down means wfb_tx restarted (its seq counter starts at 0 each
 * process). Both are logged but not acted on — purely operator
 * visibility. */
static int wfb_stats_handle_datagram(const char *body,
                                     RadioState *radio, Controller *ctrl,
                                     const Config *cfg,
                                     int *last_written_fec_k,
                                     int *last_written_fec_n,
                                     long *last_written_kbps,
                                     long *last_seq,
                                     uint64_t now)
{
	/* Sanity check: must look like a tx_stats v1 record. */
	if (!strstr(body, "\"tx_stats\"")) return -1;

	long mcs, bw, gi, stbc, ldpc, vht_mode, vht_nss;
	if (json_get_int(body, "mcs",      &mcs)      != 0) return -1;
	if (json_get_int(body, "bw",       &bw)       != 0) return -1;
	if (json_get_int(body, "short_gi", &gi)       != 0) return -1;
	if (json_get_int(body, "stbc",     &stbc)     != 0) return -1;
	if (json_get_int(body, "ldpc",     &ldpc)     != 0) return -1;
	if (json_get_int(body, "vht_mode", &vht_mode) != 0) return -1;
	if (json_get_int(body, "vht_nss",  &vht_nss)  != 0) return -1;

	long fec_k = -1, fec_n = -1;
	(void)json_get_int(body, "fec_k", &fec_k);
	(void)json_get_int(body, "fec_n", &fec_n);

	radio_apply_observation(radio, ctrl, cfg,
	                        (int)mcs, (int)bw, (int)gi,
	                        (int)stbc, (int)ldpc,
	                        (int)vht_mode, (int)vht_nss,
	                        (int)fec_k, (int)fec_n,
	                        last_written_fec_k, last_written_fec_n,
	                        now);
	bitrate_assert(radio, ctrl, cfg, last_written_kbps, now);

	/* Surface congestion signals from wfb_tx. The counters are
	 * per-interval (wfb_tx resets them after each emit), so any
	 * non-zero value means the tx side hit rxq overflow / injection
	 * timeout / a truncated packet within the last interval. We log
	 * but don't act on these — the eventual congestion-aware throttle
	 * will live on top of this signal. */
	long drops = 0, trunc = 0;
	(void)json_get_int(body, "pkts_drop", &drops);
	(void)json_get_int(body, "pkts_trunc", &trunc);
	if (drops > 0 || trunc > 0) {
		LOG("wfb-stats: tx dropped %ld pkts (trunc=%ld) last interval",
		    drops, trunc);
	}

	/* Sequence-gap detection. The "seq" field is an additive v1
	 * extension; older wfb_tx without it will simply not match the
	 * lookup and we silently skip — backward compatible. */
	long seq = -1;
	if (json_get_int(body, "seq", &seq) == 0 && seq >= 0) {
		if (*last_seq >= 0) {
			if (seq < *last_seq) {
				LOG("wfb-stats: seq went backwards (%ld -> %ld) — wfb_tx likely restarted",
				    *last_seq, seq);
			} else if (seq > *last_seq + 1) {
				LOG("wfb-stats: seq gap %ld -> %ld (%ld datagram(s) lost on the wire)",
				    *last_seq, seq, seq - *last_seq - 1);
			}
		}
		*last_seq = seq;
	}
	return 0;
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
		"  --wfb-stats-port N   bind UDP listener on 127.0.0.1:N for wfb_tx -Y\n"
		"                       JSON stats. When set, radio.* + fec_k/fec_n\n"
		"                       come from incoming datagrams and CMD_GET_RADIO\n"
		"                       polling is disabled (default 0 = polling mode).\n"
		"\n"
		"FEC sizing:\n"
		"  --mtu N              packet size budget (default 1446)\n"
		"  --min-k N            min k (default 1)\n"
		"  --max-k N            max k (default 48)\n"
		"  --ppf-deadband F     ±F*MTU slack at the (current_ppf±1)*MTU\n"
		"                       quantization edges (default 0.15; 0 disables).\n"
		"                       Suppresses k flips driven by EWMA jitter near\n"
		"                       a bucket boundary; a sustained shift past the\n"
		"                       deadband still moves k normally.\n"
		"  --k-hyst-up N        min Δk to trigger an up-move (default 2)\n"
		"  --cooldown-up F      min seconds between up-moves (default 1.0)\n"
		"  --k-down-dwell F     candidate must hold below current k for F s\n"
		"                       before a k-down commit (default 8.0; pure\n"
		"                       anti-bounce, longer than --mcs-settle-s so\n"
		"                       an MCS edge-trigger always wins the race)\n"
		"  --startup-grace F    suppress emits for first F seconds (default 2.0)\n"
		"\n"
		"Link budget:\n"
		"  --safety F           fraction of PHY rate usable (default 0.5)\n"
		"                       target = clamp(phy*k/n*safety, min, max)\n"
		"  --bitrate-min N      bitrate floor in kbps (default 1000)\n"
		"  --bitrate-max N      bitrate ceiling in kbps (default 0 = unlimited)\n"
		"  --bitrate-tol F      bitrate re-apply tolerance (default 0.15 = 15%%)\n"
		"  --bitrate-grace F    suppress FEC emits for F s after a bitrate write\n"
		"                       (default 2.0; absorbs small venc transients)\n"
		"  --mcs-settle-s F     suppress FEC emits for F s after a detected\n"
		"                       (external) MCS change (default 5.0; EWMA needs\n"
		"                       longer to track the big frame-size jump caused\n"
		"                       by a phy rate change)\n"
		"\n"
		"Post-MCS-drop FEC boost (armed on detected external MCS-down):\n"
		"  --boost-s F          parity boost duration after MCS drop (default 3.0)\n"
		"  --boost-mult F       (n-k) parity multiplier during boost (default 1.3)\n"
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
	c->ppf_deadband_frac = 0.15f;    /* ±15% of MTU slack at ppf bucket
	                                  * edges; suppresses noise-driven k
	                                  * flips while still tracking real
	                                  * sustained shifts. 0 disables. */

	c->k_hyst_up = 2;                /* require Δk≥2 to emit an up-move */
	c->cooldown_up_s = 1.0f;         /* at most one up-move per second */
	c->k_down_dwell_s = 8.0f;        /* k-down candidate must hold for 8 s */
	c->startup_grace_s = 2.0f;

	c->safety_margin = 0.5f;         /* leave 50% airtime for uplink + headroom */
	c->bitrate_min_kbps = 1000;
	c->bitrate_max_kbps = 0;         /* 0 = unlimited */
	c->bitrate_tolerance = 0.15f;
	c->bitrate_grace_s = 2.0f;
	c->mcs_settle_s    = 5.0f;

	c->subscribe_s = 2.0f;
	c->radio_poll_s = 1.0f;          /* radio + bitrate-assert cadence */
	c->wfb_stats_port = 0;           /* 0 = polling mode (legacy) */

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
		OPT_PPF_DEADBAND,
		OPT_K_HYST_UP, OPT_COOLDOWN_UP, OPT_K_DOWN_DWELL, OPT_STARTUP_GRACE,
		OPT_SAFETY, OPT_BITRATE_MIN, OPT_BITRATE_MAX,
		OPT_BITRATE_DESIRED, /* deprecated alias for --bitrate-max */
		OPT_BITRATE_TOL, OPT_BITRATE_GRACE, OPT_MCS_SETTLE, OPT_DRY_RUN,
		OPT_BOOST_S, OPT_BOOST_MULT,
		OPT_WFB_STATS_PORT,
	};
	static const struct option longopts[] = {
		{"sidecar",       required_argument, 0, OPT_SIDECAR},
		{"wfb",           required_argument, 0, OPT_WFB},
		{"venc",          required_argument, 0, OPT_VENC},
		{"mtu",           required_argument, 0, OPT_MTU},
		{"min-k",         required_argument, 0, OPT_MINK},
		{"max-k",         required_argument, 0, OPT_MAXK},
		{"ppf-deadband",  required_argument, 0, OPT_PPF_DEADBAND},
		{"k-hyst-up",     required_argument, 0, OPT_K_HYST_UP},
		{"cooldown-up",   required_argument, 0, OPT_COOLDOWN_UP},
		{"k-down-dwell",  required_argument, 0, OPT_K_DOWN_DWELL},
		{"startup-grace", required_argument, 0, OPT_STARTUP_GRACE},
		{"safety",        required_argument, 0, OPT_SAFETY},
		{"bitrate-min",   required_argument, 0, OPT_BITRATE_MIN},
		{"bitrate-max",   required_argument, 0, OPT_BITRATE_MAX},
		{"bitrate-desired", required_argument, 0, OPT_BITRATE_DESIRED},
		{"bitrate-tol",   required_argument, 0, OPT_BITRATE_TOL},
		{"bitrate-grace", required_argument, 0, OPT_BITRATE_GRACE},
		{"mcs-settle-s",  required_argument, 0, OPT_MCS_SETTLE},
		{"dry-run",       no_argument,       0, OPT_DRY_RUN},
		{"boost-s",       required_argument, 0, OPT_BOOST_S},
		{"boost-mult",    required_argument, 0, OPT_BOOST_MULT},
		{"wfb-stats-port", required_argument, 0, OPT_WFB_STATS_PORT},
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
		case OPT_MTU:           cfg.mtu             = atoi(optarg); break;
		case OPT_MINK:          cfg.min_k           = atoi(optarg); break;
		case OPT_MAXK:          cfg.max_k           = atoi(optarg); break;
		case OPT_PPF_DEADBAND:  cfg.ppf_deadband_frac = (float)atof(optarg); break;
		case OPT_K_HYST_UP:     cfg.k_hyst_up       = atoi(optarg); break;
		case OPT_COOLDOWN_UP:   cfg.cooldown_up_s   = (float)atof(optarg); break;
		case OPT_K_DOWN_DWELL:  cfg.k_down_dwell_s  = (float)atof(optarg); break;
		case OPT_STARTUP_GRACE: cfg.startup_grace_s = (float)atof(optarg); break;
		case OPT_SAFETY:        cfg.safety_margin   = (float)atof(optarg); break;
		case OPT_BITRATE_MIN:   cfg.bitrate_min_kbps = atol(optarg); break;
		case OPT_BITRATE_MAX:   cfg.bitrate_max_kbps = atol(optarg); break;
		case OPT_BITRATE_DESIRED:
			/* Deprecated alias — kept so older invocations still work. */
			cfg.bitrate_max_kbps = atol(optarg);
			fprintf(stderr, "[fec] --bitrate-desired is deprecated; use --bitrate-max\n");
			break;
		case OPT_BITRATE_TOL:   cfg.bitrate_tolerance = (float)atof(optarg); break;
		case OPT_BITRATE_GRACE: cfg.bitrate_grace_s = (float)atof(optarg); break;
		case OPT_MCS_SETTLE:    cfg.mcs_settle_s    = (float)atof(optarg); break;
		case OPT_DRY_RUN:      cfg.dry_run        = true; break;
		case OPT_BOOST_S:      cfg.boost_s        = (float)atof(optarg); break;
		case OPT_BOOST_MULT:   cfg.boost_mult     = (float)atof(optarg); break;
		case OPT_WFB_STATS_PORT: {
			int p = atoi(optarg);
			if (p < 0 || p > 65535) {
				fprintf(stderr, "invalid --wfb-stats-port (1..65535)\n");
				return 1;
			}
			cfg.wfb_stats_port = (uint16_t)p;
			break;
		}
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

	/* Optional wfb_tx -Y stats subscriber. -1 = disabled (polling mode). */
	int stats_fd = -1;
	if (cfg.wfb_stats_port != 0) {
		stats_fd = wfb_stats_open_listener(cfg.wfb_stats_port);
		if (stats_fd < 0) {
			LOG("wfb-stats: bind on udp:%u failed (%s); falling back to CMD_GET_RADIO polling",
			    cfg.wfb_stats_port, strerror(errno));
			cfg.wfb_stats_port = 0;
		} else {
			LOG("wfb-stats: listening on udp:%u (radio + fec_k/n driven by datagrams; CMD_GET_RADIO polling disabled)",
			    cfg.wfb_stats_port);
		}
	}

	Controller ctrl = {0};
	HeadroomRing ring = {0};
	FpsEst fps = {0};

	uint64_t next_subscribe_us = 0;
	uint64_t next_radio_us     = 0;
	uint64_t next_heartbeat_us = 0;

	/* Last known radio state (for reporting + link budget). */
	RadioState radio = {0};

	/* Tracks the bitrate target we last asserted to venc. The controller
	 * is purely write-side now — it never queries venc.bitrate over HTTP.
	 * If something else changes venc.bitrate externally, we silently
	 * re-assert safe_budget on the next 1 Hz tick. The k-down dwell and
	 * k-up cooldown absorb the resulting EWMA transient without
	 * thrashing CMD_SET_FEC. -1 = nothing written yet. */
	long last_written_kbps = -1;

	/* Tracks the FEC sizes we last asserted via CMD_SET_FEC. Used by the
	 * stats-subscribe path to detect external set_fec calls. -1 = nothing
	 * written yet. */
	int last_written_fec_k = -1;
	int last_written_fec_n = -1;

	/* Subscriber-side housekeeping: when the first datagram arrives we
	 * log peer + key fields once (so the operator can see the wiring is
	 * working without enabling -v). last_stats_us tracks freshness so
	 * we can flag a stale stream when wfb_tx exits or its -Y is dropped.
	 * last_seq is the producer's monotonic counter from the JSON
	 * (additive v1 extension) so we can flag dropped datagrams + wfb_tx
	 * restart (-1 = unset, awaiting first observation). */
	bool     stats_first_logged    = false;
	uint64_t last_stats_us         = 0;
	bool     stats_was_stale       = false;
	long     last_stats_seq        = -1;
	const uint64_t STATS_STALE_US  = 3000000ULL;  /* 3 s */

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

		/* --- 5 s heartbeat (always on) --- */
		if (now >= next_heartbeat_us) {
			if (ctrl.have_current && radio.valid) {
				int k = ctrl.current.k, n = ctrl.current.n;
				float fps_hz = fps_get(&fps);
				bool boost = (ctrl.boost_until_us != 0 && now < ctrl.boost_until_us);
				LOG("hb: k=%d n=%d avg=%.1fkB fps=%.1f mcs=%d phy=%.1fMbps br=%ldkbps upd=%u%s",
				    k, n,
				    ctrl.avg_frame_size / 1024.0f, fps_hz,
				    radio.mcs, radio.phy_mbps,
				    last_written_kbps,
				    ctrl.update_count,
				    boost ? " BOOST" : "");
			}
			next_heartbeat_us = now + 5000000ULL;
		}

		/* --- Radio observation + bitrate assertion ---
		 *
		 * Two paths, mutually exclusive (chosen at startup):
		 *
		 *   stats-subscribe (--wfb-stats-port set):
		 *     Drain stats_fd in the sidecar poll() block below.
		 *     Each datagram drives radio_apply_observation() +
		 *     bitrate_assert(). Lower latency on radio changes
		 *     (per-datagram instead of up-to-1s) and decoupled from
		 *     wfb_tx process lifecycle.
		 *
		 *   polling (default):
		 *     1 Hz CMD_GET_RADIO request/response. No fec_k/n info
		 *     comes back through this channel, so external set_fec
		 *     detection is unavailable in this mode. */
		if (cfg.wfb_stats_port == 0 && now >= next_radio_us) {
			CmdResp resp;
			if (wfb_get_radio(&cfg, &resp) == 0) {
				radio_apply_observation(
				    &radio, &ctrl, &cfg,
				    resp.u.get_radio.mcs_index,
				    resp.u.get_radio.bandwidth,
				    resp.u.get_radio.short_gi,
				    resp.u.get_radio.stbc,
				    resp.u.get_radio.ldpc,
				    resp.u.get_radio.vht_mode,
				    resp.u.get_radio.vht_nss,
				    -1, -1,  /* polling has no FEC info */
				    &last_written_fec_k, &last_written_fec_n,
				    now);
				LOGV(&cfg, "radio: mcs=%d bw=%d gi=%s vht=%d nss=%d phy=%.1f Mbps",
				     radio.mcs, radio.bandwidth,
				     radio.short_gi ? "short" : "long",
				     radio.vht_mode, radio.vht_nss, radio.phy_mbps);
			} else {
				LOGV(&cfg, "radio: get_radio timed out");
			}
			bitrate_assert(&radio, &ctrl, &cfg, &last_written_kbps, now);
			next_radio_us = now + (uint64_t)(cfg.radio_poll_s * 1e6);
		}

		/* --- Poll sidecar (and stats UDP if enabled) for up to 100 ms --- */
		struct pollfd pfds[2];
		int npfds = 0;
		int sidecar_idx = -1, stats_idx = -1;
		pfds[npfds].fd = sfd;
		pfds[npfds].events = POLLIN;
		sidecar_idx = npfds++;
		if (stats_fd >= 0) {
			pfds[npfds].fd = stats_fd;
			pfds[npfds].events = POLLIN;
			stats_idx = npfds++;
		}
		int pr = poll(pfds, npfds, 100);
		if (pr < 0) { if (errno == EINTR) continue; perror("poll"); break; }
		if (pr == 0) continue;

		/* Drain wfb_tx stats first — radio info may inform a same-tick
		 * sidecar read's bitrate calc. recvfrom() (vs recv()) so we can
		 * surface the peer in the first-datagram log. */
		if (stats_idx >= 0 && (pfds[stats_idx].revents & POLLIN)) {
			char dgram[2048];
			struct sockaddr_in peer;
			socklen_t plen;
			for (;;) {
				plen = sizeof(peer);
				ssize_t n = recvfrom(stats_fd, dgram, sizeof(dgram) - 1, 0,
				                     (struct sockaddr *)&peer, &plen);
				if (n <= 0) break;
				dgram[n] = '\0';
				int rc = wfb_stats_handle_datagram(dgram,
				                                   &radio, &ctrl, &cfg,
				                                   &last_written_fec_k,
				                                   &last_written_fec_n,
				                                   &last_written_kbps,
				                                   &last_stats_seq,
				                                   now_us());
				if (rc != 0) continue;

				last_stats_us = now_us();
				if (stats_was_stale) {
					LOG("wfb-stats: stream resumed (radio info refreshed)");
					stats_was_stale = false;
				}
				if (!stats_first_logged) {
					char ip[INET_ADDRSTRLEN];
					inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
					LOG("wfb-stats: first datagram from %s:%u (mcs=%d bw=%d fec=%d/%d phy=%.1fMbps)",
					    ip, (unsigned)ntohs(peer.sin_port),
					    radio.mcs, radio.bandwidth,
					    last_written_fec_k, last_written_fec_n,
					    radio.phy_mbps);
					stats_first_logged = true;
				}
			}
		}

		/* Stale-stream check: if subscriber mode is on and we've seen
		 * at least one datagram, log once when datagrams stop arriving
		 * for STATS_STALE_US. Edge-triggered both ways (paired with
		 * the "stream resumed" log inside the recv loop). */
		if (cfg.wfb_stats_port != 0 && last_stats_us != 0) {
			uint64_t age = now_us() - last_stats_us;
			if (!stats_was_stale && age > STATS_STALE_US) {
				LOG("wfb-stats: no datagrams for %.1fs, radio info may be stale",
				    age / 1e6f);
				stats_was_stale = true;
			}
		}

		if (!(pfds[sidecar_idx].revents & POLLIN)) continue;

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
				else {
					last_written_fec_k = next_params.k;
					last_written_fec_n = next_params.n;
				}
			} else {
				last_written_fec_k = next_params.k;
				last_written_fec_n = next_params.n;
			}
		}
	}

	if (stats_fd >= 0) close(stats_fd);
	close(sfd);
	LOG("stopped after %u FEC updates", ctrl.update_count);
	return 0;
}
