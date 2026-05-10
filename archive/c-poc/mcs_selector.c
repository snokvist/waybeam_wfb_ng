/*
 * mcs_selector — Simple C POC for adaptive wfb-ng MCS selection.
 *
 * Subscribes to wfb_rx -Y rx_ant JSON datagrams, computes an effective
 * RSSI (smoothed antenna RSSI minus a smoothed loss-ratio penalty),
 * walks an asymmetric 3-bucket FSM (fast-down / slow-up + deadband
 * around the bucket thresholds + dwell hysteresis + per-direction
 * cooldown + oscillation backoff), and sends CMD_SET_RADIO to wfb_tx
 * varying mcs_index in isolation — every other radiotap field
 * (bandwidth, stbc, ldpc, short_gi, vht_mode, vht_nss) is preserved
 * from a CMD_GET_RADIO sync done at startup and refreshed after every
 * successful SET.
 *
 * Single thread, poll() loop. Sync UDP req/resp for control (the recv
 * is short — 300 ms max — and runs at most once per bucket commit, so
 * it can't starve the rx_ant intake at 10 Hz).
 *
 * Coexistence with fec_controller:
 *   Both daemons read CMD_GET_RADIO; mcs_selector additionally writes.
 *   When fec_controller is in --wfb-stats-port subscribe mode it sees
 *   our MCS write on the next wfb_tx -Y stats datagram, logs an
 *   "external change" entry, and arms its own mcs_settle_s window so
 *   k recomputes against the new airtime baseline. No coordination
 *   protocol needed.
 *
 * Failsafe: if no rx_ant datagram arrives for failsafe_timeout_s, the
 * FSM forces range bucket 0 (lowest MCS in the active range) and
 * freezes until failsafe_recovery_consecutive consecutive good samples
 * arrive. The boot-time SET (range[0] mcs) is OPT-IN via --start-low;
 * default is observe-then-act so a healthy link doesn't take a hit on
 * every selector restart.
 *
 * Single thread, poll() loop. No libs beyond libc.
 *
 * Target: SigmaStar Infinity6E (armv7l / OpenIPC). Cross-build with the
 * star6e toolchain. Designed to run on the vehicle alongside
 * fec_controller; both endpoints (wfb_rx -Y stats, wfb_tx control) are
 * 127.0.0.1.
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
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ── wfb_tx control protocol (matches poc/build/wfb-ng/src/tx_cmd.h) ───── */

#define CMD_SET_RADIO  2
#define CMD_GET_RADIO  4

#pragma pack(push, 1)
typedef struct {
	uint8_t stbc;
	uint8_t ldpc;          /* bool, 0/1 */
	uint8_t short_gi;      /* bool, 0/1 */
	uint8_t bandwidth;
	uint8_t mcs_index;
	uint8_t vht_mode;      /* bool, 0/1 */
	uint8_t vht_nss;
} RadioBody;            /* 7 bytes */

typedef struct {
	uint32_t req_id;       /* network byte order */
	uint8_t  cmd_id;
	union {
		RadioBody set_radio;
		struct { uint8_t pad; } get_radio;
	} u;
} CmdReq;

typedef struct {
	uint32_t req_id;
	uint32_t rc;
	union {
		RadioBody get_radio;
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

/* RSSI aggregator across antennas. Encoded as int so the TunableDesc
 * registry can expose it; CLI accepts the string form too. */
enum { AGG_BEST_AVG = 0, AGG_BEST_MAX = 1, AGG_MEAN_AVG = 2 };

typedef struct {
	/* Endpoints */
	char     stats_host[64];          /* UDP listener for wfb_rx -Y JSON */
	uint16_t stats_port;
	char     wfb_host[64];            /* wfb_tx control (CMD_SET/GET_RADIO) */
	uint16_t wfb_port;

	/* RSSI thresholds (dBm) defining the three buckets within a range:
	 *   eff_rssi <  rssi_thresh_low                       -> bucket 0
	 *   rssi_thresh_low <= eff < rssi_thresh_high          -> bucket 1
	 *   eff_rssi >= rssi_thresh_high                       -> bucket 2 */
	float    rssi_thresh_low;
	float    rssi_thresh_high;

	/* Crossing deadband (dB). Going UP, eff must clear the threshold by
	 * +deadband; going DOWN, eff must drop below by -deadband. Stops
	 * threshold-grazing oscillation. */
	float    rssi_deadband_db;

	/* Smoothing */
	float    rssi_ewma_alpha;         /* 0.3 ≈ 3-sample memory at 10 Hz */
	float    loss_ewma_alpha;         /* faster — loss is the bad-news lane */
	int      rssi_aggregator;         /* AGG_BEST_AVG / BEST_MAX / MEAN_AVG */

	/* Loss penalty: split into two coefficients so the operator can
	 * weight actual post-FEC loss differently from pre-FEC stress.
	 *
	 *   penalty = clamp(lost%       * loss_lost_penalty_db_per_pct +
	 *                   fec_recov%  * loss_recovered_penalty_db_per_pct,
	 *                   0, loss_penalty_max_db)
	 *
	 * Default loss_recovered = 0.0 because with typical wfb-ng FEC
	 * configs (e.g. -k 6 -n 10 = 40% parity), fec_recovered routinely
	 * sits at 15-25% on a HEALTHY link — that's "FEC doing its job,"
	 * not bad-link signal. Set non-zero only if you want the selector
	 * to react to FEC consumption before any data is actually lost. */
	float    loss_lost_penalty_db_per_pct;
	float    loss_recovered_penalty_db_per_pct;
	float    loss_penalty_max_db;

	/* Asymmetric gating (fast drop, slow climb). */
	int      up_consecutive;
	int      down_consecutive;
	float    up_cooldown_s;
	float    down_cooldown_s;

	/* Failsafe watchdog: forces bucket 0 if no rx_ant arrives for
	 * failsafe_timeout_s; freezes FSM until failsafe_recovery_consecutive
	 * good samples seen. */
	float    failsafe_timeout_s;
	int      failsafe_recovery_consecutive;

	/* Oscillation detector — mirrors fec_controller pattern. */
	float    oscillation_window_s;
	int      oscillation_threshold;
	float    oscillation_backoff;

	/* Active range: which mcs_index to emit per bucket. Defaults to
	 * (1,2,3) ("med" range). Editable at runtime via /set?mcs_bucket_0=N. */
	int      mcs_bucket_0;
	int      mcs_bucket_1;
	int      mcs_bucket_2;
	int      mcs_min;                 /* defence-in-depth clamp on emit */
	int      mcs_max;

	/* HTTP API on 127.0.0.1:api_port (0 disables). Routes /, /params,
	 * /set?k=v, /status, /events, /health. */
	int      api_port;

	/* Behavior */
	bool     start_low;               /* boot-time SET range[0] mcs (default off) */
	bool     dry_run;                 /* compute but do not send CMD_SET_RADIO */
	int      verbose;
} Config;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

/* Monotonic timestamp prefix: "[mcs t=  12.345] …" */
static uint64_t g_log_start_us = 0;
static void log_init(void) { g_log_start_us = now_us(); }
static double log_rel_s(void)
{
	return (double)(now_us() - g_log_start_us) / 1e6;
}

/* Forward declaration — defined alongside the SSE client table near the
 * bottom of the API section. Broadcasts a formatted log line (without the
 * "[mcs t=...]" prefix) to all SSE-subscribed clients as a JSON event. */
static void sse_broadcast_log(double t_s, const char *line);

static void log_emit(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void log_emit(const char *fmt, ...)
{
	char body[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n >= (int)sizeof(body)) n = sizeof(body) - 1;
	double t = log_rel_s();
	fprintf(stderr, "[mcs t=%8.3f] %s\n", t, body);
	sse_broadcast_log(t, body);
}

#define LOGV(cfg, fmt, ...) \
	do { if ((cfg)->verbose) log_emit(fmt, ##__VA_ARGS__); } while (0)
#define LOG(fmt, ...) log_emit(fmt, ##__VA_ARGS__)

/* ── Tiny JSON scrapers ──────────────────────────────────────────────── */

/* Find an integer field by name. Same idiom as fec_controller — strstr +
 * strtol on a known well-formed schema. Returns 0 ok, -1 if missing or
 * unparseable. */
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

/* Find an integer field nested inside a specific block. Caller passes
 * the outer key (e.g. "rssi") and the inner key (e.g. "avg"); we locate
 * `"<outer>":{` first then look for `"<inner>":N` within the next
 * ~256 bytes. Returns 0 ok, -1 missing.
 *
 * Used to disambiguate ant[].rssi.avg from any other "avg" that might
 * appear in a future schema extension. */
static int json_get_int_in(const char *block, const char *inner, long *out)
{
	const char *brace = strchr(block, '{');
	if (!brace) return -1;
	const char *end = strchr(brace, '}');
	if (!end) return -1;
	char window[512];
	size_t wlen = (size_t)(end - brace + 1);
	if (wlen >= sizeof(window)) wlen = sizeof(window) - 1;
	memcpy(window, brace, wlen);
	window[wlen] = '\0';
	return json_get_int(window, inner, out);
}

/* ── Scorer: EWMA RSSI + EWMA loss penalty → effective RSSI ──────────── */

typedef struct {
	float  smoothed_rssi;
	float  smoothed_lost;       /* (lost / data) */
	float  smoothed_recov;      /* (fec_recovered / data) */
	bool   have_rssi;
	bool   have_loss;
} Scorer;

typedef struct {
	float raw_rssi;
	float smoothed_rssi;
	float lost_ratio;          /* 0..1 — only "lost" counter (post-FEC) */
	float smoothed_lost_ratio;
	float recov_ratio;         /* 0..1 — only "fec_recovered" counter */
	float smoothed_recov_ratio;
	float loss_penalty_db;
	float effective_rssi;
} Score;

static void scorer_reset(Scorer *s)
{
	s->smoothed_rssi = 0.0f;
	s->smoothed_lost = 0.0f;
	s->smoothed_recov = 0.0f;
	s->have_rssi = false;
	s->have_loss = false;
}

/* Pull antenna RSSI values out of one rx_ant JSON datagram, aggregate
 * per cfg->rssi_aggregator, then fold into the EWMA along with the loss
 * ratios.
 *
 * Loss denominator is `pkt.uniq` (unique packets after multi-antenna
 * dedup) rather than `pkt.data` (per-antenna data count, scales with
 * antenna count). uniq matches the operator's mental model of "packets
 * I expected to receive in this interval." Falls back to `data` if uniq
 * isn't present.
 *
 * Returns false if the datagram has no antenna entries (caller treats
 * like "no scoring data this tick" — failsafe still ticks off
 * last_datagram_us, so this doesn't keep the watchdog from firing). */
static bool scorer_update(Scorer *s, const Config *cfg, const char *body,
                          long pkt_uniq, long pkt_lost, long pkt_fec_recovered,
                          Score *out)
{
	float best = -200.0f;       /* very low sentinel */
	float sum = 0.0f;
	int   count = 0;
	bool  saw_any = false;

	const char *p = body;
	for (;;) {
		const char *r = strstr(p, "\"rssi\":");
		if (!r) break;
		long avg, mx;
		if (json_get_int_in(r, "avg", &avg) != 0) break;
		mx = avg;  /* default if "max" absent — keeps AGG_BEST_MAX safe */
		(void)json_get_int_in(r, "max", &mx);
		float val;
		switch (cfg->rssi_aggregator) {
		case AGG_BEST_MAX: val = (float)mx; break;
		default:           val = (float)avg; break;
		}
		if (cfg->rssi_aggregator == AGG_MEAN_AVG) {
			sum += (float)avg;
		} else {
			if (!saw_any || val > best) best = val;
		}
		count++;
		saw_any = true;
		/* Advance past this `}` so the next strstr finds the next ant. */
		const char *brace_close = strchr(r, '}');
		if (!brace_close) break;
		p = brace_close + 1;
	}

	if (!saw_any) return false;

	float raw = (cfg->rssi_aggregator == AGG_MEAN_AVG)
	    ? (sum / (float)count)
	    : best;

	if (!s->have_rssi) {
		s->smoothed_rssi = raw;
		s->have_rssi = true;
	} else {
		float a = cfg->rssi_ewma_alpha;
		s->smoothed_rssi = a * raw + (1.0f - a) * s->smoothed_rssi;
	}

	long denom = pkt_uniq > 0 ? pkt_uniq : 1;
	float lost_r  = (pkt_lost          > 0) ? (float)pkt_lost          / (float)denom : 0.0f;
	float recov_r = (pkt_fec_recovered > 0) ? (float)pkt_fec_recovered / (float)denom : 0.0f;
	if (lost_r  > 1.0f) lost_r  = 1.0f;
	if (recov_r > 1.0f) recov_r = 1.0f;

	if (!s->have_loss) {
		s->smoothed_lost  = lost_r;
		s->smoothed_recov = recov_r;
		s->have_loss = true;
	} else {
		float a = cfg->loss_ewma_alpha;
		s->smoothed_lost  = a * lost_r  + (1.0f - a) * s->smoothed_lost;
		s->smoothed_recov = a * recov_r + (1.0f - a) * s->smoothed_recov;
	}

	float penalty =
		cfg->loss_lost_penalty_db_per_pct      * 100.0f * s->smoothed_lost +
		cfg->loss_recovered_penalty_db_per_pct * 100.0f * s->smoothed_recov;
	if (penalty < 0.0f) penalty = 0.0f;
	if (penalty > cfg->loss_penalty_max_db) penalty = cfg->loss_penalty_max_db;

	out->raw_rssi = raw;
	out->smoothed_rssi = s->smoothed_rssi;
	out->lost_ratio = lost_r;
	out->smoothed_lost_ratio = s->smoothed_lost;
	out->recov_ratio = recov_r;
	out->smoothed_recov_ratio = s->smoothed_recov;
	out->loss_penalty_db = penalty;
	out->effective_rssi = s->smoothed_rssi - penalty;
	return true;
}

/* ── Bucket FSM (deadband-aware) ──────────────────────────────────────── */

/* Pick a bucket (0/1/2) for a given effective RSSI. `current` is the
 * bucket the selector last committed to; the deadband makes transitions
 * sticky relative to it. current < 0 means "no commitment yet" — pure
 * threshold lookup. */
static int bucket_from_rssi(float rssi, int current, const Config *cfg)
{
	float lo = cfg->rssi_thresh_low;
	float hi = cfg->rssi_thresh_high;
	float db = cfg->rssi_deadband_db;

	if (current < 0) {
		if (rssi <  lo) return 0;
		if (rssi <  hi) return 1;
		return 2;
	}
	if (current == 0) {
		if (rssi >= lo + db)
			return (rssi >= hi + db) ? 2 : 1;
		return 0;
	}
	if (current == 1) {
		if (rssi <  lo - db) return 0;
		if (rssi >= hi + db) return 2;
		return 1;
	}
	/* current == 2 */
	if (rssi <  hi - db)
		return (rssi <  lo - db) ? 0 : 1;
	return 2;
}

static int mcs_for_bucket(int bucket, const Config *cfg)
{
	int mcs;
	switch (bucket) {
	case 0:  mcs = cfg->mcs_bucket_0; break;
	case 1:  mcs = cfg->mcs_bucket_1; break;
	default: mcs = cfg->mcs_bucket_2; break;
	}
	if (mcs < cfg->mcs_min) mcs = cfg->mcs_min;
	if (mcs > cfg->mcs_max) mcs = cfg->mcs_max;
	return mcs;
}

/* ── Selector: thin state + the decision pipeline ─────────────────────── */

#define OSC_RING 16

typedef struct {
	int       current_bucket;        /* -1 = none */
	int       current_mcs;           /* -1 = none */
	uint64_t  last_change_us;
	uint64_t  last_datagram_us;      /* 0 = none seen yet */

	int       pending_bucket;        /* -1 = none */
	int       pending_streak;

	bool      in_failsafe;
	int       recovery_streak;

	/* Oscillation ring — last N change times in microseconds. */
	uint64_t  changes_us[OSC_RING];
	int       changes_count;
	int       changes_head;

	uint32_t  commit_count;
} Selector;

typedef enum {
	SD_NONE = 0,
	SD_INIT,
	SD_DOWN,
	SD_UP,
	SD_FAILSAFE,
	SD_RECOVERED
} SelectDecision;

static const char *sd_name(SelectDecision d)
{
	switch (d) {
	case SD_INIT:       return "init";
	case SD_DOWN:       return "down";
	case SD_UP:         return "up";
	case SD_FAILSAFE:   return "failsafe";
	case SD_RECOVERED:  return "recovered";
	default:            return "none";
	}
}

static void selector_init(Selector *s)
{
	s->current_bucket = -1;
	s->current_mcs = -1;
	s->last_change_us = 0;
	s->last_datagram_us = 0;
	s->pending_bucket = -1;
	s->pending_streak = 0;
	s->in_failsafe = false;
	s->recovery_streak = 0;
	s->changes_count = 0;
	s->changes_head = 0;
	s->commit_count = 0;
}

/* Logical expire: drop ring entries older than oscillation_window_s. */
static void selector_expire_changes(Selector *s, const Config *cfg, uint64_t now)
{
	uint64_t window_us = (uint64_t)(cfg->oscillation_window_s * 1e6f);
	if (window_us == 0 || now < window_us) return;
	uint64_t cutoff = now - window_us;
	while (s->changes_count > 0) {
		int oldest = (s->changes_head - s->changes_count + OSC_RING) % OSC_RING;
		if (s->changes_us[oldest] >= cutoff) break;
		s->changes_count--;
	}
}

static void selector_record_change(Selector *s, const Config *cfg, uint64_t now)
{
	s->changes_us[s->changes_head] = now;
	s->changes_head = (s->changes_head + 1) % OSC_RING;
	if (s->changes_count < OSC_RING) s->changes_count++;
	selector_expire_changes(s, cfg, now);
}

static bool selector_is_oscillating(const Selector *s, const Config *cfg)
{
	return s->changes_count >= cfg->oscillation_threshold;
}

/* Commit `bucket` as the new state. Returns the corresponding mcs_index
 * via `*out_mcs`. */
static SelectDecision selector_commit(Selector *s, const Config *cfg,
                                      int bucket, uint64_t now,
                                      SelectDecision reason, int *out_mcs)
{
	int prev_mcs = s->current_mcs;
	s->current_bucket = bucket;
	s->current_mcs = mcs_for_bucket(bucket, cfg);
	if (prev_mcs != s->current_mcs) {
		s->last_change_us = now;
		selector_record_change(s, cfg, now);
		s->commit_count++;
	}
	s->pending_bucket = -1;
	s->pending_streak = 0;
	*out_mcs = s->current_mcs;
	return reason;
}

/* Drive the watchdog without a fresh datagram. Returns SD_FAILSAFE on
 * a fresh trip into bucket 0 (caller emits SET_RADIO). */
static SelectDecision selector_tick_no_data(Selector *s, const Config *cfg,
                                            uint64_t now, int *out_mcs)
{
	/* Always expire stale change-times so /status reflects the truth even
	 * during long idle stretches with no commits. */
	selector_expire_changes(s, cfg, now);

	if (s->last_datagram_us == 0) return SD_NONE;
	uint64_t gap = now - s->last_datagram_us;
	if (gap < (uint64_t)(cfg->failsafe_timeout_s * 1e6f)) return SD_NONE;
	if (s->in_failsafe && s->current_bucket == 0) return SD_NONE;

	LOG("failsafe: no rx_ant for %.2fs (>%.2fs) — forcing bucket 0",
	    (double)gap / 1e6, (double)cfg->failsafe_timeout_s);
	s->in_failsafe = true;
	s->recovery_streak = 0;
	return selector_commit(s, cfg, 0, now, SD_FAILSAFE, out_mcs);
}

/* Feed one Score (the per-tick scorer output). Returns the decision. */
static SelectDecision selector_update(Selector *s, const Config *cfg,
                                      const Score *score, uint64_t now,
                                      int *out_mcs)
{
	s->last_datagram_us = now;

	int candidate = bucket_from_rssi(score->effective_rssi,
	                                 s->current_bucket, cfg);

	/* First commit ever — emit immediately so wfb_tx + selector agree. */
	if (s->current_bucket < 0)
		return selector_commit(s, cfg, candidate, now, SD_INIT, out_mcs);

	/* Failsafe recovery gating: even good samples don't unfreeze the FSM
	 * until we've seen failsafe_recovery_consecutive of them. */
	if (s->in_failsafe) {
		float floor_eff = cfg->rssi_thresh_low + cfg->rssi_deadband_db;
		if (score->effective_rssi >= floor_eff) {
			s->recovery_streak++;
			if (s->recovery_streak >= cfg->failsafe_recovery_consecutive) {
				LOG("failsafe: recovered after %d good samples",
				    s->recovery_streak);
				s->in_failsafe = false;
				s->recovery_streak = 0;
				/* Fall through to normal logic — caller may want to
				 * report a "recovered" event but no SET fires unless
				 * the bucket changes. We commit current_bucket=0
				 * still (no MCS change) by falling through. */
			} else {
				return SD_NONE;
			}
		} else {
			s->recovery_streak = 0;
			return SD_NONE;
		}
	}

	if (candidate == s->current_bucket) {
		s->pending_bucket = -1;
		s->pending_streak = 0;
		return SD_NONE;
	}

	/* Streak accounting for hysteresis. */
	if (candidate != s->pending_bucket) {
		s->pending_bucket = candidate;
		s->pending_streak = 1;
	} else {
		s->pending_streak++;
	}

	bool going_down = candidate < s->current_bucket;
	int  required = going_down
	    ? cfg->down_consecutive
	    : cfg->up_consecutive;
	if (s->pending_streak < required) return SD_NONE;

	/* Cooldown. */
	float elapsed = (float)(now - s->last_change_us) / 1e6f;
	if (going_down) {
		if (elapsed < cfg->down_cooldown_s) return SD_NONE;
		return selector_commit(s, cfg, candidate, now, SD_DOWN, out_mcs);
	}
	float cooldown = cfg->up_cooldown_s;
	if (selector_is_oscillating(s, cfg))
		cooldown *= cfg->oscillation_backoff;
	if (elapsed < cooldown) return SD_NONE;
	return selector_commit(s, cfg, candidate, now, SD_UP, out_mcs);
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

/* Bind a UDP listener on INADDR_ANY:port for wfb_rx -Y stats datagrams.
 * Non-blocking so the main loop's poll() can drain it without stalling. */
static int stats_open_listener(uint16_t port)
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

/* ── wfb_tx control client (CMD_GET_RADIO + CMD_SET_RADIO) ───────────── */

static uint32_t g_req_id = 1;

/* GET — sync request/response. 300 ms timeout. Returns 0 on success and
 * fills `out`. Single-shot socket so we don't carry control-channel
 * state across calls; cheap on a single-host loopback. */
static int wfb_get_radio(const Config *cfg, RadioBody *out)
{
	struct sockaddr_in dst;
	if (resolve_ipv4(cfg->wfb_host, cfg->wfb_port, &dst) != 0) return -1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	uint32_t req_id = g_req_id++;
	CmdReq req = {0};
	req.req_id = htonl(req_id);
	req.cmd_id = CMD_GET_RADIO;

	/* offsetof(CmdReq, u) = 5 on packed layout (4 + 1) */
	if (sendto(fd, &req, 5, 0,
	           (const struct sockaddr*)&dst, sizeof(dst)) != 5) {
		close(fd);
		return -1;
	}

	struct pollfd p = { .fd = fd, .events = POLLIN };
	int pr = poll(&p, 1, 300);
	if (pr <= 0) { close(fd); return -1; }

	CmdResp resp = {0};
	ssize_t n = recv(fd, &resp, sizeof(resp), 0);
	close(fd);
	if (n < (ssize_t)(sizeof(uint32_t) * 2 + sizeof(RadioBody))) return -1;
	if (ntohl(resp.req_id) != req_id) return -1;
	if (ntohl(resp.rc) != 0) return -1;

	*out = resp.u.get_radio;
	return 0;
}

/* SET — sync request, sync response (small). Sends the full RadioBody;
 * caller is responsible for filling all fields. Returns 0 on success
 * (ack rc=0 with matching req_id). */
static int wfb_set_radio(const Config *cfg, const RadioBody *params)
{
	struct sockaddr_in dst;
	if (resolve_ipv4(cfg->wfb_host, cfg->wfb_port, &dst) != 0) return -1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	uint32_t req_id = g_req_id++;
	CmdReq req = {0};
	req.req_id = htonl(req_id);
	req.cmd_id = CMD_SET_RADIO;
	req.u.set_radio = *params;
	/* Normalise bools to 0/1 even though Python tests prove the
	 * unnormalised path also "works" — defensive against future wfb_tx
	 * versions that may strict-check. */
	req.u.set_radio.ldpc     = req.u.set_radio.ldpc     ? 1 : 0;
	req.u.set_radio.short_gi = req.u.set_radio.short_gi ? 1 : 0;
	req.u.set_radio.vht_mode = req.u.set_radio.vht_mode ? 1 : 0;

	/* wire: req_id(4) + cmd_id(1) + body(7) = 12 bytes */
	if (sendto(fd, &req, 12, 0,
	           (const struct sockaddr*)&dst, sizeof(dst)) != 12) {
		close(fd);
		return -1;
	}

	struct pollfd p = { .fd = fd, .events = POLLIN };
	int pr = poll(&p, 1, 300);
	if (pr <= 0) { close(fd); return -1; }

	CmdResp resp = {0};
	ssize_t n = recv(fd, &resp, sizeof(resp), 0);
	close(fd);
	if (n < (ssize_t)(sizeof(uint32_t) * 2)) return -1;
	if (ntohl(resp.req_id) != req_id) return -1;
	if (ntohl(resp.rc) != 0) return -1;
	return 0;
}

/* ── Local HTTP API (live tuning) — copied straight from fec_controller ── */

typedef enum { TF_INT, TF_LONG, TF_FLOAT, TF_BOOL } TunableType;

typedef struct {
	const char  *name;
	TunableType  type;
	size_t       offset;
	double       lo, hi;
	const char  *help;
} TunableDesc;

static const TunableDesc TUNABLES[] = {
	/* Thresholds */
	{"rssi_thresh_low",      TF_FLOAT, offsetof(Config, rssi_thresh_low),      -120.0,   0.0, "lower RSSI threshold (dBm)"},
	{"rssi_thresh_high",     TF_FLOAT, offsetof(Config, rssi_thresh_high),     -120.0,   0.0, "upper RSSI threshold (dBm)"},
	{"rssi_deadband_db",     TF_FLOAT, offsetof(Config, rssi_deadband_db),       0.0,  20.0, "symmetric deadband around thresholds"},

	/* Smoothing */
	{"rssi_ewma_alpha",      TF_FLOAT, offsetof(Config, rssi_ewma_alpha),       0.001,  1.0, "EWMA alpha for RSSI"},
	{"loss_ewma_alpha",      TF_FLOAT, offsetof(Config, loss_ewma_alpha),       0.001,  1.0, "EWMA alpha for loss ratio"},
	{"rssi_aggregator",      TF_INT,   offsetof(Config, rssi_aggregator),       0,      2,   "0=best_avg 1=best_max 2=mean_avg"},

	/* Loss penalty (split between true post-FEC loss and pre-FEC recovery) */
	{"loss_lost_penalty_db_per_pct",      TF_FLOAT, offsetof(Config, loss_lost_penalty_db_per_pct),      0.0, 5.0, "dB per % of post-FEC lost"},
	{"loss_recovered_penalty_db_per_pct", TF_FLOAT, offsetof(Config, loss_recovered_penalty_db_per_pct), 0.0, 5.0, "dB per % of fec_recovered (default 0; FEC working = no signal)"},
	{"loss_penalty_max_db",               TF_FLOAT, offsetof(Config, loss_penalty_max_db),               0.0, 60.0, "cap on combined loss penalty"},

	/* Asymmetric gating */
	{"up_consecutive",       TF_INT,   offsetof(Config, up_consecutive),         1,    20,   "samples required to commit up"},
	{"down_consecutive",     TF_INT,   offsetof(Config, down_consecutive),       1,    20,   "samples required to commit down"},
	{"up_cooldown_s",        TF_FLOAT, offsetof(Config, up_cooldown_s),          0.0,  60.0, "min seconds between up-commits"},
	{"down_cooldown_s",      TF_FLOAT, offsetof(Config, down_cooldown_s),        0.0,  60.0, "min seconds between down-commits"},

	/* Failsafe */
	{"failsafe_timeout_s",          TF_FLOAT, offsetof(Config, failsafe_timeout_s),          0.05, 30.0, "watchdog gap before forcing bucket 0"},
	{"failsafe_recovery_consecutive", TF_INT, offsetof(Config, failsafe_recovery_consecutive), 1,  20,   "good samples before unfreezing"},

	/* Oscillation */
	{"oscillation_window_s", TF_FLOAT, offsetof(Config, oscillation_window_s),   0.5,  60.0, "rolling window for change-rate"},
	{"oscillation_threshold", TF_INT,  offsetof(Config, oscillation_threshold),  2,    OSC_RING, "changes/window before backoff"},
	{"oscillation_backoff",  TF_FLOAT, offsetof(Config, oscillation_backoff),    1.0,  20.0, "up-cooldown multiplier when oscillating"},

	/* Active range buckets */
	{"mcs_bucket_0",         TF_INT,   offsetof(Config, mcs_bucket_0),           0,    11,   "mcs_index for bucket 0 (lowest)"},
	{"mcs_bucket_1",         TF_INT,   offsetof(Config, mcs_bucket_1),           0,    11,   "mcs_index for bucket 1 (mid)"},
	{"mcs_bucket_2",         TF_INT,   offsetof(Config, mcs_bucket_2),           0,    11,   "mcs_index for bucket 2 (top)"},
	{"mcs_min",              TF_INT,   offsetof(Config, mcs_min),                0,    11,   "clamp emit lower bound"},
	{"mcs_max",              TF_INT,   offsetof(Config, mcs_max),                0,    11,   "clamp emit upper bound"},

	/* Behavior */
	{"start_low",            TF_BOOL,  offsetof(Config, start_low),              0, 0,       "force range[0] mcs at startup (default off)"},
	{"dry_run",              TF_BOOL,  offsetof(Config, dry_run),                0, 0,       "compute but do not send CMD_SET_RADIO"},
	{"verbose",              TF_INT,   offsetof(Config, verbose),                0,    4,    "log verbosity"},
};
#define TUNABLES_COUNT (sizeof(TUNABLES) / sizeof(TUNABLES[0]))

static double tunable_read(const TunableDesc *t, const Config *c)
{
	const char *base = (const char *)c + t->offset;
	switch (t->type) {
	case TF_INT:   return (double) *(const int *)base;
	case TF_LONG:  return (double) *(const long *)base;
	case TF_FLOAT: return (double) *(const float *)base;
	case TF_BOOL:  return (double) (*(const bool *)base ? 1 : 0);
	}
	return 0.0;
}

static int tunable_write(const TunableDesc *t, Config *c, const char *val)
{
	if (t->lo != t->hi) {
		double v = atof(val);
		if (v < t->lo || v > t->hi) return -1;
	}
	char *base = (char *)c + t->offset;
	switch (t->type) {
	case TF_INT:   *(int *)base   = atoi(val); return 0;
	case TF_LONG:  *(long *)base  = atol(val); return 0;
	case TF_FLOAT: *(float *)base = (float)atof(val); return 0;
	case TF_BOOL:
		*(bool *)base = (val[0] == '1' || val[0] == 't' || val[0] == 'T');
		return 0;
	}
	return -1;
}

static const TunableDesc *tunable_find(const char *name)
{
	for (size_t i = 0; i < TUNABLES_COUNT; i++)
		if (strcmp(TUNABLES[i].name, name) == 0) return &TUNABLES[i];
	return NULL;
}

static int json_append_field(char *buf, size_t cap, size_t pos,
                             const TunableDesc *t, const Config *c, bool first)
{
	double v = tunable_read(t, c);
	const char *sep = first ? "" : ",";
	int n;
	if (t->type == TF_INT || t->type == TF_LONG)
		n = snprintf(buf + pos, cap - pos, "%s\"%s\":%lld",
		             sep, t->name, (long long)v);
	else if (t->type == TF_BOOL)
		n = snprintf(buf + pos, cap - pos, "%s\"%s\":%s",
		             sep, t->name, v != 0.0 ? "true" : "false");
	else
		n = snprintf(buf + pos, cap - pos, "%s\"%s\":%g",
		             sep, t->name, v);
	if (n < 0 || (size_t)n >= cap - pos) return -1;
	return n;
}

static int params_to_json(const Config *c, char *buf, size_t cap)
{
	size_t pos = 0;
	int n = snprintf(buf + pos, cap - pos, "{");
	if (n < 0) return -1;
	pos += n;
	for (size_t i = 0; i < TUNABLES_COUNT; i++) {
		int w = json_append_field(buf, cap, pos, &TUNABLES[i], c, i == 0);
		if (w < 0) return -1;
		pos += w;
	}
	n = snprintf(buf + pos, cap - pos, "}");
	if (n < 0 || (size_t)n >= cap - pos) return -1;
	pos += n;
	return (int)pos;
}

typedef struct {
	const Config    *cfg;
	const Selector  *sel;
	const RadioBody *radio;
	bool             radio_valid;
	uint64_t         last_datagram_us;
	float            last_eff_rssi;
	float            last_lost_ratio;
	float            last_recov_ratio;
	float            last_loss_penalty_db;
	int              last_emit_mcs;
} ApiSnapshot;

static int status_to_json(const ApiSnapshot *s, char *buf, size_t cap)
{
	double age_s = s->last_datagram_us == 0
	    ? -1.0
	    : (double)(now_us() - s->last_datagram_us) / 1e6;
	int n = snprintf(buf, cap,
		"{\"uptime_s\":%.3f,"
		"\"selector\":{\"current_bucket\":%d,\"current_mcs\":%d,"
		"\"in_failsafe\":%s,\"recovery_streak\":%d,"
		"\"pending_bucket\":%d,\"pending_streak\":%d,"
		"\"commit_count\":%u,\"changes_in_window\":%d},"
		"\"radio\":{\"valid\":%s,\"mcs\":%d,\"bw\":%d,\"short_gi\":%d,"
		"\"stbc\":%d,\"ldpc\":%d,\"vht_mode\":%d,\"vht_nss\":%d},"
		"\"score\":{\"effective_rssi\":%.2f,\"lost_ratio\":%.4f,"
		"\"recov_ratio\":%.4f,\"loss_penalty_db\":%.2f},"
		"\"stats\":{\"age_s\":%.3f,\"last_emit_mcs\":%d}}",
		log_rel_s(),
		s->sel->current_bucket, s->sel->current_mcs,
		s->sel->in_failsafe ? "true" : "false", s->sel->recovery_streak,
		s->sel->pending_bucket, s->sel->pending_streak,
		s->sel->commit_count, s->sel->changes_count,
		s->radio_valid ? "true" : "false",
		s->radio->mcs_index, s->radio->bandwidth, s->radio->short_gi,
		s->radio->stbc, s->radio->ldpc, s->radio->vht_mode, s->radio->vht_nss,
		(double)s->last_eff_rssi,
		(double)s->last_lost_ratio, (double)s->last_recov_ratio,
		(double)s->last_loss_penalty_db,
		age_s, s->last_emit_mcs);
	if (n < 0 || (size_t)n >= cap) return -1;
	return n;
}

typedef struct {
	const char *name;
	const char *reason;
} ApiError;

static void apply_query(Config *cfg, char *qstr,
                        const char **applied, int *applied_n,
                        ApiError *errors, int *errors_n,
                        int max_keys)
{
	*applied_n = 0;
	*errors_n  = 0;
	while (qstr && *qstr) {
		char *amp = strchr(qstr, '&');
		if (amp) *amp = '\0';
		char *eq = strchr(qstr, '=');
		const char *key = qstr;
		const char *val = NULL;
		if (eq) { *eq = '\0'; val = eq + 1; }
		const TunableDesc *t = tunable_find(key);
		if (!t) {
			if (*errors_n < max_keys) {
				errors[*errors_n].name = key;
				errors[*errors_n].reason = "unknown";
				(*errors_n)++;
			}
		} else if (!val || !*val) {
			if (*errors_n < max_keys) {
				errors[*errors_n].name = key;
				errors[*errors_n].reason = "no_value";
				(*errors_n)++;
			}
		} else if (tunable_write(t, cfg, val) != 0) {
			if (*errors_n < max_keys) {
				errors[*errors_n].name = key;
				errors[*errors_n].reason = "out_of_range";
				(*errors_n)++;
			}
		} else {
			if (*applied_n < max_keys) {
				applied[*applied_n] = key;
				(*applied_n)++;
			}
		}
		if (!amp) break;
		qstr = amp + 1;
	}
}

static int help_to_text(char *buf, size_t cap)
{
	size_t pos = 0;
	int n = snprintf(buf + pos, cap - pos,
		"mcs_selector HTTP API (127.0.0.1)\n"
		"\n"
		"Routes:\n"
		"  GET /                  this help\n"
		"  GET /params            JSON of all tunable Config fields\n"
		"  GET /set?k=v&k=v...    apply changes; returns JSON\n"
		"  GET /status            JSON snapshot of selector + radio\n"
		"  GET /events            text/event-stream — every log line as JSON\n"
		"  GET /health            \"ok\\n\"\n"
		"\n"
		"Tunables (key  type  range  description):\n");
	if (n < 0) return -1;
	pos += n;
	for (size_t i = 0; i < TUNABLES_COUNT; i++) {
		const TunableDesc *t = &TUNABLES[i];
		const char *type =
			t->type == TF_INT   ? "int"   :
			t->type == TF_LONG  ? "long"  :
			t->type == TF_FLOAT ? "float" : "bool";
		char range[64];
		if (t->type == TF_BOOL)              snprintf(range, sizeof(range), "0|1");
		else if (t->lo == t->hi)             snprintf(range, sizeof(range), "(any)");
		else                                 snprintf(range, sizeof(range), "[%g..%g]", t->lo, t->hi);
		n = snprintf(buf + pos, cap - pos, "  %-30s %-5s %-14s %s\n",
		             t->name, type, range, t->help ? t->help : "");
		if (n < 0 || (size_t)n >= cap - pos) return -1;
		pos += n;
	}
	return (int)pos;
}

static void api_send(int fd, int status, const char *content_type,
                     const char *body, int body_len)
{
	const char *reason = (status == 200) ? "OK" :
	                     (status == 400) ? "Bad Request" :
	                     (status == 404) ? "Not Found" :
	                     (status == 500) ? "Internal Server Error" :
	                     (status == 503) ? "Service Unavailable" : "Error";
	char hdr[256];
	int hlen = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n",
		status, reason, content_type, body_len);
	if (hlen > 0) (void)!write(fd, hdr, (size_t)hlen);
	if (body_len > 0) (void)!write(fd, body, (size_t)body_len);
	close(fd);
}

#define API_MAX_CLIENTS 8
#define API_MAX_SSE     4
#define API_BUF_BYTES   2048

typedef struct {
	int      fd;
	uint64_t accepted_us;
	size_t   pos;
	bool     sse;
	char     buf[API_BUF_BYTES];
} ApiClient;

static ApiClient *g_api_clients = NULL;

static int api_listen_open(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
	if (listen(fd, 4) < 0) { close(fd); return -1; }
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return fd;
}

static int api_client_alloc(ApiClient *cs, int fd)
{
	for (int i = 0; i < API_MAX_CLIENTS; i++) {
		if (cs[i].fd < 0) {
			cs[i].fd = fd;
			cs[i].accepted_us = now_us();
			cs[i].pos = 0;
			return i;
		}
	}
	return -1;
}

static void api_client_drop(ApiClient *c)
{
	if (c->fd >= 0) close(c->fd);
	c->fd = -1;
	c->pos = 0;
	c->sse = false;
}

static int json_escape(char *out, size_t cap, size_t pos, const char *in)
{
	size_t start = pos;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		if (pos >= cap - 8) return -1;
		unsigned c = *p;
		if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = (char)c; }
		else if (c == '\n')        { out[pos++] = '\\'; out[pos++] = 'n'; }
		else if (c == '\r')        { out[pos++] = '\\'; out[pos++] = 'r'; }
		else if (c == '\t')        { out[pos++] = '\\'; out[pos++] = 't'; }
		else if (c < 0x20) {
			int n = snprintf(out + pos, cap - pos, "\\u%04x", c);
			if (n < 0) return -1;
			pos += (size_t)n;
		} else {
			out[pos++] = (char)c;
		}
	}
	return (int)(pos - start);
}

static void api_send_sse_headers(int fd)
{
	const char *hdr =
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-store\r\n"
		"X-Accel-Buffering: no\r\n"
		"Connection: keep-alive\r\n\r\n"
		":mcs_selector events stream\n\n";
	(void)!write(fd, hdr, strlen(hdr));
}

/* See fec_controller.c for the long version of the reentrancy invariant.
 * Short version: log_emit() is the SOLE caller; nothing in this function
 * may invoke log_emit() either directly or transitively. */
static void sse_broadcast_log(double t_s, const char *line)
{
	static bool in_broadcast = false;
	if (in_broadcast) return;
	if (!g_api_clients) return;
	in_broadcast = true;
	char ev[1536];
	int hpos = snprintf(ev, sizeof(ev), "data: {\"t_s\":%.3f,\"msg\":\"", t_s);
	if (hpos < 0 || hpos >= (int)sizeof(ev)) goto done;
	int wrote = json_escape(ev, sizeof(ev), (size_t)hpos, line);
	if (wrote < 0) goto done;
	int tpos = hpos + wrote;
	int t = snprintf(ev + tpos, sizeof(ev) - (size_t)tpos, "\"}\n\n");
	if (t < 0 || (size_t)t >= sizeof(ev) - (size_t)tpos) goto done;
	int total = tpos + t;

	for (int i = 0; i < API_MAX_CLIENTS; i++) {
		if (!g_api_clients[i].sse) continue;
		if (g_api_clients[i].fd < 0) continue;
		ssize_t w = send(g_api_clients[i].fd, ev, (size_t)total,
		                 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w == (ssize_t)total) continue;
		close(g_api_clients[i].fd);
		g_api_clients[i].fd = -1;
		g_api_clients[i].sse = false;
		g_api_clients[i].pos = 0;
	}
done:
	in_broadcast = false;
}

static void sse_heartbeat_all(void)
{
	if (!g_api_clients) return;
	const char *ping = ":ping\n\n";
	for (int i = 0; i < API_MAX_CLIENTS; i++) {
		if (!g_api_clients[i].sse) continue;
		if (g_api_clients[i].fd < 0) continue;
		ssize_t w = send(g_api_clients[i].fd, ping, strlen(ping),
		                 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w == (ssize_t)strlen(ping)) continue;
		close(g_api_clients[i].fd);
		g_api_clients[i].fd = -1;
		g_api_clients[i].sse = false;
		g_api_clients[i].pos = 0;
	}
}

static int api_count_sse(const ApiClient *cs)
{
	int n = 0;
	for (int i = 0; i < API_MAX_CLIENTS; i++) if (cs[i].sse) n++;
	return n;
}

static void api_handle_request(ApiClient *c, Config *cfg, ApiSnapshot *snap)
{
	c->buf[c->pos < API_BUF_BYTES ? c->pos : API_BUF_BYTES - 1] = '\0';

	if (strncmp(c->buf, "GET ", 4) != 0) {
		api_send(c->fd, 400, "text/plain", "expected GET\n", 13);
		c->fd = -1;
		return;
	}
	char *path = c->buf + 4;
	char *sp = strchr(path, ' ');
	if (!sp) {
		api_send(c->fd, 400, "text/plain", "malformed request line\n", 23);
		c->fd = -1;
		return;
	}
	*sp = '\0';
	char *qs = strchr(path, '?');
	if (qs) { *qs = '\0'; qs++; }

	char body[8192];
	int n = -1;

	if (strcmp(path, "/") == 0) {
		n = help_to_text(body, sizeof(body));
		if (n > 0) api_send(c->fd, 200, "text/plain", body, n);
		else       api_send(c->fd, 500, "text/plain", "help overflow\n", 14);
	}
	else if (strcmp(path, "/params") == 0) {
		n = params_to_json(snap->cfg, body, sizeof(body));
		if (n > 0) api_send(c->fd, 200, "application/json", body, n);
		else       api_send(c->fd, 500, "text/plain", "json overflow\n", 14);
	}
	else if (strcmp(path, "/set") == 0) {
		const char *applied[32];
		ApiError    errors[32];
		int an = 0, en = 0;
		apply_query(cfg, qs, applied, &an, errors, &en, 32);
		size_t pos = 0;
		int w = snprintf(body + pos, sizeof(body) - pos, "{\"applied\":[");
		pos += (w > 0 ? (size_t)w : 0);
		for (int i = 0; i < an; i++) {
			w = snprintf(body + pos, sizeof(body) - pos, "%s\"%s\"",
			             i ? "," : "", applied[i]);
			pos += (w > 0 ? (size_t)w : 0);
		}
		w = snprintf(body + pos, sizeof(body) - pos, "],\"errors\":[");
		pos += (w > 0 ? (size_t)w : 0);
		for (int i = 0; i < en; i++) {
			w = snprintf(body + pos, sizeof(body) - pos,
			             "%s{\"key\":\"", i ? "," : "");
			if (w > 0) pos += (size_t)w;
			int esc = json_escape(body, sizeof(body), pos, errors[i].name);
			if (esc > 0) pos += (size_t)esc;
			w = snprintf(body + pos, sizeof(body) - pos,
			             "\",\"reason\":\"%s\"}", errors[i].reason);
			pos += (w > 0 ? (size_t)w : 0);
		}
		w = snprintf(body + pos, sizeof(body) - pos, "],\"params\":");
		pos += (w > 0 ? (size_t)w : 0);
		w = params_to_json(snap->cfg, body + pos, sizeof(body) - pos);
		if (w > 0) pos += (size_t)w;
		w = snprintf(body + pos, sizeof(body) - pos, "}");
		pos += (w > 0 ? (size_t)w : 0);
		api_send(c->fd, 200, "application/json", body, (int)pos);

		if (an > 0)
			LOGV(cfg, "api: applied %d key(s) via /set", an);
		/* Cross-field consistency log. Per-field bounds are enforced by
		 * tunable_write(); this catches invariants spanning two fields
		 * (e.g. low must stay below high). We don't roll back — operator
		 * can fix with another /set — but we shout so it's not silent. */
		if (cfg->rssi_thresh_low >= cfg->rssi_thresh_high) {
			LOG("api: WARNING rssi_thresh_low (%.1f) >= rssi_thresh_high (%.1f) — bucket FSM will misbehave; fix with /set",
			    (double)cfg->rssi_thresh_low,
			    (double)cfg->rssi_thresh_high);
		}
		if (cfg->mcs_min > cfg->mcs_max) {
			LOG("api: WARNING mcs_min (%d) > mcs_max (%d) — clamp will pin to mcs_max; fix with /set",
			    cfg->mcs_min, cfg->mcs_max);
		}
	}
	else if (strcmp(path, "/status") == 0) {
		n = status_to_json(snap, body, sizeof(body));
		if (n > 0) api_send(c->fd, 200, "application/json", body, n);
		else       api_send(c->fd, 500, "text/plain", "json overflow\n", 14);
	}
	else if (strcmp(path, "/health") == 0) {
		api_send(c->fd, 200, "text/plain", "ok\n", 3);
	}
	else if (strcmp(path, "/events") == 0) {
		if (!g_api_clients) {
			api_send(c->fd, 503, "text/plain",
			         "sse: broadcast not initialized\n", 31);
			c->fd = -1;
			return;
		}
		if (api_count_sse(g_api_clients) >= API_MAX_SSE) {
			api_send(c->fd, 503, "text/plain",
			         "sse: too many subscribers\n", 26);
			c->fd = -1;
			return;
		}
		api_send_sse_headers(c->fd);
		c->sse = true;
		c->pos = 0;
		LOGV(cfg, "api: sse subscriber attached (fd=%d)", c->fd);
		return;
	}
	else {
		api_send(c->fd, 404, "text/plain", "no such route\n", 14);
	}
	c->fd = -1;
}

static void api_client_drain(ApiClient *c, Config *cfg, ApiSnapshot *snap)
{
	for (;;) {
		if (c->pos >= API_BUF_BYTES - 1) {
			api_send(c->fd, 400, "text/plain", "request too large\n", 18);
			c->fd = -1;
			return;
		}
		ssize_t r = recv(c->fd, c->buf + c->pos,
		                 API_BUF_BYTES - 1 - c->pos, MSG_DONTWAIT);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			api_client_drop(c);
			return;
		}
		if (r == 0) {
			api_client_drop(c);
			return;
		}
		c->pos += (size_t)r;
		c->buf[c->pos] = '\0';
		if (strstr(c->buf, "\r\n\r\n") || strstr(c->buf, "\n\n")) {
			api_handle_request(c, cfg, snap);
			return;
		}
	}
}

/* ── CLI / usage ─────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Endpoints:\n"
		"  --stats HOST:PORT     UDP listener for wfb_rx -Y JSON (default 127.0.0.1:5801)\n"
		"  --wfb HOST:PORT       wfb_tx control endpoint (default 127.0.0.1:8000)\n"
		"  --api-port N          HTTP API on 127.0.0.1:N (default 8766; 0 disables)\n"
		"\n"
		"Range presets (sets the three mcs_bucket_* tunables together):\n"
		"  --range low           bucket 0/1/2 = mcs 0/1/2\n"
		"  --range med           bucket 0/1/2 = mcs 1/2/3 (default)\n"
		"  --range high          bucket 0/1/2 = mcs 2/3/4\n"
		"\n"
		"Thresholds & deadband:\n"
		"  --rssi-low F          lower RSSI threshold dBm (default -70)\n"
		"  --rssi-high F         upper RSSI threshold dBm (default -50)\n"
		"  --deadband F          symmetric deadband dB (default 2.0)\n"
		"\n"
		"Smoothing & loss:\n"
		"  --rssi-alpha F        EWMA alpha for RSSI (default 0.3)\n"
		"  --loss-alpha F        EWMA alpha for loss ratio (default 0.5)\n"
		"  --aggregator best_avg|best_max|mean_avg  (default best_avg)\n"
		"  --loss-lost-pct F     dB penalty per %% of post-FEC lost (default 0.5)\n"
		"  --loss-recov-pct F    dB penalty per %% of fec_recovered (default 0.0;\n"
		"                          set non-zero to react to FEC consumption before\n"
		"                          actual data loss — usually unnecessary, since\n"
		"                          fec_controller already adapts k/n to that signal)\n"
		"  --loss-cap F          max loss penalty dB (default 20)\n"
		"\n"
		"Asymmetric gating (fast-down / slow-up):\n"
		"  --up-consec N         samples needed to commit up (default 3)\n"
		"  --down-consec N       samples needed to commit down (default 1)\n"
		"  --up-cooldown F       min seconds between up-commits (default 3.0)\n"
		"  --down-cooldown F     min seconds between down-commits (default 0.2)\n"
		"\n"
		"Failsafe & oscillation backoff:\n"
		"  --failsafe F          watchdog gap before forcing bucket 0 (default 0.5)\n"
		"  --recover-consec N    good samples before unfreezing (default 3)\n"
		"  --osc-window F        rolling window for change-rate (default 5.0)\n"
		"  --osc-threshold N     changes/window before backoff (default 4)\n"
		"  --osc-backoff F       up-cooldown multiplier (default 3.0)\n"
		"\n"
		"Behavior:\n"
		"  --start-low           force range[0] mcs at startup (default off — observe-then-act)\n"
		"  --dry-run             compute but do not send CMD_SET_RADIO\n"
		"  -v, --verbose         extra logs\n"
		"  -h, --help            this message\n",
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

static int parse_aggregator(const char *s)
{
	if (strcmp(s, "best_avg") == 0) return AGG_BEST_AVG;
	if (strcmp(s, "best_max") == 0) return AGG_BEST_MAX;
	if (strcmp(s, "mean_avg") == 0) return AGG_MEAN_AVG;
	return -1;
}

static int parse_range_preset(const char *s, Config *cfg)
{
	if (strcmp(s, "low") == 0)  { cfg->mcs_bucket_0 = 0; cfg->mcs_bucket_1 = 1; cfg->mcs_bucket_2 = 2; return 0; }
	if (strcmp(s, "med") == 0)  { cfg->mcs_bucket_0 = 1; cfg->mcs_bucket_1 = 2; cfg->mcs_bucket_2 = 3; return 0; }
	if (strcmp(s, "high") == 0) { cfg->mcs_bucket_0 = 2; cfg->mcs_bucket_1 = 3; cfg->mcs_bucket_2 = 4; return 0; }
	return -1;
}

static void config_defaults(Config *c)
{
	strcpy(c->stats_host, "127.0.0.1"); c->stats_port = 5801;
	strcpy(c->wfb_host,   "127.0.0.1"); c->wfb_port   = 8000;

	c->rssi_thresh_low  = -70.0f;
	c->rssi_thresh_high = -50.0f;
	c->rssi_deadband_db =  2.0f;

	c->rssi_ewma_alpha = 0.3f;
	c->loss_ewma_alpha = 0.5f;
	c->rssi_aggregator = AGG_BEST_AVG;

	c->loss_lost_penalty_db_per_pct      = 0.5f;
	c->loss_recovered_penalty_db_per_pct = 0.0f;   /* FEC working ≠ bad link */
	c->loss_penalty_max_db               = 20.0f;

	c->up_consecutive   = 3;
	c->down_consecutive = 1;
	c->up_cooldown_s    = 3.0f;
	c->down_cooldown_s  = 0.2f;

	c->failsafe_timeout_s            = 0.5f;
	c->failsafe_recovery_consecutive = 3;

	c->oscillation_window_s   = 5.0f;
	c->oscillation_threshold  = 4;
	c->oscillation_backoff    = 3.0f;

	/* Default to "med" range. */
	c->mcs_bucket_0 = 1;
	c->mcs_bucket_1 = 2;
	c->mcs_bucket_2 = 3;
	c->mcs_min = 0;
	c->mcs_max = 11;

	c->api_port = 8766;        /* one above fec_controller's 8765 */

	c->start_low = false;
	c->dry_run = false;
	c->verbose = 0;
}

/* ── Main loop ───────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	Config cfg;
	config_defaults(&cfg);

	enum {
		OPT_STATS = 256, OPT_WFB, OPT_API_PORT, OPT_RANGE,
		OPT_RSSI_LOW, OPT_RSSI_HIGH, OPT_DEADBAND,
		OPT_RSSI_ALPHA, OPT_LOSS_ALPHA, OPT_AGGREGATOR,
		OPT_LOSS_LOST_PCT, OPT_LOSS_RECOV_PCT, OPT_LOSS_CAP,
		OPT_UP_CONSEC, OPT_DOWN_CONSEC, OPT_UP_COOLDOWN, OPT_DOWN_COOLDOWN,
		OPT_FAILSAFE, OPT_RECOVER_CONSEC,
		OPT_OSC_WINDOW, OPT_OSC_THRESHOLD, OPT_OSC_BACKOFF,
		OPT_START_LOW, OPT_DRY_RUN,
	};
	static const struct option longopts[] = {
		{"stats",          required_argument, 0, OPT_STATS},
		{"wfb",            required_argument, 0, OPT_WFB},
		{"api-port",       required_argument, 0, OPT_API_PORT},
		{"range",          required_argument, 0, OPT_RANGE},
		{"rssi-low",       required_argument, 0, OPT_RSSI_LOW},
		{"rssi-high",      required_argument, 0, OPT_RSSI_HIGH},
		{"deadband",       required_argument, 0, OPT_DEADBAND},
		{"rssi-alpha",     required_argument, 0, OPT_RSSI_ALPHA},
		{"loss-alpha",     required_argument, 0, OPT_LOSS_ALPHA},
		{"aggregator",     required_argument, 0, OPT_AGGREGATOR},
		{"loss-lost-pct",  required_argument, 0, OPT_LOSS_LOST_PCT},
		{"loss-recov-pct", required_argument, 0, OPT_LOSS_RECOV_PCT},
		{"loss-cap",       required_argument, 0, OPT_LOSS_CAP},
		{"up-consec",      required_argument, 0, OPT_UP_CONSEC},
		{"down-consec",    required_argument, 0, OPT_DOWN_CONSEC},
		{"up-cooldown",    required_argument, 0, OPT_UP_COOLDOWN},
		{"down-cooldown",  required_argument, 0, OPT_DOWN_COOLDOWN},
		{"failsafe",       required_argument, 0, OPT_FAILSAFE},
		{"recover-consec", required_argument, 0, OPT_RECOVER_CONSEC},
		{"osc-window",     required_argument, 0, OPT_OSC_WINDOW},
		{"osc-threshold",  required_argument, 0, OPT_OSC_THRESHOLD},
		{"osc-backoff",    required_argument, 0, OPT_OSC_BACKOFF},
		{"start-low",      no_argument,       0, OPT_START_LOW},
		{"dry-run",        no_argument,       0, OPT_DRY_RUN},
		{"verbose",        no_argument,       0, 'v'},
		{"help",           no_argument,       0, 'h'},
		{0, 0, 0, 0},
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
		switch (ch) {
		case OPT_STATS:
			if (parse_hostport(optarg, cfg.stats_host,
			                   sizeof(cfg.stats_host), &cfg.stats_port) != 0) {
				fprintf(stderr, "invalid --stats\n"); return 1;
			}
			break;
		case OPT_WFB:
			if (parse_hostport(optarg, cfg.wfb_host,
			                   sizeof(cfg.wfb_host), &cfg.wfb_port) != 0) {
				fprintf(stderr, "invalid --wfb\n"); return 1;
			}
			break;
		case OPT_API_PORT: {
			int p = atoi(optarg);
			if (p < 0 || p > 65535) {
				fprintf(stderr, "invalid --api-port\n"); return 1;
			}
			cfg.api_port = p;
			break;
		}
		case OPT_RANGE:
			if (parse_range_preset(optarg, &cfg) != 0) {
				fprintf(stderr, "invalid --range (low|med|high)\n");
				return 1;
			}
			break;
		case OPT_RSSI_LOW:       cfg.rssi_thresh_low  = (float)atof(optarg); break;
		case OPT_RSSI_HIGH:      cfg.rssi_thresh_high = (float)atof(optarg); break;
		case OPT_DEADBAND:       cfg.rssi_deadband_db = (float)atof(optarg); break;
		case OPT_RSSI_ALPHA:     cfg.rssi_ewma_alpha  = (float)atof(optarg); break;
		case OPT_LOSS_ALPHA:     cfg.loss_ewma_alpha  = (float)atof(optarg); break;
		case OPT_AGGREGATOR: {
			int a = parse_aggregator(optarg);
			if (a < 0) {
				fprintf(stderr, "invalid --aggregator (best_avg|best_max|mean_avg)\n");
				return 1;
			}
			cfg.rssi_aggregator = a;
			break;
		}
		case OPT_LOSS_LOST_PCT:  cfg.loss_lost_penalty_db_per_pct      = (float)atof(optarg); break;
		case OPT_LOSS_RECOV_PCT: cfg.loss_recovered_penalty_db_per_pct = (float)atof(optarg); break;
		case OPT_LOSS_CAP:       cfg.loss_penalty_max_db               = (float)atof(optarg); break;
		case OPT_UP_CONSEC:      cfg.up_consecutive   = atoi(optarg); break;
		case OPT_DOWN_CONSEC:    cfg.down_consecutive = atoi(optarg); break;
		case OPT_UP_COOLDOWN:    cfg.up_cooldown_s    = (float)atof(optarg); break;
		case OPT_DOWN_COOLDOWN:  cfg.down_cooldown_s  = (float)atof(optarg); break;
		case OPT_FAILSAFE:       cfg.failsafe_timeout_s = (float)atof(optarg); break;
		case OPT_RECOVER_CONSEC: cfg.failsafe_recovery_consecutive = atoi(optarg); break;
		case OPT_OSC_WINDOW:     cfg.oscillation_window_s   = (float)atof(optarg); break;
		case OPT_OSC_THRESHOLD:  cfg.oscillation_threshold  = atoi(optarg); break;
		case OPT_OSC_BACKOFF:    cfg.oscillation_backoff    = (float)atof(optarg); break;
		case OPT_START_LOW:      cfg.start_low = true; break;
		case OPT_DRY_RUN:        cfg.dry_run   = true; break;
		case 'v':                cfg.verbose   = 1; break;
		case 'h': default: usage(argv[0]); return (ch == 'h') ? 0 : 1;
		}
	}

	if (cfg.rssi_thresh_low >= cfg.rssi_thresh_high) {
		fprintf(stderr, "rssi_thresh_low (%.1f) must be < rssi_thresh_high (%.1f)\n",
		        (double)cfg.rssi_thresh_low, (double)cfg.rssi_thresh_high);
		return 1;
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	log_init();
	LOG("stats=%s:%u wfb=%s:%u api=%d range=(%d,%d,%d) thresh=(%.1f/%.1f)±%.1f dry=%d",
	    cfg.stats_host, cfg.stats_port,
	    cfg.wfb_host,   cfg.wfb_port,
	    cfg.api_port,
	    cfg.mcs_bucket_0, cfg.mcs_bucket_1, cfg.mcs_bucket_2,
	    (double)cfg.rssi_thresh_low, (double)cfg.rssi_thresh_high,
	    (double)cfg.rssi_deadband_db,
	    cfg.dry_run ? 1 : 0);

	int stats_fd = stats_open_listener(cfg.stats_port);
	if (stats_fd < 0) {
		LOG("stats: bind on udp:%u failed (%s)",
		    cfg.stats_port, strerror(errno));
		return 1;
	}
	LOG("stats: listening on udp:%u for wfb_rx -Y JSON", cfg.stats_port);

	/* Bootstrap radiotap cache via CMD_GET_RADIO. We need every field
	 * except mcs_index preserved on subsequent SETs; without the cache
	 * we can't safely write. Failure is non-fatal but blocks emits
	 * until a successful GET — caller can retry by sending a SIGHUP-
	 * style restart, or wait for any external SET to drift through. */
	RadioBody radio = {0};
	bool radio_valid = false;
	if (wfb_get_radio(&cfg, &radio) == 0) {
		radio_valid = true;
		LOG("wfb_tx radio sync: bw=%d sgi=%d stbc=%d ldpc=%d mcs=%d vht=%d nss=%d",
		    radio.bandwidth, radio.short_gi, radio.stbc, radio.ldpc,
		    radio.mcs_index, radio.vht_mode, radio.vht_nss);
	} else {
		LOG("wfb_tx radio sync FAILED (is wfb_tx running on %s:%u?) — emits suppressed until GET succeeds",
		    cfg.wfb_host, cfg.wfb_port);
	}

	/* Optional safety boot SET. Off by default — operator opts in
	 * with --start-low. Avoids the "every restart drops bandwidth"
	 * surprise that the Python POC had. */
	if (cfg.start_low && radio_valid && !cfg.dry_run) {
		int target_mcs = mcs_for_bucket(0, &cfg);
		if (radio.mcs_index != target_mcs) {
			RadioBody r = radio;
			r.mcs_index = (uint8_t)target_mcs;
			if (wfb_set_radio(&cfg, &r) == 0) {
				radio = r;
				LOG("startup: forced mcs=%d (range[0]) per --start-low",
				    target_mcs);
			} else {
				LOG("startup: --start-low SET failed");
			}
		}
	}

	/* Optional HTTP API listener. Same pattern as fec_controller. */
	int api_fd = -1;
	ApiClient api_clients[API_MAX_CLIENTS];
	for (int i = 0; i < API_MAX_CLIENTS; i++) {
		api_clients[i].fd = -1;
		api_clients[i].sse = false;
		api_clients[i].pos = 0;
	}
	g_api_clients = api_clients;
	if (cfg.api_port > 0) {
		api_fd = api_listen_open((uint16_t)cfg.api_port);
		if (api_fd < 0) {
			LOG("api: bind on tcp:%d failed (%s); HTTP API disabled",
			    cfg.api_port, strerror(errno));
			cfg.api_port = 0;
		} else {
			LOG("api: listening on tcp:127.0.0.1:%d (GET / for help)",
			    cfg.api_port);
		}
	}
	uint64_t next_sse_ping_us = now_us() + 15000000ULL;

	Selector sel;
	selector_init(&sel);
	Scorer scorer;
	scorer_reset(&scorer);

	/* Snapshot fields exposed via /status. */
	float    last_eff_rssi    = 0.0f;
	float    last_lost_ratio  = 0.0f;
	float    last_recov_ratio = 0.0f;
	float    last_loss_pen_db = 0.0f;
	int      last_emit_mcs    = -1;
	uint64_t last_datagram_us = 0;

	/* Parse-error rate limiter: log first 5 then once per 30 s with
	 * the running total. Mirrors fec_controller's "tell me what changed,
	 * not every byte" ergonomic. */
	int      parse_errors = 0;
	uint64_t parse_errors_last_log_us = 0;
	const uint64_t PARSE_ERR_LOG_INTERVAL_US = 30000000ULL;

	/* Truncation watchdog: rx_ant datagrams larger than our recv buffer
	 * (2048 B) get silently chopped by recv(2). MSG_TRUNC reports the
	 * full length so we can detect this. Log first occurrence + a roll-up. */
	int      truncations = 0;
	uint64_t truncations_last_log_us = 0;

	/* SET / realign failure tracker: rising-edge log only.
	 *   sf_state = 0 idle, 1 = currently failing
	 * On transition idle→fail we log; on fail→fail we suppress; on
	 * fail→idle (recovery) we log. */
	int      sf_state = 0;
	uint64_t sf_first_us = 0;
	int      sf_target_mcs = -1;

	/* Heartbeat — every 5 s when we have valid state. */
	uint64_t next_heartbeat_us = now_us() + 5000000ULL;

	/* Watchdog tick — every quarter of failsafe_timeout_s, but no less
	 * than 50 ms. Drives selector_tick_no_data() so the failsafe trips
	 * even if rx_ant goes silent. */
	uint64_t next_watchdog_us = now_us() +
	    (uint64_t)((cfg.failsafe_timeout_s / 4.0f) * 1e6f);
	if (next_watchdog_us == 0) next_watchdog_us = now_us() + 50000;

	while (!g_stop) {
		uint64_t now = now_us();

		/* Watchdog tick. */
		if (now >= next_watchdog_us) {
			int new_mcs = -1;
			SelectDecision sd = selector_tick_no_data(&sel, &cfg, now, &new_mcs);
			if (sd == SD_FAILSAFE && radio_valid && !cfg.dry_run) {
				RadioBody r = radio;
				r.mcs_index = (uint8_t)new_mcs;
				if (wfb_set_radio(&cfg, &r) == 0) {
					radio = r;
					last_emit_mcs = new_mcs;
					if (sf_state) {
						LOG("SET_RADIO recovered (mcs=%d, %d.%03ds since first failure)",
						    new_mcs,
						    (int)((now - sf_first_us) / 1000000ULL),
						    (int)(((now - sf_first_us) / 1000ULL) % 1000));
						sf_state = 0;
					}
				} else {
					/* DELIBERATELY do NOT undo here. Failsafe is a
					 * safety-mode intent — keep sel.in_failsafe=true
					 * so /status reflects reality, the trip log won't
					 * re-fire (early-return path inside
					 * selector_tick_no_data), and recovery semantics
					 * stay alive. The realign block on the next
					 * datagram (or recovery once good samples arrive)
					 * retries the SET. */
					if (!sf_state || sf_target_mcs != new_mcs) {
						LOG("failsafe: SET_RADIO send failed (mcs=%d, wfb_tx still %d) — staying failsafed, retrying on next datagram",
						    new_mcs, radio.mcs_index);
						sf_state = 1;
						sf_first_us = now;
						sf_target_mcs = new_mcs;
					}
				}
			} else if (sd == SD_FAILSAFE) {
				last_emit_mcs = new_mcs;
			}
			float period_s = cfg.failsafe_timeout_s / 4.0f;
			if (period_s < 0.05f) period_s = 0.05f;
			next_watchdog_us = now + (uint64_t)(period_s * 1e6f);
		}

		/* Heartbeat. */
		if (now >= next_heartbeat_us) {
			if (sel.current_bucket >= 0) {
				char divergence[32] = "";
				if (radio_valid && sel.current_mcs >= 0 &&
				    radio.mcs_index != (uint8_t)sel.current_mcs) {
					snprintf(divergence, sizeof(divergence),
					         " WFB_MCS=%d!", radio.mcs_index);
				}
				LOG("hb: bucket=%d mcs=%d eff=%.1f lost=%.2f%% recov=%.1f%% pen=%.1fdB commits=%u%s%s",
				    sel.current_bucket, sel.current_mcs,
				    (double)last_eff_rssi,
				    (double)(last_lost_ratio * 100.0f),
				    (double)(last_recov_ratio * 100.0f),
				    (double)last_loss_pen_db,
				    sel.commit_count,
				    sel.in_failsafe ? " FAILSAFE" : "",
				    divergence);
			} else {
				/* No datagram has ever arrived. Operator visibility:
				 * tell them WHERE we're listening so the wiring can be
				 * checked without enabling -v. */
				LOG("hb: waiting for rx_ant on udp:%u (no datagram yet, %.1fs since boot)",
				    cfg.stats_port, log_rel_s());
			}
			next_heartbeat_us = now + 5000000ULL;
		}

		/* Periodic parse-error rollup. */
		if (parse_errors > 0 &&
		    now - parse_errors_last_log_us >= PARSE_ERR_LOG_INTERVAL_US) {
			LOG("rx_ant: %d parse errors since last report (last %ds)",
			    parse_errors, (int)(PARSE_ERR_LOG_INTERVAL_US / 1000000ULL));
			parse_errors = 0;
			parse_errors_last_log_us = now;
		}

		/* Build pollfd set. */
		struct pollfd pfds[2 + API_MAX_CLIENTS];
		int npfds = 0;
		int stats_idx = -1, api_listen_idx = -1;
		int api_client_idx[API_MAX_CLIENTS];
		for (int i = 0; i < API_MAX_CLIENTS; i++) api_client_idx[i] = -1;

		pfds[npfds].fd = stats_fd;
		pfds[npfds].events = POLLIN;
		stats_idx = npfds++;
		if (api_fd >= 0) {
			pfds[npfds].fd = api_fd;
			pfds[npfds].events = POLLIN;
			api_listen_idx = npfds++;
			for (int i = 0; i < API_MAX_CLIENTS; i++) {
				if (api_clients[i].fd >= 0) {
					pfds[npfds].fd = api_clients[i].fd;
					pfds[npfds].events = POLLIN;
					api_client_idx[i] = npfds++;
				}
			}
		}

		/* Wait for the soonest of: stats / API I/O / next watchdog tick. */
		uint64_t next_deadline = next_watchdog_us;
		if (next_heartbeat_us < next_deadline) next_deadline = next_heartbeat_us;
		int timeout_ms = 50;
		if (next_deadline > now) {
			uint64_t ahead = (next_deadline - now) / 1000ULL;
			if (ahead < (uint64_t)timeout_ms) timeout_ms = (int)ahead + 1;
		} else {
			timeout_ms = 0;
		}
		int pr = poll(pfds, npfds, timeout_ms);
		if (pr < 0) { if (errno == EINTR) continue; perror("poll"); break; }

		/* --- Service HTTP API first --- */
		if (api_listen_idx >= 0 && pr > 0 &&
		    (pfds[api_listen_idx].revents & POLLIN)) {
			for (;;) {
				struct sockaddr_in peer;
				socklen_t plen = sizeof(peer);
				int cfd = accept(api_fd, (struct sockaddr *)&peer, &plen);
				if (cfd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) break;
					break;
				}
				int fl = fcntl(cfd, F_GETFL, 0);
				if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
				if (api_client_alloc(api_clients, cfd) < 0) {
					api_send(cfd, 503, "text/plain",
					         "no free api slot\n", 17);
				}
			}
		}
		if (api_listen_idx >= 0) {
			ApiSnapshot snap = {
				.cfg = &cfg, .sel = &sel, .radio = &radio,
				.radio_valid = radio_valid,
				.last_datagram_us = last_datagram_us,
				.last_eff_rssi = last_eff_rssi,
				.last_lost_ratio = last_lost_ratio,
				.last_recov_ratio = last_recov_ratio,
				.last_loss_penalty_db = last_loss_pen_db,
				.last_emit_mcs = last_emit_mcs,
			};
			for (int i = 0; i < API_MAX_CLIENTS; i++) {
				if (api_client_idx[i] < 0) continue;
				if (api_clients[i].fd < 0) continue;
				if (api_clients[i].sse) {
					if (pfds[api_client_idx[i]].revents & (POLLHUP | POLLERR))
						api_client_drop(&api_clients[i]);
					continue;
				}
				if (pfds[api_client_idx[i]].revents & (POLLIN | POLLHUP | POLLERR))
					api_client_drain(&api_clients[i], &cfg, &snap);
			}
			uint64_t now2 = now_us();
			for (int i = 0; i < API_MAX_CLIENTS; i++) {
				if (api_clients[i].fd >= 0 && !api_clients[i].sse &&
				    now2 - api_clients[i].accepted_us > 2000000ULL) {
					api_client_drop(&api_clients[i]);
				}
			}
			if (now2 >= next_sse_ping_us) {
				sse_heartbeat_all();
				next_sse_ping_us = now2 + 15000000ULL;
			}
		}

		/* --- Drain rx_ant datagrams --- */
		if (stats_idx >= 0 && pr > 0 && (pfds[stats_idx].revents & POLLIN)) {
			char dgram[2048];
			for (;;) {
				/* MSG_TRUNC reports the FULL incoming length even when
				 * recv truncates into our buffer — lets us detect future
				 * schema growth past 2 KB without silently corrupting. */
				ssize_t n = recv(stats_fd, dgram, sizeof(dgram) - 1,
				                 MSG_TRUNC);
				if (n <= 0) break;
				if ((size_t)n >= sizeof(dgram)) {
					truncations++;
					if (truncations == 1 ||
					    now - truncations_last_log_us >= PARSE_ERR_LOG_INTERVAL_US) {
						LOG("rx_ant: datagram truncated (%zd bytes > %zu buffer) — increase dgram[] in source",
						    n, sizeof(dgram) - 1);
						truncations_last_log_us = now;
					}
					continue;  /* truncated payload is unsafe to parse */
				}
				dgram[n] = '\0';

				/* Filter: must look like an rx_ant ver 1 record. The
				 * forward-compatible parser ignores other "type" or
				 * other "ver" silently. */
				if (!strstr(dgram, "\"type\":\"rx_ant\"")) continue;
				if (!strstr(dgram, "\"ver\":1")) continue;

				long pkt_uniq = -1, pkt_data = -1, pkt_lost = 0, pkt_fec_recovered = 0;
				const char *pkt_block = strstr(dgram, "\"pkt\":");
				if (!pkt_block) {
					parse_errors++;
					if (parse_errors <= 5)
						LOG("rx_ant: missing pkt block");
					continue;
				}
				/* Prefer uniq (post-dedup) over data (per-antenna sum). */
				(void)json_get_int_in(pkt_block, "uniq", &pkt_uniq);
				(void)json_get_int_in(pkt_block, "data", &pkt_data);
				if (pkt_uniq < 0) pkt_uniq = pkt_data;  /* fall back */
				if (pkt_uniq < 0) {
					parse_errors++;
					if (parse_errors <= 5)
						LOG("rx_ant: missing both pkt.uniq and pkt.data");
					continue;
				}
				(void)json_get_int_in(pkt_block, "lost", &pkt_lost);
				(void)json_get_int_in(pkt_block, "fec_recovered", &pkt_fec_recovered);

				Score score;
				if (!scorer_update(&scorer, &cfg, dgram,
				                   pkt_uniq, pkt_lost, pkt_fec_recovered,
				                   &score))
					continue;

				last_datagram_us = now_us();
				last_eff_rssi    = score.effective_rssi;
				last_lost_ratio  = score.smoothed_lost_ratio;
				last_recov_ratio = score.smoothed_recov_ratio;
				last_loss_pen_db = score.loss_penalty_db;

				int new_mcs = -1;
				Selector sel_snap = sel;       /* undo if SET fails below */
				SelectDecision sd =
				    selector_update(&sel, &cfg, &score, last_datagram_us,
				                    &new_mcs);
				if (sd == SD_NONE) {
					/* No bucket transition — but did the bucket→mcs
					 * mapping shift under us (e.g. operator /set the
					 * mcs_bucket_X tunables, or external party changed
					 * wfb_tx mcs)? Realign without burning a commit:
					 * the FSM's current_bucket is still authoritative,
					 * we just refresh current_mcs and resend the SET.
					 *
					 * Realign deliberately bypasses up_cooldown_s — the
					 * operator just told us the new mapping, and gating
					 * a manual change on the algorithmic cooldown would
					 * be surprising. Same reason we don't touch
					 * last_change_us here. */
					if (sel.current_bucket >= 0 && radio_valid &&
					    !cfg.dry_run) {
						int want = mcs_for_bucket(sel.current_bucket, &cfg);
						if (want != (int)radio.mcs_index) {
							/* First-occurrence log only (rising edge of
							 * "want != have"). After a successful SET the
							 * condition clears; if SET keeps failing the
							 * tracker below silences the spam. */
							if (!sf_state || sf_target_mcs != want) {
								LOG("realign: bucket=%d want_mcs=%d wfb_mcs=%d",
								    sel.current_bucket, want, radio.mcs_index);
							}
							RadioBody r2 = radio;
							r2.mcs_index = (uint8_t)want;
							if (wfb_set_radio(&cfg, &r2) == 0) {
								radio = r2;
								sel.current_mcs = want;
								last_emit_mcs = want;
								if (sf_state) {
									LOG("realign: recovered (mcs=%d, %d.%03ds since first failure)",
									    want,
									    (int)((now_us() - sf_first_us) / 1000000ULL),
									    (int)(((now_us() - sf_first_us) / 1000ULL) % 1000));
									sf_state = 0;
								}
							} else {
								if (!sf_state || sf_target_mcs != want) {
									LOG("realign: SET_RADIO failed (mcs=%d) — retrying silently until recovery",
									    want);
									sf_state = 1;
									sf_first_us = now_us();
									sf_target_mcs = want;
								}
							}
						}
					}
					continue;
				}

				LOG("decision: %s bucket=%d mcs=%d eff=%.1f smooth=%.1f raw=%.1f lost=%.2f%% recov=%.1f%% pen=%.1fdB%s",
				    sd_name(sd), sel.current_bucket, sel.current_mcs,
				    (double)score.effective_rssi,
				    (double)score.smoothed_rssi,
				    (double)score.raw_rssi,
				    (double)(score.smoothed_lost_ratio * 100.0f),
				    (double)(score.smoothed_recov_ratio * 100.0f),
				    (double)score.loss_penalty_db,
				    selector_is_oscillating(&sel, &cfg) ? " [osc]" : "");

				if (cfg.dry_run) {
					last_emit_mcs = new_mcs;
					continue;
				}
				if (!radio_valid) {
					/* Lazy retry GET so the controller can recover from
					 * a wfb_tx that wasn't running at startup. */
					if (wfb_get_radio(&cfg, &radio) == 0) {
						radio_valid = true;
						LOG("wfb_tx radio sync: bw=%d sgi=%d stbc=%d ldpc=%d mcs=%d vht=%d nss=%d",
						    radio.bandwidth, radio.short_gi, radio.stbc,
						    radio.ldpc, radio.mcs_index, radio.vht_mode,
						    radio.vht_nss);
					} else {
						/* Roll back so next sample retries instead of
						 * sitting with a phantom commit. */
						sel = sel_snap;
						LOG("emit suppressed: radio cache invalid (GET failed) — undoing commit");
						continue;
					}
				}
				RadioBody r = radio;
				r.mcs_index = (uint8_t)new_mcs;
				if (wfb_set_radio(&cfg, &r) != 0) {
					sel = sel_snap;
					if (!sf_state || sf_target_mcs != new_mcs) {
						LOG("SET_RADIO failed (selector wanted mcs=%d, wfb_tx still %d) — undoing commit, retrying silently until recovery",
						    new_mcs, radio.mcs_index);
						sf_state = 1;
						sf_first_us = now_us();
						sf_target_mcs = new_mcs;
					}
					continue;
				}
				if (sf_state) {
					LOG("SET_RADIO recovered (mcs=%d, %d.%03ds since first failure)",
					    new_mcs,
					    (int)((now_us() - sf_first_us) / 1000000ULL),
					    (int)(((now_us() - sf_first_us) / 1000ULL) % 1000));
					sf_state = 0;
				}
				/* Read back so subsequent SETs reflect what wfb_tx
				 * actually latched (e.g. operator changed bandwidth
				 * out-of-band). On read-back failure keep optimistic
				 * cache (the SET acked rc=0). */
				RadioBody fresh;
				if (wfb_get_radio(&cfg, &fresh) == 0) radio = fresh;
				else                                   radio = r;
				last_emit_mcs = new_mcs;
			}
		}
	}

	if (stats_fd >= 0) close(stats_fd);
	if (api_fd >= 0) close(api_fd);
	g_api_clients = NULL;
	for (int i = 0; i < API_MAX_CLIENTS; i++)
		if (api_clients[i].fd >= 0) close(api_clients[i].fd);
	LOG("stopped after %u commits", sel.commit_count);
	return 0;
}
