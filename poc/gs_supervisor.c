/*
 * gs_supervisor — ground-side wfb supervisor (scaffolding).
 *
 * Single native binary that fork/execs N×wfb_rx and N×wfb_tx children
 * from one JSON config and exposes a REST API for lifecycle control.
 * Replaces the wfb-ng.sh / S99wfb / wbmode / wfb_cmd sprawl on the
 * ground side. See GS_SUPERVISOR.md for the full design.
 *
 * Asymmetric to vehicle by design — the vehicle keeps link_controller
 * as the single venc-driven control loop; the ground gets dumb pipes.
 *
 * SCAFFOLDING SCOPE (this file)
 *   - Minimal hand-rolled JSON parser (jsmn-shaped, zero alloc)
 *   - Tunnel model with N interfaces per tunnel (diversity rx,
 *     mirror-mode tx) and arbitrary tunnel count
 *   - fork/exec children with composed argv; SIGCHLD-driven respawn
 *     with bounded exponential backoff if autostart is set
 *   - REST: GET /api/v1/health
 *           GET /api/v1/status
 *           GET /api/v1/tunnels
 *           GET /api/v1/tunnels/{name}
 *           GET /api/v1/tunnels/{name}/start
 *           GET /api/v1/tunnels/{name}/stop
 *           GET /api/v1/tunnels/{name}/restart
 *   - Clean shutdown on SIGTERM/SIGINT (children get SIGTERM → SIGKILL
 *     after grace, then system.down commands run)
 *
 * TODO (follow-up commits)
 *   - wfb_cmd passthrough (POST /api/v1/tunnels/{name}/control)
 *   - WCMD emit (POST /api/v1/cmd) using wcmd_proto.h
 *   - System up/down whitelisted commands (monitor-mode bring-up;
 *     exact iw/ip incantations to be added on device)
 *   - SSE event stream + per-child stdout/stderr ring buffers
 *   - Embedded WebUI (poc/webui/gs.html via xxd -i, like link_controller)
 *   - Config reload (POST /api/v1/reload — diff & re-apply)
 *
 * Style matches link_controller.c: single-thread poll() loop, libc only,
 * GET-based API. No pthreads, no external deps.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- compile-time limits (small, predictable) ----------------- */

#define GS_MAX_TUNNELS       8
#define GS_MAX_IFACES        4
#define GS_MAX_EXTRA_ARGS   16
#define GS_MAX_SYSTEM_CMDS  32
#define GS_NAME_MAX         32
#define GS_PATH_MAX        256
#define GS_ARG_MAX         128
#define GS_ARGV_MAX         64
#define GS_ARGV_BUF       1536

#define API_MAX_CLIENTS      8
#define API_BUF_BYTES     2048

#define GS_DEFAULT_HTTP_PORT 9080
#define GS_DEFAULT_CONFIG    "/etc/waybeam/gs_supervisor.json"

/* respawn backoff schedule (ms). Capped — if we exhaust it we stay at the
 * last entry forever. Matches procd ratelimit semantics. */
static const int GS_BACKOFF_MS[] = { 500, 1000, 2000, 4000, 8000, 16000, 30000 };
#define GS_BACKOFF_LEN ((int)(sizeof(GS_BACKOFF_MS) / sizeof(GS_BACKOFF_MS[0])))

/* SIGTERM-then-SIGKILL grace period when stopping a child. */
#define GS_STOP_GRACE_MS  1500

/* ---------- tiny utils ----------------------------------------------- */

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_sigchld  = 0;
static int                   g_verbose  = 0;   /* -v: full info + inherit child stdout/stderr */

static void on_signal(int sig)
{
	if (sig == SIGCHLD) g_sigchld = 1;
	else                g_shutdown = 1;
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static uint64_t now_ms(void) { return now_us() / 1000; }

static void logf_(const char *level, const char *fmt, ...)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	char hdr[64];
	strftime(hdr, sizeof(hdr), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s.%03ld [gs/%s] ", hdr, ts.tv_nsec / 1000000, level);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define LOG_INFO(...)  logf_("info",  __VA_ARGS__)
#define LOG_WARN(...)  logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)   logf_("err",   __VA_ARGS__)
/* LOG_DEBUG silently dropped unless -v was passed. Use for per-event noise
 * (argv dumps, respawn attempts) that's only useful when troubleshooting. */
#define LOG_DEBUG(...) do { if (g_verbose) logf_("debug", __VA_ARGS__); } while (0)

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p   += w;
		len -= (size_t)w;
	}
	return 0;
}

/* ---------- minimal JSON parser (jsmn-shaped) ------------------------ *
 *
 * One-shot tokenizer. Produces a flat array of tokens with parent links;
 * traversal helpers walk the array. Zero allocations, no recursion in the
 * parser (state machine + stack-of-containers stored in token parent
 * field). Just enough for our config schema.
 *
 * Strings are NOT unescaped — we only use plain ASCII keys/values. If we
 * ever need full string handling we'll vendor jsmn properly.
 */

typedef enum { JT_NONE = 0, JT_OBJ, JT_ARR, JT_STR, JT_PRIM } JTokType;

typedef struct {
	JTokType type;
	int      start;   /* offset in source */
	int      end;     /* one past last char */
	int      size;    /* children for OBJ/ARR; 0 otherwise */
	int      parent;  /* index in token array, -1 for root */
} JTok;

#define JSON_MAX_TOKS 512

typedef struct {
	const char *js;
	int         len;
	int         pos;
	JTok       *toks;
	int         tok_max;
	int         tok_count;
	int         super;   /* index of current container */
} JParser;

static JTok *json_alloc(JParser *p)
{
	if (p->tok_count >= p->tok_max) return NULL;
	JTok *t = &p->toks[p->tok_count++];
	t->type = JT_NONE; t->start = -1; t->end = -1; t->size = 0; t->parent = -1;
	return t;
}

static int json_parse_prim(JParser *p)
{
	int start = p->pos;
	for (; p->pos < p->len; p->pos++) {
		char c = p->js[p->pos];
		if (c == ',' || c == '}' || c == ']' ||
		    c == ' ' || c == '\t' || c == '\n' || c == '\r')
			break;
	}
	JTok *t = json_alloc(p);
	if (!t) return -1;
	t->type = JT_PRIM; t->start = start; t->end = p->pos; t->parent = p->super;
	if (p->super >= 0) p->toks[p->super].size++;
	p->pos--;
	return 0;
}

static int json_parse_str(JParser *p)
{
	int start = ++p->pos;  /* skip opening quote */
	for (; p->pos < p->len; p->pos++) {
		char c = p->js[p->pos];
		if (c == '"') {
			JTok *t = json_alloc(p);
			if (!t) return -1;
			t->type = JT_STR; t->start = start; t->end = p->pos; t->parent = p->super;
			if (p->super >= 0) p->toks[p->super].size++;
			return 0;
		}
		if (c == '\\' && p->pos + 1 < p->len) p->pos++;  /* skip escape */
	}
	return -1;
}

static int json_parse(const char *js, int len, JTok *toks, int max)
{
	JParser p = { .js = js, .len = len, .pos = 0,
	              .toks = toks, .tok_max = max, .tok_count = 0, .super = -1 };
	for (; p.pos < p.len; p.pos++) {
		char c = p.js[p.pos];
		if (c == '{' || c == '[') {
			JTok *t = json_alloc(&p);
			if (!t) return -1;
			t->type = (c == '{') ? JT_OBJ : JT_ARR;
			t->start = p.pos; t->end = -1; t->parent = p.super;
			if (p.super >= 0) p.toks[p.super].size++;
			p.super = p.tok_count - 1;
		} else if (c == '}' || c == ']') {
			JTokType expect = (c == '}') ? JT_OBJ : JT_ARR;
			if (p.super < 0) return -1;
			if (p.toks[p.super].type != expect) return -1;
			p.toks[p.super].end = p.pos + 1;
			/* unwind */
			int s = p.super;
			p.super = p.toks[s].parent;
			while (p.super >= 0 && p.toks[p.super].end != -1)
				p.super = p.toks[p.super].parent;
		} else if (c == '"') {
			if (json_parse_str(&p) < 0) return -1;
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
		           c == ':' || c == ',') {
			/* skip */
		} else {
			if (json_parse_prim(&p) < 0) return -1;
		}
	}
	if (p.super != -1) return -1;
	return p.tok_count;
}

static bool jeq(const char *js, const JTok *t, const char *s)
{
	if (t->type != JT_STR) return false;
	int slen = (int)strlen(s);
	return (t->end - t->start) == slen && strncmp(js + t->start, s, (size_t)slen) == 0;
}

/* Advance past a token + all its descendants. Returns next index.
 * NOTE: token .size counts each direct child token. For OBJ that means
 * 2 × pair_count (one for the key string, one for the value). */
static int jskip(const JTok *toks, int n, int i)
{
	if (i >= n) return n;
	if (toks[i].type == JT_OBJ) {
		int pairs = toks[i].size / 2;
		int j = i + 1;
		while (pairs-- > 0) {
			j = jskip(toks, n, j);     /* key */
			j = jskip(toks, n, j);     /* value */
		}
		return j;
	}
	if (toks[i].type == JT_ARR) {
		int kids = toks[i].size;
		int j = i + 1;
		while (kids-- > 0) j = jskip(toks, n, j);
		return j;
	}
	return i + 1;
}

/* Find a child of object `obj_idx` by key. Returns idx of value, or -1. */
static int jfind(const char *js, const JTok *toks, int n, int obj_idx, const char *key)
{
	if (obj_idx < 0 || obj_idx >= n || toks[obj_idx].type != JT_OBJ) return -1;
	int pairs = toks[obj_idx].size / 2;
	int i = obj_idx + 1;
	for (int k = 0; k < pairs; k++) {
		if (jeq(js, &toks[i], key)) return i + 1;
		i = jskip(toks, n, i);     /* key */
		i = jskip(toks, n, i);     /* value */
	}
	return -1;
}

static int jstr(const char *js, const JTok *t, char *out, size_t cap)
{
	if (!t || t->type != JT_STR) { out[0] = 0; return -1; }
	int slen = t->end - t->start;
	if (slen < 0 || (size_t)slen >= cap) slen = (int)cap - 1;
	memcpy(out, js + t->start, (size_t)slen);
	out[slen] = 0;
	return slen;
}

static int jint(const char *js, const JTok *t, long *out)
{
	if (!t || t->type != JT_PRIM) return -1;
	char buf[32];
	int slen = t->end - t->start;
	if (slen <= 0 || slen >= (int)sizeof(buf)) return -1;
	memcpy(buf, js + t->start, (size_t)slen);
	buf[slen] = 0;
	char *end = NULL;
	long v = strtol(buf, &end, 10);
	if (!end || *end) return -1;
	*out = v;
	return 0;
}

static int jbool(const char *js, const JTok *t, bool *out)
{
	if (!t || t->type != JT_PRIM) return -1;
	int slen = t->end - t->start;
	if (slen == 4 && !strncmp(js + t->start, "true",  4)) { *out = true;  return 0; }
	if (slen == 5 && !strncmp(js + t->start, "false", 5)) { *out = false; return 0; }
	return -1;
}

/* ---------- config + tunnel model ------------------------------------ */

typedef enum {
	TS_STOPPED = 0,   /* never started, or explicitly stopped via REST */
	TS_STARTING,      /* fork/exec just issued */
	TS_RUNNING,       /* alive */
	TS_BACKOFF,       /* exited unexpectedly, waiting to respawn */
	TS_FAILED,        /* exec failed or backoff exhausted in a future revision */
} TunnelState;

typedef struct {
	char        name[GS_NAME_MAX];
	char        role[4];               /* "rx" or "tx" */
	char        binary[GS_PATH_MAX];   /* override executable; "" = wfb_rx / wfb_tx */
	int         link_id;
	int         radio_port;

	int         iface_count;
	char        ifaces[GS_MAX_IFACES][IFNAMSIZ];

	/* rx-only */
	char        udp_out_ip[GS_ARG_MAX]; /* "" = leave -c/-u to wfb_rx defaults */
	int         udp_out_port;
	char        stats_out[GS_ARG_MAX]; /* "ip:port" or empty */

	/* tx-only */
	int         udp_in_port;
	int         control_port;
	int         fec_k, fec_n;
	int         bandwidth_mhz;
	int         mcs_index;             /* -1 = unset */
	int         stbc;                  /* -1 = unset */
	int         ldpc;                  /* -1 = unset */

	int         extra_arg_count;
	char        extra_args[GS_MAX_EXTRA_ARGS][GS_ARG_MAX];

	bool        autostart;

	/* runtime state (mutable) */
	TunnelState state;
	pid_t       pid;
	uint64_t    started_us;
	uint64_t    exited_us;
	int         exit_code;             /* WEXITSTATUS, or -signo if killed */
	int         restart_count;
	int         backoff_idx;
	uint64_t    next_start_ms;         /* 0 = ASAP */
	bool        autostart_on_exit;     /* cleared by explicit /stop */
	uint64_t    stop_deadline_ms;      /* SIGTERM→SIGKILL transition */

	/* rx-only stats listener.  We always bind a local UDP port and pass it
	 * to wfb_rx -Y so the supervisor can surface rx_ant counters in the
	 * Tunnels WebUI.  If the user configured a `stats_out` for downstream
	 * consumers (e.g. uplink TX → vehicle), supervisor re-emits each
	 * datagram to that target verbatim — preserving the existing pipeline.
	 *
	 *   wfb_rx ── -Y 127.0.0.1:<stats_local_port> ──▶ supervisor
	 *                                                  │
	 *                                                  ├─▶ parse → Tunnel.st_*
	 *                                                  └─▶ stats_fwd_addr (re-emit)
	 */
	int                 stats_local_fd;
	uint16_t            stats_local_port;
	struct sockaddr_in  stats_fwd_addr;
	int                 stats_fwd_active;
	char                stats_local_arg[GS_ARG_MAX]; /* "127.0.0.1:port" passed to -Y */

	/* parsed rx_ant / tx_stats snapshot
	 *
	 * rx_ant fields (rx role):     st_pkt_*, st_ant_count, st_rssi_best
	 * tx_stats fields (tx role):   st_tx_pkts_in/out, st_tx_bytes_*,
	 *                              st_tx_drop, st_tx_trunc, st_tx_fec_timeouts
	 * shared fields:               st_first_us, st_last_us, st_interval_ms,
	 *                              st_msg_count
	 *
	 * tx_stats also carries fec.k/fec.n/mcs/bw — those land in the
	 * radio_cache / fec_cache fields directly so the WebUI's TX panel
	 * doesn't need a separate get_radio / get_fec round-trip on every
	 * change. */
	uint64_t    st_first_us;
	uint64_t    st_last_us;
	uint32_t    st_interval_ms;
	uint32_t    st_msg_count;

	/* rx_ant decoder counters */
	uint32_t    st_pkt_all;
	uint32_t    st_pkt_lost;
	uint32_t    st_pkt_fec;
	uint32_t    st_pkt_outgoing;
	uint32_t    st_pkt_dec_err;
	uint32_t    st_pkt_bytes;
	uint32_t    st_pkt_uniq;
	int         st_ant_count;
	int         st_rssi_best;          /* highest avg over antennas */

	/* tx_stats encoder counters */
	uint32_t    st_tx_pkts_in;
	uint32_t    st_tx_pkts_out;
	uint32_t    st_tx_bytes_in;
	uint32_t    st_tx_bytes_out;
	uint32_t    st_tx_drop;
	uint32_t    st_tx_trunc;
	uint32_t    st_tx_fec_timeouts;

	/* tx-only runtime cache (echoes wfb_tx live state via wfb_cmd) */
	int         short_gi;              /* -1 = unset */
	int         vht_mode;
	int         vht_nss;
	int         fec_timeout_ms;        /* runtime, -1 = unknown */
	int         radio_cache_have;
	int         fec_cache_have;
	uint64_t    radio_cache_us;
	uint64_t    tx_init_query_after_us; /* schedule a get_radio/get_fec */
} Tunnel;

typedef struct {
	char     key_file[GS_PATH_MAX];
	char     http_bind[64];
	uint16_t http_port;

	int      system_up_count;
	char     system_up[GS_MAX_SYSTEM_CMDS][GS_PATH_MAX];
	int      system_down_count;
	char     system_down[GS_MAX_SYSTEM_CMDS][GS_PATH_MAX];

	int      tunnel_count;
	Tunnel   tunnels[GS_MAX_TUNNELS];

	bool     venc_cmd_enabled;
	char     venc_cmd_uplink[GS_NAME_MAX];
	int      venc_cmd_rate_limit_ms;
} Config;

static const char *tunnel_state_name(TunnelState s)
{
	switch (s) {
	case TS_STOPPED:  return "stopped";
	case TS_STARTING: return "starting";
	case TS_RUNNING:  return "running";
	case TS_BACKOFF:  return "backoff";
	case TS_FAILED:   return "failed";
	}
	return "?";
}

/* ---------- config loader -------------------------------------------- */

static int cfg_parse_tunnel(const char *js, JTok *toks, int n, int t_idx, Tunnel *t)
{
	memset(t, 0, sizeof(*t));
	t->mcs_index = -1; t->stbc = -1; t->ldpc = -1;
	t->short_gi = -1; t->vht_mode = -1; t->vht_nss = -1;
	t->fec_timeout_ms = -1;
	t->stats_local_fd = -1;
	t->st_rssi_best = INT_MIN;
	t->autostart = true;

	int v;
	long lv;
	bool bv;
	char tmp[GS_ARG_MAX];

	if ((v = jfind(js, toks, n, t_idx, "name")) < 0) return -1;
	if (jstr(js, &toks[v], t->name, sizeof(t->name)) < 0) return -1;

	if ((v = jfind(js, toks, n, t_idx, "role")) < 0) return -1;
	if (jstr(js, &toks[v], t->role, sizeof(t->role)) < 0) return -1;
	if (strcmp(t->role, "rx") && strcmp(t->role, "tx")) return -1;

	if ((v = jfind(js, toks, n, t_idx, "link_id")) < 0) return -1;
	if (jint(js, &toks[v], &lv) < 0) return -1;
	t->link_id = (int)lv;

	if ((v = jfind(js, toks, n, t_idx, "radio_port")) >= 0) {
		if (jint(js, &toks[v], &lv) < 0) return -1;
		t->radio_port = (int)lv;
	}

	v = jfind(js, toks, n, t_idx, "interfaces");
	if (v < 0 || toks[v].type != JT_ARR) return -1;
	int kids = toks[v].size;
	if (kids < 1 || kids > GS_MAX_IFACES) return -1;
	int ci = v + 1;
	for (int k = 0; k < kids; k++) {
		if (jstr(js, &toks[ci], t->ifaces[k], IFNAMSIZ) < 0) return -1;
		ci = jskip(toks, n, ci);
	}
	t->iface_count = kids;

	if ((v = jfind(js, toks, n, t_idx, "extra_args")) >= 0 && toks[v].type == JT_ARR) {
		kids = toks[v].size;
		if (kids > GS_MAX_EXTRA_ARGS) return -1;
		ci = v + 1;
		for (int k = 0; k < kids; k++) {
			if (jstr(js, &toks[ci], t->extra_args[k], GS_ARG_MAX) < 0) return -1;
			ci = jskip(toks, n, ci);
		}
		t->extra_arg_count = kids;
	}

	if ((v = jfind(js, toks, n, t_idx, "autostart")) >= 0) {
		if (jbool(js, &toks[v], &bv) < 0) return -1;
		t->autostart = bv;
	}

	if ((v = jfind(js, toks, n, t_idx, "binary")) >= 0) {
		if (jstr(js, &toks[v], t->binary, sizeof(t->binary)) < 0) return -1;
	}

	if (!strcmp(t->role, "rx")) {
		/* udp_out is optional — wfb_rx defaults to 127.0.0.1:5600. */
		if ((v = jfind(js, toks, n, t_idx, "udp_out")) >= 0) {
			if (jstr(js, &toks[v], tmp, sizeof(tmp)) < 0) return -1;
			char *colon = strchr(tmp, ':');
			if (!colon) return -1;
			*colon = 0;
			snprintf(t->udp_out_ip, sizeof(t->udp_out_ip), "%s", tmp);
			t->udp_out_port = atoi(colon + 1);
			if (t->udp_out_port <= 0 || t->udp_out_port > 65535) return -1;
		}

		if ((v = jfind(js, toks, n, t_idx, "stats_out")) >= 0) {
			if (jstr(js, &toks[v], t->stats_out, sizeof(t->stats_out)) < 0)
				return -1;
		}
	} else {
		if ((v = jfind(js, toks, n, t_idx, "udp_in_port")) < 0) return -1;
		if (jint(js, &toks[v], &lv) < 0) return -1;
		t->udp_in_port = (int)lv;

		if ((v = jfind(js, toks, n, t_idx, "control_port")) >= 0) {
			if (jint(js, &toks[v], &lv) < 0) return -1;
			t->control_port = (int)lv;
		}

		int fec_idx = jfind(js, toks, n, t_idx, "fec");
		if (fec_idx >= 0 && toks[fec_idx].type == JT_OBJ) {
			if ((v = jfind(js, toks, n, fec_idx, "k")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->fec_k = (int)lv;
			}
			if ((v = jfind(js, toks, n, fec_idx, "n")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->fec_n = (int)lv;
			}
		}

		int radio_idx = jfind(js, toks, n, t_idx, "radio");
		if (radio_idx >= 0 && toks[radio_idx].type == JT_OBJ) {
			if ((v = jfind(js, toks, n, radio_idx, "bandwidth_mhz")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->bandwidth_mhz = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "mcs_index")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->mcs_index = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "stbc")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->stbc = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "ldpc")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->ldpc = (int)lv;
			}
		}
	}

	t->state = TS_STOPPED;
	t->pid   = -1;
	return 0;
}

static int cfg_load(const char *path, Config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->http_bind, sizeof(c->http_bind), "0.0.0.0");
	c->http_port = GS_DEFAULT_HTTP_PORT;
	c->venc_cmd_rate_limit_ms = 50;

	int fd = open(path, O_RDONLY);
	if (fd < 0) { LOG_ERR("config open(%s): %s", path, strerror(errno)); return -1; }
	struct stat st;
	if (fstat(fd, &st) < 0) { close(fd); return -1; }
	if (st.st_size <= 0 || st.st_size > 256 * 1024) { close(fd); return -1; }
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) { close(fd); return -1; }
	ssize_t r = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (r != st.st_size) { free(buf); return -1; }
	buf[r] = 0;

	JTok toks[JSON_MAX_TOKS];
	int n = json_parse(buf, (int)r, toks, JSON_MAX_TOKS);
	if (n < 1 || toks[0].type != JT_OBJ) {
		LOG_ERR("config parse failed (n=%d)", n);
		free(buf); return -1;
	}

	int v;
	long lv;
	if ((v = jfind(buf, toks, n, 0, "key_file")) >= 0)
		jstr(buf, &toks[v], c->key_file, sizeof(c->key_file));

	int http_idx = jfind(buf, toks, n, 0, "http");
	if (http_idx >= 0 && toks[http_idx].type == JT_OBJ) {
		if ((v = jfind(buf, toks, n, http_idx, "bind")) >= 0)
			jstr(buf, &toks[v], c->http_bind, sizeof(c->http_bind));
		if ((v = jfind(buf, toks, n, http_idx, "port")) >= 0 &&
		    jint(buf, &toks[v], &lv) == 0)
			c->http_port = (uint16_t)lv;
	}

	int sys_idx = jfind(buf, toks, n, 0, "system");
	if (sys_idx >= 0 && toks[sys_idx].type == JT_OBJ) {
		int up = jfind(buf, toks, n, sys_idx, "up");
		if (up >= 0 && toks[up].type == JT_ARR) {
			int kids = toks[up].size;
			if (kids > GS_MAX_SYSTEM_CMDS) {
				LOG_ERR("config: system.up has %d entries, max %d",
				        kids, GS_MAX_SYSTEM_CMDS);
				free(buf); return -1;
			}
			int ci = up + 1;
			for (int k = 0; k < kids; k++) {
				jstr(buf, &toks[ci], c->system_up[k], GS_PATH_MAX);
				ci = jskip(toks, n, ci);
			}
			c->system_up_count = kids;
		}
		int dn = jfind(buf, toks, n, sys_idx, "down");
		if (dn >= 0 && toks[dn].type == JT_ARR) {
			int kids = toks[dn].size;
			if (kids > GS_MAX_SYSTEM_CMDS) {
				LOG_ERR("config: system.down has %d entries, max %d",
				        kids, GS_MAX_SYSTEM_CMDS);
				free(buf); return -1;
			}
			int ci = dn + 1;
			for (int k = 0; k < kids; k++) {
				jstr(buf, &toks[ci], c->system_down[k], GS_PATH_MAX);
				ci = jskip(toks, n, ci);
			}
			c->system_down_count = kids;
		}
	}

	int vc_idx = jfind(buf, toks, n, 0, "venc_cmd");
	if (vc_idx >= 0 && toks[vc_idx].type == JT_OBJ) {
		bool bv;
		if ((v = jfind(buf, toks, n, vc_idx, "enabled")) >= 0 &&
		    jbool(buf, &toks[v], &bv) == 0)
			c->venc_cmd_enabled = bv;
		if ((v = jfind(buf, toks, n, vc_idx, "uplink_tunnel")) >= 0)
			jstr(buf, &toks[v], c->venc_cmd_uplink, sizeof(c->venc_cmd_uplink));
		if ((v = jfind(buf, toks, n, vc_idx, "rate_limit_ms")) >= 0 &&
		    jint(buf, &toks[v], &lv) == 0)
			c->venc_cmd_rate_limit_ms = (int)lv;
	}

	int tarr = jfind(buf, toks, n, 0, "tunnels");
	if (tarr < 0 || toks[tarr].type != JT_ARR) {
		LOG_ERR("config: missing 'tunnels' array");
		free(buf); return -1;
	}
	int kids = toks[tarr].size;
	if (kids < 1 || kids > GS_MAX_TUNNELS) {
		LOG_ERR("config: tunnel count %d out of range [1..%d]",
		        kids, GS_MAX_TUNNELS);
		free(buf); return -1;
	}
	int ci = tarr + 1;
	for (int k = 0; k < kids; k++) {
		if (cfg_parse_tunnel(buf, toks, n, ci, &c->tunnels[k]) < 0) {
			LOG_ERR("config: tunnel #%d parse failed", k);
			free(buf); return -1;
		}
		ci = jskip(toks, n, ci);
	}
	c->tunnel_count = kids;

	free(buf);
	return 0;
}

/* ---------- argv composition ----------------------------------------- */

typedef struct {
	char  buf[GS_ARGV_BUF];
	size_t pos;
	char *argv[GS_ARGV_MAX];
	int   argc;
} ArgvBuilder;

static int ab_push(ArgvBuilder *ab, const char *fmt, ...)
{
	if (ab->argc >= GS_ARGV_MAX - 1) return -1;
	if (ab->pos >= sizeof(ab->buf)) return -1;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(ab->buf + ab->pos, sizeof(ab->buf) - ab->pos, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(ab->buf) - ab->pos) return -1;
	ab->argv[ab->argc++] = ab->buf + ab->pos;
	ab->pos += (size_t)n + 1;
	return 0;
}

static int build_argv_rx(const Tunnel *t, const char *key_file, ArgvBuilder *ab)
{
	if (ab_push(ab, "%s", t->binary[0] ? t->binary : "wfb_rx") < 0) return -1;
	if (key_file && key_file[0] && (ab_push(ab, "-K") < 0 || ab_push(ab, "%s", key_file) < 0))
		return -1;
	if (t->udp_out_ip[0]) {
		if (ab_push(ab, "-c") < 0 || ab_push(ab, "%s", t->udp_out_ip) < 0) return -1;
		if (ab_push(ab, "-u") < 0 || ab_push(ab, "%d", t->udp_out_port) < 0) return -1;
	}
	if (ab_push(ab, "-i") < 0 || ab_push(ab, "%d", t->link_id) < 0) return -1;
	if (ab_push(ab, "-p") < 0 || ab_push(ab, "%d", t->radio_port) < 0) return -1;
	/* Always wire wfb_rx -Y into the supervisor's local listener so the
	 * Tunnels WebUI can show pkt counters / RSSI.  Forwarding to the user's
	 * configured stats_out happens in stats_drain(). */
	if (t->stats_local_arg[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_local_arg) < 0) return -1;
	} else if (t->stats_out[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_out) < 0) return -1;
	}
	for (int i = 0; i < t->extra_arg_count; i++)
		if (ab_push(ab, "%s", t->extra_args[i]) < 0) return -1;
	for (int i = 0; i < t->iface_count; i++)
		if (ab_push(ab, "%s", t->ifaces[i]) < 0) return -1;
	ab->argv[ab->argc] = NULL;
	return 0;
}

static int build_argv_tx(const Tunnel *t, const char *key_file, ArgvBuilder *ab)
{
	if (ab_push(ab, "%s", t->binary[0] ? t->binary : "wfb_tx") < 0) return -1;
	if (key_file && key_file[0] && (ab_push(ab, "-K") < 0 || ab_push(ab, "%s", key_file) < 0))
		return -1;
	if (ab_push(ab, "-i") < 0 || ab_push(ab, "%d", t->link_id) < 0) return -1;
	if (ab_push(ab, "-p") < 0 || ab_push(ab, "%d", t->radio_port) < 0) return -1;
	if (ab_push(ab, "-u") < 0 || ab_push(ab, "%d", t->udp_in_port) < 0) return -1;
	if (t->control_port > 0) {
		if (ab_push(ab, "-C") < 0 || ab_push(ab, "%d", t->control_port) < 0) return -1;
	}
	if (t->fec_k > 0) {
		if (ab_push(ab, "-k") < 0 || ab_push(ab, "%d", t->fec_k) < 0) return -1;
	}
	if (t->fec_n > 0) {
		if (ab_push(ab, "-n") < 0 || ab_push(ab, "%d", t->fec_n) < 0) return -1;
	}
	if (t->bandwidth_mhz > 0) {
		if (ab_push(ab, "-B") < 0 || ab_push(ab, "%d", t->bandwidth_mhz) < 0) return -1;
	}
	if (t->mcs_index >= 0) {
		if (ab_push(ab, "-M") < 0 || ab_push(ab, "%d", t->mcs_index) < 0) return -1;
	}
	if (t->stbc >= 0) {
		if (ab_push(ab, "-S") < 0 || ab_push(ab, "%d", t->stbc) < 0) return -1;
	}
	if (t->ldpc >= 0) {
		if (ab_push(ab, "-L") < 0 || ab_push(ab, "%d", t->ldpc) < 0) return -1;
	}
	if (t->stats_local_arg[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_local_arg) < 0) return -1;
	}
	for (int i = 0; i < t->extra_arg_count; i++)
		if (ab_push(ab, "%s", t->extra_args[i]) < 0) return -1;
	for (int i = 0; i < t->iface_count; i++)
		if (ab_push(ab, "%s", t->ifaces[i]) < 0) return -1;
	ab->argv[ab->argc] = NULL;
	return 0;
}

/* ---------- supervisor ----------------------------------------------- */

/* forward declarations: stats helpers live in the iface-validation block
 * further down to keep them grouped with the rest of the runtime/IPC code. */
static int  stats_listener_open(Tunnel *t);
static void stats_listener_close(Tunnel *t);
static void stats_drain(Tunnel *t);
static int  wfb_cmd_refresh_radio(Tunnel *t);
static int  wfb_cmd_refresh_fec(Tunnel *t);

static int tunnel_spawn(Tunnel *t, const char *key_file)
{
	/* Lazy-open the per-tunnel stats listener.  rx tunnels emit rx_ant,
	 * tx tunnels emit tx_stats — same -Y wire format, parsed by the same
	 * stats_drain().  Port stays stable across respawns so the argv is
	 * cacheable. */
	if (t->stats_local_fd < 0) {
		(void)stats_listener_open(t);
	}

	ArgvBuilder ab = {0};
	int rc = !strcmp(t->role, "rx")
	       ? build_argv_rx(t, key_file, &ab)
	       : build_argv_tx(t, key_file, &ab);
	if (rc < 0) {
		LOG_ERR("tunnel '%s': argv build failed", t->name);
		t->state = TS_FAILED;
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		LOG_ERR("tunnel '%s': fork: %s", t->name, strerror(errno));
		t->state = TS_FAILED;
		return -1;
	}
	if (pid == 0) {
		/* child — restore default signal handling, exec */
		signal(SIGCHLD, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT,  SIG_DFL);
		signal(SIGHUP,  SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		/* Silence wfb_rx/wfb_tx PKT spam unless -v was passed.  Keep
		 * stderr open just before exec so any exec failure message
		 * still reaches the operator; redirect after the (failed)
		 * exec is impossible, so we silence both fds up-front and
		 * write the exec-failure message to a fresh fd 2 via /dev/tty
		 * fallback — too fragile.  Instead leave stderr alone so the
		 * exec error bubbles up; only stdout gets nulled. */
		if (!g_verbose) {
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
		}
		execvp(ab.argv[0], ab.argv);
		/* exec failed — best-effort error to whatever fd 2 points to. */
		fprintf(stderr, "[gs] exec %s: %s\n", ab.argv[0], strerror(errno));
		_exit(127);
	}

	t->pid           = pid;
	t->state         = TS_STARTING;
	t->started_us    = now_us();
	t->autostart_on_exit = true;
	t->stop_deadline_ms  = 0;

	/* For tx tunnels with a control_port, schedule a one-shot
	 * get_radio + get_fec ~500 ms after spawn to populate the live cache.
	 * wfb_tx needs a moment to bind its control socket. */
	if (!strcmp(t->role, "tx") && t->control_port > 0)
		t->tx_init_query_after_us = t->started_us + 500000ull;

	/* Reset stats counters — wfb_rx/wfb_tx restart their own monotonic
	 * counters on respawn, so old values would lie. */
	t->st_msg_count = 0;
	t->st_interval_ms = 0;
	t->st_first_us = t->st_last_us = 0;
	if (!strcmp(t->role, "rx")) {
		t->st_pkt_all = t->st_pkt_lost = t->st_pkt_fec = 0;
		t->st_pkt_outgoing = t->st_pkt_dec_err = t->st_pkt_bytes = 0;
		t->st_pkt_uniq = 0;
		t->st_ant_count = 0;
		t->st_rssi_best = INT_MIN;
	} else {
		t->st_tx_pkts_in = t->st_tx_pkts_out = 0;
		t->st_tx_bytes_in = t->st_tx_bytes_out = 0;
		t->st_tx_drop = t->st_tx_trunc = t->st_tx_fec_timeouts = 0;
	}

	/* Promote to RUNNING after a short settle window in supervisor_tick;
	 * for now treat exec as success unless SIGCHLD says otherwise. */
	t->state = TS_RUNNING;
	t->restart_count++;

	/* log composed argv for diagnosability */
	char joined[512];
	size_t pos = 0;
	for (int i = 0; i < ab.argc && pos < sizeof(joined) - 2; i++) {
		int w = snprintf(joined + pos, sizeof(joined) - pos,
		                 "%s%s", i ? " " : "", ab.argv[i]);
		if (w < 0) break;
		pos += (size_t)w;
	}
	LOG_INFO("tunnel '%s' started pid=%d argv: %s", t->name, pid, joined);
	return 0;
}

static void tunnel_request_stop(Tunnel *t)
{
	if (t->pid <= 0) {
		t->state = TS_STOPPED;
		t->autostart_on_exit = false;
		return;
	}
	t->autostart_on_exit = false;
	t->stop_deadline_ms  = now_ms() + GS_STOP_GRACE_MS;
	if (kill(t->pid, SIGTERM) < 0 && errno != ESRCH)
		LOG_WARN("tunnel '%s': SIGTERM pid=%d: %s",
		         t->name, t->pid, strerror(errno));
	else
		LOG_INFO("tunnel '%s' stopping (SIGTERM pid=%d)", t->name, t->pid);
}

static void tunnel_on_exit(Tunnel *t, int wstatus)
{
	t->exited_us = now_us();
	if (WIFEXITED(wstatus))         t->exit_code = WEXITSTATUS(wstatus);
	else if (WIFSIGNALED(wstatus))  t->exit_code = -WTERMSIG(wstatus);
	else                            t->exit_code = -1;
	LOG_INFO("tunnel '%s' exited pid=%d code=%d (autostart_on_exit=%d)",
	         t->name, t->pid, t->exit_code, t->autostart_on_exit);
	t->pid = -1;
	t->stop_deadline_ms = 0;

	if (!t->autostart_on_exit) {
		t->state = TS_STOPPED;
		t->backoff_idx = 0;
		return;
	}
	int idx = t->backoff_idx;
	if (idx >= GS_BACKOFF_LEN) idx = GS_BACKOFF_LEN - 1;
	t->next_start_ms = now_ms() + (uint64_t)GS_BACKOFF_MS[idx];
	if (t->backoff_idx < GS_BACKOFF_LEN - 1) t->backoff_idx++;
	t->state = TS_BACKOFF;
}

static void supervisor_reap(Config *c)
{
	for (;;) {
		int wstatus = 0;
		pid_t pid = waitpid(-1, &wstatus, WNOHANG);
		if (pid <= 0) break;
		bool matched = false;
		for (int i = 0; i < c->tunnel_count; i++) {
			if (c->tunnels[i].pid == pid) {
				tunnel_on_exit(&c->tunnels[i], wstatus);
				matched = true;
				break;
			}
		}
		if (!matched) LOG_WARN("reaped unknown pid=%d", pid);
	}
}

/* Periodic tick: handle SIGKILL escalation + backoff respawn, plus
 * deferred TX state queries (~500 ms after spawn so wfb_tx has bound
 * its control socket). */
static void supervisor_tick(Config *c)
{
	uint64_t t_ms = now_ms();
	uint64_t t_us = now_us();
	for (int i = 0; i < c->tunnel_count; i++) {
		Tunnel *t = &c->tunnels[i];

		if (t->stop_deadline_ms && t_ms >= t->stop_deadline_ms && t->pid > 0) {
			LOG_WARN("tunnel '%s' SIGKILL pid=%d (grace expired)", t->name, t->pid);
			kill(t->pid, SIGKILL);
			t->stop_deadline_ms = 0;
		}

		if (t->state == TS_BACKOFF && t_ms >= t->next_start_ms) {
			LOG_INFO("tunnel '%s' respawning (attempt %d)",
			         t->name, t->restart_count + 1);
			tunnel_spawn(t, c->key_file);
		}

		if (t->tx_init_query_after_us && t_us >= t->tx_init_query_after_us &&
		    t->pid > 0 && t->state == TS_RUNNING) {
			t->tx_init_query_after_us = 0;
			if (wfb_cmd_refresh_radio(t) == 0) {
				LOG_INFO("tunnel '%s' radio cache: bw=%d mcs=%d stbc=%d "
				         "ldpc=%d sgi=%d vht_mode=%d vht_nss=%d",
				         t->name, t->bandwidth_mhz, t->mcs_index, t->stbc,
				         t->ldpc, t->short_gi, t->vht_mode, t->vht_nss);
			} else {
				/* Likely too soon — retry once 1 s later. */
				t->tx_init_query_after_us = t_us + 1000000ull;
			}
			(void)wfb_cmd_refresh_fec(t);
		}
	}
}

/* ---------- system commands ------------------------------------------ */

static int run_system_cmd(const char *cmd)
{
	LOG_INFO("system: %s", cmd);
	int rc = system(cmd);     /* TODO: replace with fork+execvp + arg parsing */
	if (rc != 0) LOG_WARN("system: '%s' rc=%d", cmd, rc);
	return rc;
}

static void run_system_block(const char *label, char cmds[][GS_PATH_MAX], int n)
{
	if (n <= 0) return;
	LOG_INFO("running %s commands (%d)", label, n);
	for (int i = 0; i < n; i++) {
		if (cmds[i][0]) (void)run_system_cmd(cmds[i]);
	}
}

/* ---------- rx stats listener + rx_ant parser ------------------------ *
 *
 * One UDP socket per rx tunnel.  wfb_rx is launched with `-Y` pointing at
 * this socket; the parser extracts pkt counters and antenna RSSI for the
 * Tunnels WebUI.  If the user originally configured `stats_out` to a
 * different host:port (e.g. forwarding rx_ant to the uplink TX so the
 * vehicle's link_controller sees ground RSSI), the supervisor re-emits
 * each datagram verbatim — the local consumption is purely additive.
 */

/* Parse "host:port" → sockaddr_in.  Returns 0 on success. */
static int parse_host_port(const char *s, struct sockaddr_in *out)
{
	if (!s || !*s) return -1;
	const char *colon = strrchr(s, ':');
	if (!colon || colon == s) return -1;
	char host[64];
	size_t hl = (size_t)(colon - s);
	if (hl >= sizeof(host)) return -1;
	memcpy(host, s, hl);
	host[hl] = 0;
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535) return -1;
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port   = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1) return -1;
	return 0;
}

/* Open a non-blocking UDP listener on 127.0.0.1:0 and stash the assigned
 * port in t->stats_local_port + a "127.0.0.1:port" string in
 * t->stats_local_arg (for handing to wfb_rx -Y). */
static int stats_listener_open(Tunnel *t)
{
	if (t->stats_local_fd >= 0) return 0;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		LOG_ERR("tunnel '%s': stats listener socket: %s", t->name, strerror(errno));
		return -1;
	}
	int rcvbuf = 256 * 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_in sa = {0};
	sa.sin_family      = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		LOG_ERR("tunnel '%s': stats bind 127.0.0.1:0: %s",
		        t->name, strerror(errno));
		close(fd);
		return -1;
	}
	socklen_t slen = sizeof(sa);
	if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0) {
		LOG_ERR("tunnel '%s': stats getsockname: %s",
		        t->name, strerror(errno));
		close(fd);
		return -1;
	}
	t->stats_local_fd   = fd;
	t->stats_local_port = ntohs(sa.sin_port);
	snprintf(t->stats_local_arg, sizeof(t->stats_local_arg),
	         "127.0.0.1:%u", (unsigned)t->stats_local_port);

	/* If user configured stats_out, parse for re-emit. */
	if (t->stats_out[0]) {
		if (parse_host_port(t->stats_out, &t->stats_fwd_addr) == 0) {
			t->stats_fwd_active = 1;
			LOG_INFO("tunnel '%s': stats listening on udp:%u (forward to %s)",
			         t->name, (unsigned)t->stats_local_port, t->stats_out);
		} else {
			t->stats_fwd_active = 0;
			LOG_WARN("tunnel '%s': stats_out '%s' unparseable; not forwarding",
			         t->name, t->stats_out);
		}
	} else {
		t->stats_fwd_active = 0;
		LOG_INFO("tunnel '%s': stats listening on udp:%u",
		         t->name, (unsigned)t->stats_local_port);
	}
	return 0;
}

/* Pull an unsigned-decimal field out of a pinned JSON window.  We
 * intentionally avoid a real parser — the rx_ant payload is generated
 * by a single producer with a stable shape and we just want the named
 * counters. */
static int json_pick_u32(const char *js, size_t jl, const char *key, uint32_t *out)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 2 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		if (js[p] == '-') return -1; /* refuse negative for u32 */
		uint64_t v = 0;
		int seen = 0;
		while (p < jl && js[p] >= '0' && js[p] <= '9') {
			v = v * 10u + (uint32_t)(js[p] - '0');
			p++;
			seen = 1;
			if (v > 0xFFFFFFFFull) return -1;
		}
		if (!seen) continue;
		*out = (uint32_t)v;
		return 0;
	}
	return -1;
}

/* Pull a signed integer field. */
static int json_pick_i32(const char *js, size_t jl, const char *key, int32_t *out)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 2 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		int neg = 0;
		if (js[p] == '-') { neg = 1; p++; }
		int64_t v = 0;
		int seen = 0;
		while (p < jl && js[p] >= '0' && js[p] <= '9') {
			v = v * 10 + (js[p] - '0');
			p++;
			seen = 1;
		}
		if (!seen) continue;
		*out = (int32_t)(neg ? -v : v);
		return 0;
	}
	return -1;
}

/* Slice a JSON value by key — returns pointer + length of the substring
 * starting at the value (object, array, or scalar).  We only need this to
 * carve out the "pkt":{...} block so json_pick_u32 ranges stay scoped. */
static int json_slice_object(const char *js, size_t jl, const char *key,
                              const char **out_start, size_t *out_len)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 3 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		char open  = js[p];
		char close = (open == '{') ? '}' : (open == '[') ? ']' : 0;
		if (!close) return -1;
		int depth = 0;
		size_t start = p;
		for (; p < jl; p++) {
			if (js[p] == open)  depth++;
			if (js[p] == close) depth--;
			if (depth == 0) {
				*out_start = js + start;
				*out_len   = (p - start) + 1;
				return 0;
			}
		}
		return -1;
	}
	return -1;
}

/* Drain pending rx_ant datagrams off a single tunnel's stats fd. */
static void stats_drain(Tunnel *t)
{
	for (;;) {
		char buf[4096];
		ssize_t got = recv(t->stats_local_fd, buf, sizeof(buf) - 1, 0);
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			if (errno == EINTR) continue;
			LOG_WARN("tunnel '%s': stats recv: %s", t->name, strerror(errno));
			return;
		}
		if (got == 0) continue;
		buf[got] = 0;

		/* Re-emit to user's stats_out before parsing — keeps latency
		 * for the downstream consumer minimal. */
		if (t->stats_fwd_active) {
			(void)sendto(t->stats_local_fd, buf, (size_t)got, 0,
			             (struct sockaddr *)&t->stats_fwd_addr,
			             sizeof(t->stats_fwd_addr));
		}

		uint32_t iv;
		if (json_pick_u32(buf, (size_t)got, "interval_ms", &iv) == 0)
			t->st_interval_ms = iv;

		/* Dispatch on JSON type.  wfb_rx emits rx_ant; wfb_tx emits
		 * tx_stats.  We accept either on the same socket so a single
		 * supervisor port works for both tunnel roles. */
		bool is_rx_ant   = strstr(buf, "\"type\":\"rx_ant\"") != NULL;
		bool is_tx_stats = strstr(buf, "\"type\":\"tx_stats\"") != NULL;
		if (!is_rx_ant && !is_tx_stats) continue;

		if (is_rx_ant) {
			const char *pkt = NULL;
			size_t pkl = 0;
			if (json_slice_object(buf, (size_t)got, "pkt", &pkt, &pkl) == 0) {
				uint32_t u;
				if (json_pick_u32(pkt, pkl, "all",            &u) == 0) t->st_pkt_all      = u;
				if (json_pick_u32(pkt, pkl, "lost",           &u) == 0) t->st_pkt_lost     = u;
				if (json_pick_u32(pkt, pkl, "fec_recovered",  &u) == 0) t->st_pkt_fec      = u;
				if (json_pick_u32(pkt, pkl, "outgoing",       &u) == 0) t->st_pkt_outgoing = u;
				if (json_pick_u32(pkt, pkl, "dec_err",        &u) == 0) t->st_pkt_dec_err  = u;
				if (json_pick_u32(pkt, pkl, "outgoing_bytes", &u) == 0) t->st_pkt_bytes    = u;
				if (json_pick_u32(pkt, pkl, "uniq",           &u) == 0) t->st_pkt_uniq     = u;
			}

			/* Walk antenna list for best avg rssi.  rssi.avg is signed. */
			const char *ant = NULL;
			size_t al = 0;
			int ant_count = 0;
			int best_rssi = INT_MIN;
			if (json_slice_object(buf, (size_t)got, "ant", &ant, &al) == 0 && al >= 2) {
				int depth = 0;
				size_t obj_start = 0;
				for (size_t i = 0; i < al; i++) {
					char ch = ant[i];
					if (ch == '{') {
						if (depth == 0) obj_start = i;
						depth++;
					} else if (ch == '}') {
						depth--;
						if (depth == 0) {
							const char *o = ant + obj_start;
							size_t ol = (i - obj_start) + 1;
							const char *rblock;
							size_t rbl;
							if (json_slice_object(o, ol, "rssi", &rblock, &rbl) == 0) {
								int32_t avg;
								if (json_pick_i32(rblock, rbl, "avg", &avg) == 0) {
									if (avg > best_rssi) best_rssi = avg;
								}
							}
							ant_count++;
						}
					}
				}
			}
			t->st_ant_count = ant_count;
			if (best_rssi != INT_MIN) t->st_rssi_best = best_rssi;
		}

		if (is_tx_stats) {
			const char *txb = NULL;
			size_t tbl = 0;
			if (json_slice_object(buf, (size_t)got, "tx", &txb, &tbl) == 0) {
				uint32_t u;
				int32_t  s;
				if (json_pick_u32(txb, tbl, "pkts_in",      &u) == 0) t->st_tx_pkts_in     = u;
				if (json_pick_u32(txb, tbl, "pkts_out",     &u) == 0) t->st_tx_pkts_out    = u;
				if (json_pick_u32(txb, tbl, "bytes_in",     &u) == 0) t->st_tx_bytes_in    = u;
				if (json_pick_u32(txb, tbl, "bytes_out",    &u) == 0) t->st_tx_bytes_out   = u;
				if (json_pick_u32(txb, tbl, "pkts_drop",    &u) == 0) t->st_tx_drop        = u;
				if (json_pick_u32(txb, tbl, "pkts_trunc",   &u) == 0) t->st_tx_trunc       = u;
				if (json_pick_u32(txb, tbl, "fec_timeouts", &u) == 0) t->st_tx_fec_timeouts = u;
				if (json_pick_i32(txb, tbl, "fec_k",        &s) == 0) {
					t->fec_k = s;
					t->fec_cache_have = 1;
				}
				if (json_pick_i32(txb, tbl, "fec_n",        &s) == 0) {
					t->fec_n = s;
					t->fec_cache_have = 1;
				}
			}
			/* radio block — refresh the live radio cache so the WebUI
			 * gets continuous updates without a wfb_cmd round-trip. */
			const char *rb = NULL;
			size_t rl = 0;
			if (json_slice_object(buf, (size_t)got, "radio", &rb, &rl) == 0) {
				int32_t s;
				if (json_pick_i32(rb, rl, "mcs",      &s) == 0) t->mcs_index     = s;
				if (json_pick_i32(rb, rl, "bw",       &s) == 0) t->bandwidth_mhz = s;
				if (json_pick_i32(rb, rl, "short_gi", &s) == 0) t->short_gi      = s;
				if (json_pick_i32(rb, rl, "stbc",     &s) == 0) t->stbc          = s;
				if (json_pick_i32(rb, rl, "ldpc",     &s) == 0) t->ldpc          = s;
				if (json_pick_i32(rb, rl, "vht_mode", &s) == 0) t->vht_mode      = s;
				if (json_pick_i32(rb, rl, "vht_nss",  &s) == 0) t->vht_nss       = s;
				t->radio_cache_have = 1;
				t->radio_cache_us   = now_us();
			}
		}

		uint64_t now = now_us();
		if (t->st_msg_count == 0) t->st_first_us = now;
		t->st_last_us = now;
		t->st_msg_count++;
	}
}

static void stats_listener_close(Tunnel *t)
{
	if (t->stats_local_fd >= 0) {
		close(t->stats_local_fd);
		t->stats_local_fd = -1;
	}
}

/* ---------- iface state validation ----------------------------------- *
 *
 * Run AFTER system.up. Confirms each iface declared in any tunnel:
 *   - exists and is in monitor mode (via `iw dev <iface> info`)
 *   - is administratively UP (via /sys/class/net/<iface>/flags, IFF_UP bit)
 *
 * If any check fails, the supervisor refuses to spawn tunnels. Spawning
 * wfb_rx into a non-monitor iface produces opaque "pcap_activate failed"
 * loops that aren't actionable.
 */
#define IFF_UP_BIT 0x1u

static int iface_is_monitor(const char *iface)
{
	char cmd[256];
	int n = snprintf(cmd, sizeof(cmd),
	                 "iw dev %s info 2>/dev/null | grep -qE 'type monitor'",
	                 iface);
	if (n < 0 || n >= (int)sizeof(cmd)) return -1;
	int rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static int iface_is_admin_up(const char *iface)
{
	char path[128];
	int pn = snprintf(path, sizeof(path), "/sys/class/net/%s/flags", iface);
	if (pn < 0 || pn >= (int)sizeof(path)) return -1;
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	unsigned long flags = 0;
	int n = fscanf(f, "%lx", &flags);
	fclose(f);
	if (n != 1) return -1;
	return (flags & IFF_UP_BIT) ? 0 : -1;
}

/* Block up to `timeout_ms` waiting for every tunnel iface to be both
 * admin-UP and in monitor mode.  Polled every `interval_ms`.  This
 * absorbs the nondeterministic delay between `iw dev … set monitor` and
 * the iface becoming usable for pcap_open_live — a fixed `sleep` in the
 * config is fragile (driver/USB/load dependent).  Returns 0 if every
 * iface is ready before the deadline, -1 otherwise. */
static int wait_iface_state(const Config *c, int timeout_ms, int interval_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
	int prev_bad = -1;
	for (;;) {
		int bad = 0;
		const char *first_bad = NULL;
		const char *first_reason = NULL;
		for (int i = 0; i < c->tunnel_count; i++) {
			const Tunnel *t = &c->tunnels[i];
			for (int k = 0; k < t->iface_count; k++) {
				const char *ifc = t->ifaces[k];
				if (iface_is_admin_up(ifc) != 0) {
					bad++;
					if (!first_bad) { first_bad = ifc; first_reason = "down"; }
					continue;
				}
				if (iface_is_monitor(ifc) != 0) {
					bad++;
					if (!first_bad) { first_bad = ifc; first_reason = "not-monitor"; }
				}
			}
		}
		if (bad == 0) {
			LOG_INFO("iface state OK on %d tunnel(s)", c->tunnel_count);
			return 0;
		}
		if (bad != prev_bad) {
			LOG_INFO("waiting for iface readiness — %d pending (first: %s %s)",
			         bad, first_bad ? first_bad : "?",
			         first_reason ? first_reason : "?");
			prev_bad = bad;
		}
		if (now_ms() >= deadline) {
			/* One last detailed report so the operator knows which iface
			 * timed out and why. */
			for (int i = 0; i < c->tunnel_count; i++) {
				const Tunnel *t = &c->tunnels[i];
				for (int k = 0; k < t->iface_count; k++) {
					const char *ifc = t->ifaces[k];
					if (iface_is_admin_up(ifc) != 0) {
						LOG_ERR("iface '%s' is not administratively UP "
						        "after %d ms — check 'ip link set %s up' "
						        "or extend the system.up sleep",
						        ifc, timeout_ms, ifc);
					} else if (iface_is_monitor(ifc) != 0) {
						LOG_ERR("iface '%s' is not in monitor mode "
						        "after %d ms — check 'iw dev %s set monitor'",
						        ifc, timeout_ms, ifc);
					}
				}
			}
			LOG_ERR("iface readiness timed out; refusing to spawn tunnels");
			return -1;
		}
		struct timespec ts = {
			.tv_sec  = interval_ms / 1000,
			.tv_nsec = (long)(interval_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
	}
}

/* ---------- wfb_cmd passthrough -------------------------------------- *
 *
 * Wire format mirrors poc/build/wfb-ng/src/tx_cmd.h (the patched wfb-ng
 * shipped via build_wfb_tx.sh). Keep the layout in sync if tx_cmd.h
 * changes — values are network byte order on the wire.
 *
 * Request:  uint32_t req_id; uint8_t cmd_id; <union body>
 *   set_fec    body (4 B):  k, n, fec_timeout_ms (NB)
 *   set_radio  body (7 B):  stbc, ldpc, short_gi, bw, mcs, vht_mode, vht_nss
 *   get_fec / get_radio:    no body
 *
 * Response: uint32_t req_id; uint32_t rc (NB); <union body>
 *   On error (rc != 0): only req_id+rc are sent (body omitted).
 *
 * wfb_tx binds its control socket to 127.0.0.1 only — sendto must come
 * from a 127.x address. */

#define WFB_CMD_SET_FEC          1
#define WFB_CMD_SET_RADIO        2
#define WFB_CMD_GET_FEC          3
#define WFB_CMD_GET_RADIO        4
#define WFB_FEC_TIMEOUT_KEEP     0xFFFFu
#define WFB_CMD_REPLY_TIMEOUT_MS 200

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	uint8_t  k;
	uint8_t  n;
	uint16_t fec_timeout_ms;     /* NB */
} __attribute__((packed)) WfbCmdSetFecReq;     /* 9 B */

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	uint8_t  stbc;
	uint8_t  ldpc;
	uint8_t  short_gi;
	uint8_t  bandwidth;
	uint8_t  mcs_index;
	uint8_t  vht_mode;
	uint8_t  vht_nss;
} __attribute__((packed)) WfbCmdSetRadioReq;   /* 12 B */

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
} __attribute__((packed)) WfbCmdGetReq;        /* 5 B */

typedef struct {
	uint32_t req_id;
	uint32_t rc;                 /* NB; 0 = success */
	uint8_t  k;
	uint8_t  n;
	uint16_t fec_timeout_ms;     /* NB */
} __attribute__((packed)) WfbCmdGetFecResp;    /* 12 B */

typedef struct {
	uint32_t req_id;
	uint32_t rc;                 /* NB */
	uint8_t  stbc;
	uint8_t  ldpc;
	uint8_t  short_gi;
	uint8_t  bandwidth;
	uint8_t  mcs_index;
	uint8_t  vht_mode;
	uint8_t  vht_nss;
} __attribute__((packed)) WfbCmdGetRadioResp;  /* 15 B */

/* Returns next req_id (32-bit rolling). Single-threaded => static is fine. */
static uint32_t wfb_cmd_next_req_id(void)
{
	static uint32_t s_req_id = 0;
	return ++s_req_id;
}

/* Send a wfb_cmd request to 127.0.0.1:control_port and wait briefly for a
 * reply. Returns the number of bytes received (0 on timeout, -1 on send
 * failure). On a successful recv, *out_rc is set to the parsed rc field.
 *
 * `req` is the packed request payload; `req_len` its size on the wire.
 * `resp_buf` is a caller-provided buffer big enough for the largest
 * possible response (sizeof(WfbCmdGetRadioResp)).
 */
static int wfb_cmd_round_trip(int control_port,
                              const void *req, size_t req_len,
                              void *resp_buf, size_t resp_cap,
                              uint32_t *out_rc)
{
	if (out_rc) *out_rc = 0xFFFFFFFFu;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return -1;
	struct timeval tv = {
		.tv_sec  = WFB_CMD_REPLY_TIMEOUT_MS / 1000,
		.tv_usec = (WFB_CMD_REPLY_TIMEOUT_MS % 1000) * 1000,
	};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)control_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ssize_t w = sendto(s, req, req_len, 0,
	                   (struct sockaddr *)&dst, sizeof(dst));
	if (w != (ssize_t)req_len) {
		close(s);
		return -1;
	}
	ssize_t r = recv(s, resp_buf, resp_cap, 0);
	close(s);
	if (r <= 0) return 0;          /* timeout or peer error */
	if (out_rc && r >= 8) {
		uint32_t rc_nb;
		memcpy(&rc_nb, (char *)resp_buf + 4, 4);
		*out_rc = ntohl(rc_nb);
	}
	return (int)r;
}

/* Fetch live radio params via get_radio and stash into the tunnel's
 * runtime cache.  Single-shot (~200ms timeout); caller throttles. */
static int wfb_cmd_refresh_radio(Tunnel *t)
{
	if (t->control_port <= 0) return -1;
	WfbCmdGetReq req = {
		.req_id = wfb_cmd_next_req_id(),
		.cmd_id = WFB_CMD_GET_RADIO,
	};
	char buf[64];
	uint32_t rc_out = 0xFFFFFFFFu;
	int n = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
	                           buf, sizeof(buf), &rc_out);
	if (n < (int)sizeof(WfbCmdGetRadioResp) || rc_out != 0) return -1;
	WfbCmdGetRadioResp gr;
	memcpy(&gr, buf, sizeof(gr));
	t->stbc           = gr.stbc;
	t->ldpc           = gr.ldpc;
	t->short_gi       = gr.short_gi;
	t->bandwidth_mhz  = gr.bandwidth;
	t->mcs_index      = gr.mcs_index;
	t->vht_mode       = gr.vht_mode;
	t->vht_nss        = gr.vht_nss;
	t->radio_cache_have = 1;
	t->radio_cache_us   = now_us();
	return 0;
}

static int wfb_cmd_refresh_fec(Tunnel *t)
{
	if (t->control_port <= 0) return -1;
	WfbCmdGetReq req = {
		.req_id = wfb_cmd_next_req_id(),
		.cmd_id = WFB_CMD_GET_FEC,
	};
	char buf[64];
	uint32_t rc_out = 0xFFFFFFFFu;
	int n = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
	                           buf, sizeof(buf), &rc_out);
	if (n < (int)sizeof(WfbCmdGetFecResp) || rc_out != 0) return -1;
	WfbCmdGetFecResp gf;
	memcpy(&gf, buf, sizeof(gf));
	t->fec_k          = gf.k;
	t->fec_n          = gf.n;
	t->fec_timeout_ms = ntohs(gf.fec_timeout_ms);
	t->fec_cache_have = 1;
	return 0;
}

/* Tiny query-string scanner: find &key=...& and copy value (URL-decoded
 * for + and %xx is not needed for our integer args). On hit, returns
 * pointer into qs at start of value and *len_out is value length. NULL on
 * miss. qs is the part after '?', already de-prefixed. */
static const char *qs_get(const char *qs, const char *key, size_t *len_out)
{
	if (!qs) return NULL;
	size_t klen = strlen(key);
	const char *p = qs;
	while (*p) {
		const char *eq = strchr(p, '=');
		const char *amp = strchr(p, '&');
		if (!eq) break;
		size_t kl = (size_t)(eq - p);
		const char *val = eq + 1;
		size_t vl = amp ? (size_t)(amp - val) : strlen(val);
		if (kl == klen && !strncmp(p, key, klen)) {
			if (len_out) *len_out = vl;
			return val;
		}
		if (!amp) break;
		p = amp + 1;
	}
	return NULL;
}

static int qs_get_int(const char *qs, const char *key, int *out)
{
	size_t vl;
	const char *v = qs_get(qs, key, &vl);
	if (!v || vl == 0 || vl > 10) return -1;
	char buf[12];
	memcpy(buf, v, vl); buf[vl] = 0;
	char *end = NULL;
	long iv = strtol(buf, &end, 10);
	if (!end || *end) return -1;
	*out = (int)iv;
	return 0;
}

/* ---------- WCMD emit ------------------------------------------------ *
 *
 * 16-byte WCMD_MSG_REQ, layout per poc/wcmd_proto.h. Sent to the uplink
 * tunnel's udp_in_port (127.0.0.1) where wfb_tx forwards it over the air.
 * The vehicle's link_controller demuxes WCMD frames from rx_ant JSON via
 * the WCMD_MAGIC ("WCMD" BE) prefix and proxies to the venc HTTP API.
 *
 * Fire-and-forget on this side — link_controller's WcmdResp goes back via
 * the rx_ant return path, which the ground supervisor does not yet bind.
 * Response correlation is deferred to v2 per the design doc.
 */

#define WCMD_MAGIC          0x57434D44u   /* "WCMD" big-endian */
#define WCMD_VERSION        1
#define WCMD_MSG_REQ        1
#define WCMD_KEY_BITRATE_KBPS    1
#define WCMD_KEY_FPS             2
#define WCMD_KEY_PAYLOAD_BYTES   3
#define WCMD_KEY_FORCE_IDR       4
/* Vehicle-side wfb_tx control keys — see wcmd_proto.h. */
#define WCMD_KEY_WFB_FEC_K       5
#define WCMD_KEY_WFB_FEC_N       6
#define WCMD_KEY_WFB_MCS         7
#define WCMD_KEY_WFB_BANDWIDTH   8
#define WCMD_KEY_WFB_LDPC        9
#define WCMD_KEY_WFB_STBC       10
#define WCMD_KEY_WFB_SHORT_GI   11
/* Adaptive subsystem master switches on the vehicle's link_controller. */
#define WCMD_KEY_FEC_ENABLED    12
#define WCMD_KEY_MCS_ENABLED    13

#pragma pack(push, 1)
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;
	uint16_t seq;          /* NB */
	uint8_t  key;
	uint8_t  flags;        /* must be 0 */
	uint16_t _pad;         /* must be 0 */
	int32_t  value;        /* NB */
} WcmdReq;
#pragma pack(pop)

/* Per-key rate-limit state (single-process). Index = key id (1-based). */
#define WCMD_KEY_MAX 16
static uint64_t g_wcmd_last_send_ms[WCMD_KEY_MAX + 1] = { 0 };
static uint16_t g_wcmd_seq = 0;

static int wcmd_key_from_str(const char *s, size_t n)
{
	if (n == 12 && !strncmp(s, "bitrate_kbps",  12)) return WCMD_KEY_BITRATE_KBPS;
	if (n ==  3 && !strncmp(s, "fps",            3)) return WCMD_KEY_FPS;
	if (n == 13 && !strncmp(s, "payload_bytes", 13)) return WCMD_KEY_PAYLOAD_BYTES;
	if (n ==  9 && !strncmp(s, "force_idr",      9)) return WCMD_KEY_FORCE_IDR;
	if (n ==  9 && !strncmp(s, "wfb_fec_k",      9)) return WCMD_KEY_WFB_FEC_K;
	if (n ==  9 && !strncmp(s, "wfb_fec_n",      9)) return WCMD_KEY_WFB_FEC_N;
	if (n ==  7 && !strncmp(s, "wfb_mcs",        7)) return WCMD_KEY_WFB_MCS;
	if (n == 13 && !strncmp(s, "wfb_bandwidth", 13)) return WCMD_KEY_WFB_BANDWIDTH;
	if (n ==  8 && !strncmp(s, "wfb_ldpc",       8)) return WCMD_KEY_WFB_LDPC;
	if (n ==  8 && !strncmp(s, "wfb_stbc",       8)) return WCMD_KEY_WFB_STBC;
	if (n == 12 && !strncmp(s, "wfb_short_gi", 12))  return WCMD_KEY_WFB_SHORT_GI;
	if (n == 11 && !strncmp(s, "fec_enabled", 11))   return WCMD_KEY_FEC_ENABLED;
	if (n == 11 && !strncmp(s, "mcs_enabled", 11))   return WCMD_KEY_MCS_ENABLED;
	return -1;
}

static const char *wcmd_key_name(int key)
{
	switch (key) {
	case WCMD_KEY_BITRATE_KBPS:  return "bitrate_kbps";
	case WCMD_KEY_FPS:           return "fps";
	case WCMD_KEY_PAYLOAD_BYTES: return "payload_bytes";
	case WCMD_KEY_FORCE_IDR:     return "force_idr";
	case WCMD_KEY_WFB_FEC_K:     return "wfb_fec_k";
	case WCMD_KEY_WFB_FEC_N:     return "wfb_fec_n";
	case WCMD_KEY_WFB_MCS:       return "wfb_mcs";
	case WCMD_KEY_WFB_BANDWIDTH: return "wfb_bandwidth";
	case WCMD_KEY_WFB_LDPC:      return "wfb_ldpc";
	case WCMD_KEY_WFB_STBC:      return "wfb_stbc";
	case WCMD_KEY_WFB_SHORT_GI:  return "wfb_short_gi";
	case WCMD_KEY_FEC_ENABLED:   return "fec_enabled";
	case WCMD_KEY_MCS_ENABLED:   return "mcs_enabled";
	}
	return "?";
}

/* Send a WCMD_MSG_REQ packet. Returns 0 on success, errno code (positive)
 * on send failure, -1 if the uplink tunnel is missing/disabled. */
static int wcmd_emit(const Config *c, int key, int32_t value, uint16_t *seq_out)
{
	if (!c->venc_cmd_enabled) return -1;
	const Tunnel *up = NULL;
	for (int i = 0; i < c->tunnel_count; i++) {
		if (!strcmp(c->tunnels[i].name, c->venc_cmd_uplink) &&
		    !strcmp(c->tunnels[i].role, "tx")) {
			up = &c->tunnels[i];
			break;
		}
	}
	if (!up || up->udp_in_port <= 0) return -1;

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return errno;
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)up->udp_in_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	uint16_t seq = ++g_wcmd_seq;
	if (seq_out) *seq_out = seq;

	WcmdReq req = {
		.magic    = htonl(WCMD_MAGIC),
		.version  = WCMD_VERSION,
		.msg_type = WCMD_MSG_REQ,
		.seq      = htons(seq),
		.key      = (uint8_t)key,
		.flags    = 0,
		._pad     = 0,
		.value    = (int32_t)htonl((uint32_t)value),
	};
	ssize_t w = sendto(s, &req, sizeof(req), 0,
	                   (struct sockaddr *)&dst, sizeof(dst));
	int err = (w == sizeof(req)) ? 0 : errno;
	close(s);
	return err;
}

/* ---------- Embedded WebUI ------------------------------------------- *
 *
 * webui/gs.html is xxd-i embedded by Makefile.gs_supervisor's `webui`
 * target as a const byte array. Served at `/` when the request carries
 * Accept: text/html (browser path); curl with no overrides gets the
 * /api/v1/health-style help text instead. */
#if __has_include("gs_webui_assets.h")
#  include "gs_webui_assets.h"
#else
   static const unsigned char gs_webui_html[] =
       "<!doctype html><h1>gs_webui_assets.h missing — run "
       "`make -f Makefile.gs_supervisor webui`.</h1>";
   static const unsigned int  gs_webui_html_len = sizeof(gs_webui_html) - 1;
#endif

static bool request_wants_html(const char *req)
{
	const char *eol = strchr(req, '\n');
	const char *headers = eol ? eol + 1 : req;
	return strstr(headers, "Accept: text/html") != NULL ||
	       strstr(headers, "accept: text/html") != NULL;
}

/* ---------- HTTP API ------------------------------------------------- */

typedef struct {
	int      fd;
	uint64_t accepted_us;
	size_t   pos;
	char     buf[API_BUF_BYTES];
} ApiClient;

static int api_listen_open(const char *bind_ip, uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (!bind_ip || !*bind_ip) a.sin_addr.s_addr = htonl(INADDR_ANY);
	else if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
		close(fd); return -1;
	}
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
	if (listen(fd, 4) < 0) { close(fd); return -1; }
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return fd;
}

static void api_send(int fd, int code, const char *ctype, const char *body, int body_len)
{
	if (body_len < 0) body_len = (int)strlen(body);
	const char *reason = (code == 200) ? "OK" :
	                     (code == 400) ? "Bad Request" :
	                     (code == 404) ? "Not Found" :
	                     (code == 405) ? "Method Not Allowed" :
	                                     "Error";
	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n",
		code, reason, ctype, body_len);
	if (hl > 0 && write_all(fd, hdr, (size_t)hl) == 0)
		(void)write_all(fd, body, (size_t)body_len);
}

/* Send an arbitrary byte blob (e.g. embedded HTML). */
static void api_send_blob(int fd, const char *ctype,
                          const unsigned char *body, unsigned int len)
{
	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %u\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n",
		ctype, len);
	if (hl > 0 && write_all(fd, hdr, (size_t)hl) == 0)
		(void)write_all(fd, body, (size_t)len);
}

static int json_emit_tunnel(char *buf, size_t cap, const Tunnel *t, bool full)
{
	int p = 0;
	#define APP(...) do { int _w = snprintf(buf+p, cap-p, __VA_ARGS__); \
	                      if (_w < 0 || (size_t)_w >= cap-(size_t)p) return -1; \
	                      p += _w; } while (0)
	APP("{\"name\":\"%s\",\"role\":\"%s\",\"state\":\"%s\","
	    "\"pid\":%d,\"restarts\":%d,\"link_id\":%d,\"radio_port\":%d",
	    t->name, t->role, tunnel_state_name(t->state),
	    (int)t->pid, t->restart_count, t->link_id, t->radio_port);
	APP(",\"interfaces\":[");
	for (int i = 0; i < t->iface_count; i++)
		APP("%s\"%s\"", i ? "," : "", t->ifaces[i]);
	APP("]");
	if (t->started_us)
		APP(",\"uptime_s\":%.1f",
		    t->pid > 0 ? (double)(now_us() - t->started_us) / 1e6 : 0.0);
	if (t->exit_code || t->exited_us)
		APP(",\"last_exit\":%d", t->exit_code);
	if (full) {
		if (t->binary[0]) APP(",\"binary\":\"%s\"", t->binary);
		if (!strcmp(t->role, "rx")) {
			if (t->udp_out_ip[0])
				APP(",\"udp_out\":\"%s:%d\"", t->udp_out_ip, t->udp_out_port);
			if (t->stats_out[0]) APP(",\"stats_out\":\"%s\"", t->stats_out);
		} else {
			APP(",\"udp_in_port\":%d,\"control_port\":%d,"
			    "\"fec\":{\"k\":%d,\"n\":%d,\"timeout_ms\":%d,\"have\":%s},"
			    "\"radio\":{\"bw\":%d,\"mcs\":%d,\"stbc\":%d,\"ldpc\":%d,"
			    "\"short_gi\":%d,\"vht_mode\":%d,\"vht_nss\":%d,\"have\":%s}",
			    t->udp_in_port, t->control_port,
			    t->fec_k, t->fec_n, t->fec_timeout_ms,
			    t->fec_cache_have ? "true" : "false",
			    t->bandwidth_mhz, t->mcs_index, t->stbc, t->ldpc,
			    t->short_gi, t->vht_mode, t->vht_nss,
			    t->radio_cache_have ? "true" : "false");
		}
		APP(",\"autostart\":%s", t->autostart ? "true" : "false");
	}
	/* Always include stats for both roles — even if empty, the WebUI
	 * uses the presence of `msg_count` to decide whether to render the
	 * stats panel.  age_ms tells the UI how stale the snapshot is. */
	{
		uint64_t age_ms = 0;
		if (t->st_last_us) {
			uint64_t now_u = now_us();
			age_ms = (now_u > t->st_last_us) ? (now_u - t->st_last_us) / 1000 : 0;
		}
		APP(",\"stats\":{"
		    "\"msg_count\":%u,\"interval_ms\":%u",
		    t->st_msg_count, t->st_interval_ms);
		if (!strcmp(t->role, "rx")) {
			APP(",\"pkt_all\":%u,\"pkt_lost\":%u,\"pkt_fec_recovered\":%u,"
			    "\"pkt_outgoing\":%u,\"pkt_dec_err\":%u,\"pkt_uniq\":%u,"
			    "\"outgoing_bytes\":%u,\"ant_count\":%d",
			    t->st_pkt_all, t->st_pkt_lost, t->st_pkt_fec,
			    t->st_pkt_outgoing, t->st_pkt_dec_err, t->st_pkt_uniq,
			    t->st_pkt_bytes, t->st_ant_count);
			if (t->st_rssi_best != INT_MIN) APP(",\"rssi_best\":%d", t->st_rssi_best);
		} else {
			APP(",\"pkts_in\":%u,\"pkts_out\":%u,"
			    "\"bytes_in\":%u,\"bytes_out\":%u,"
			    "\"pkts_drop\":%u,\"pkts_trunc\":%u,\"fec_timeouts\":%u",
			    t->st_tx_pkts_in, t->st_tx_pkts_out,
			    t->st_tx_bytes_in, t->st_tx_bytes_out,
			    t->st_tx_drop, t->st_tx_trunc, t->st_tx_fec_timeouts);
		}
		APP(",\"age_ms\":%llu", (unsigned long long)age_ms);
		if (t->stats_local_port)
			APP(",\"listen_port\":%u", (unsigned)t->stats_local_port);
		APP("}");
	}
	APP("}");
	return p;
	#undef APP
}

static int json_emit_status(char *buf, size_t cap, const Config *c, uint64_t up_us)
{
	int p = 0;
	#define APP(...) do { int _w = snprintf(buf+p, cap-p, __VA_ARGS__); \
	                      if (_w < 0 || (size_t)_w >= cap-(size_t)p) return -1; \
	                      p += _w; } while (0)
	APP("{\"uptime_s\":%.1f,\"tunnels\":[", (double)up_us / 1e6);
	for (int i = 0; i < c->tunnel_count; i++) {
		if (i) APP(",");
		int n = json_emit_tunnel(buf + p, cap - p, &c->tunnels[i], false);
		if (n < 0) return -1;
		p += n;
	}
	APP("]}");
	return p;
	#undef APP
}

static Tunnel *cfg_find_tunnel(Config *c, const char *name)
{
	for (int i = 0; i < c->tunnel_count; i++)
		if (!strcmp(c->tunnels[i].name, name)) return &c->tunnels[i];
	return NULL;
}

/* Routes:
 *   GET /api/v1/health
 *   GET /api/v1/status
 *   GET /api/v1/tunnels
 *   GET /api/v1/tunnels/{name}
 *   GET /api/v1/tunnels/{name}/start
 *   GET /api/v1/tunnels/{name}/stop
 *   GET /api/v1/tunnels/{name}/restart
 *   GET /api/v1/tunnels/{name}/control?cmd=set_fec&k=1&n=2[&fec_timeout_ms=50]
 *   GET /api/v1/tunnels/{name}/control?cmd=set_radio&stbc=&ldpc=&short_gi=&bandwidth=&mcs_index=&vht_mode=&vht_nss=
 *   GET /api/v1/tunnels/{name}/control?cmd=get_fec
 *   GET /api/v1/tunnels/{name}/control?cmd=get_radio
 */
static void api_handle(ApiClient *cli, Config *c, uint64_t startup_us)
{
	cli->buf[cli->pos < API_BUF_BYTES ? cli->pos : API_BUF_BYTES - 1] = 0;

	if (strncmp(cli->buf, "GET ", 4) != 0) {
		api_send(cli->fd, 405, "text/plain", "expected GET\n", -1);
		return;
	}
	bool wants_html = request_wants_html(cli->buf);
	char *path = cli->buf + 4;
	char *sp = strchr(path, ' ');
	if (!sp) { api_send(cli->fd, 400, "text/plain", "bad request\n", -1); return; }
	*sp = 0;
	char *qs = strchr(path, '?');
	const char *qstr = NULL;
	if (qs) { *qs = 0; qstr = qs + 1; }

	char body[8192];
	int n;

	if (!strcmp(path, "/")) {
		if (wants_html) {
			api_send_blob(cli->fd, "text/html; charset=utf-8",
			              gs_webui_html, gs_webui_html_len);
		} else {
			api_send(cli->fd, 200, "text/plain",
			    "gs_supervisor — see /api/v1/health|status|tunnels|cmd. "
			    "Open this URL in a browser for the WebUI.\n", -1);
		}
		return;
	}
	if (!strcmp(path, "/api/v1/health")) {
		api_send(cli->fd, 200, "text/plain", "ok\n", -1);
		return;
	}
	if (!strcmp(path, "/api/v1/status")) {
		n = json_emit_status(body, sizeof(body), c, now_us() - startup_us);
		if (n < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
		api_send(cli->fd, 200, "application/json", body, n);
		return;
	}
	if (!strcmp(path, "/api/v1/tunnels")) {
		int p = 0;
		p += snprintf(body + p, sizeof(body) - p, "[");
		for (int i = 0; i < c->tunnel_count; i++) {
			if (i) p += snprintf(body + p, sizeof(body) - p, ",");
			int w = json_emit_tunnel(body + p, sizeof(body) - p,
			                         &c->tunnels[i], true);
			if (w < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
			p += w;
		}
		p += snprintf(body + p, sizeof(body) - p, "]");
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/reinit")) {
		/* Last-resort recovery: stop every tunnel, run system.down, then
		 * system.up + wait_iface_state, then respawn autostart tunnels.
		 * This unsticks adapters that are stuck in a bad post-monitor
		 * state (txpower=-100, NO-CARRIER while in monitor mode, etc.) —
		 * the symptoms we saw when WCMD packets stopped getting on-air. */
		LOG_WARN("REST /system/reinit: tearing down all tunnels");
		for (int i = 0; i < c->tunnel_count; i++) {
			Tunnel *t = &c->tunnels[i];
			if (t->pid > 0) {
				t->autostart_on_exit = false;
				if (kill(t->pid, SIGTERM) == 0)
					t->stop_deadline_ms = now_ms() + GS_STOP_GRACE_MS;
			}
		}
		uint64_t deadline = now_ms() + GS_STOP_GRACE_MS + 500;
		while (now_ms() < deadline) {
			bool any = false;
			for (int i = 0; i < c->tunnel_count; i++)
				if (c->tunnels[i].pid > 0) any = true;
			if (!any) break;
			supervisor_reap(c);
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		for (int i = 0; i < c->tunnel_count; i++) {
			if (c->tunnels[i].pid > 0) {
				kill(c->tunnels[i].pid, SIGKILL);
				waitpid(c->tunnels[i].pid, NULL, 0);
				c->tunnels[i].pid = -1;
			}
		}
		/* Stats listeners stay open across reinit — wfb_rx/wfb_tx will
		 * be respawned with the same -Y target. */
		LOG_INFO("REST /system/reinit: running system.down");
		run_system_block("system.down", c->system_down, c->system_down_count);
		LOG_INFO("REST /system/reinit: running system.up");
		run_system_block("system.up",   c->system_up,   c->system_up_count);
		int wait_rc = wait_iface_state(c, 5000, 100);
		if (wait_rc != 0) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"iface readiness timeout — check supervisor log\"}", -1);
			return;
		}
		int spawned = 0;
		for (int i = 0; i < c->tunnel_count; i++) {
			Tunnel *t = &c->tunnels[i];
			if (t->autostart) {
				t->backoff_idx = 0;
				t->next_start_ms = 0;
				if (tunnel_spawn(t, c->key_file) == 0) spawned++;
			}
		}
		char body[160];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":true,\"reinit\":true,\"tunnels_spawned\":%d}",
		                 spawned);
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/cmd")) {
		size_t kl;
		const char *kstr = qs_get(qstr, "key", &kl);
		if (!kstr) {
			api_send(cli->fd, 400, "text/plain", "missing ?key=\n", -1);
			return;
		}
		int key = wcmd_key_from_str(kstr, kl);
		if (key < 0) {
			api_send(cli->fd, 400, "text/plain",
			         "key must be bitrate_kbps|fps|payload_bytes|force_idr|"
			         "wfb_fec_k|wfb_fec_n|wfb_mcs|wfb_bandwidth|"
			         "wfb_ldpc|wfb_stbc|wfb_short_gi|"
			         "fec_enabled|mcs_enabled\n", -1);
			return;
		}
		int32_t value = 0;
		int v;
		if (qs_get_int(qstr, "value", &v) == 0) value = (int32_t)v;
		else if (key != WCMD_KEY_FORCE_IDR) {
			api_send(cli->fd, 400, "text/plain",
			         "this key requires ?value=\n", -1);
			return;
		}
		/* Rate limit per key. */
		if (c->venc_cmd_rate_limit_ms > 0 && key >= 1 && key <= WCMD_KEY_MAX) {
			uint64_t t_ms = now_ms();
			uint64_t since = t_ms - g_wcmd_last_send_ms[key];
			if (g_wcmd_last_send_ms[key] != 0 &&
			    (int)since < c->venc_cmd_rate_limit_ms) {
				char body[128];
				int p = snprintf(body, sizeof(body),
				                 "{\"ok\":false,\"error\":\"rate_limited\","
				                 "\"key\":\"%s\",\"retry_after_ms\":%d}",
				                 wcmd_key_name(key),
				                 c->venc_cmd_rate_limit_ms - (int)since);
				api_send(cli->fd, 429, "application/json", body, p);
				return;
			}
		}
		uint16_t seq = 0;
		int rc = wcmd_emit(c, key, value, &seq);
		if (rc == -1) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"venc_cmd disabled or uplink missing\"}",
			         -1);
			return;
		}
		if (rc != 0) {
			char body[128];
			int p = snprintf(body, sizeof(body),
			                 "{\"ok\":false,\"error\":\"sendto: %s\"}",
			                 strerror(rc));
			api_send(cli->fd, 502, "application/json", body, p);
			return;
		}
		g_wcmd_last_send_ms[key] = now_ms();
		char body[128];
		int p;
		if (key == WCMD_KEY_FORCE_IDR) {
			p = snprintf(body, sizeof(body),
			             "{\"ok\":true,\"seq\":%u,\"key\":\"%s\"}",
			             (unsigned)seq, wcmd_key_name(key));
		} else {
			p = snprintf(body, sizeof(body),
			             "{\"ok\":true,\"seq\":%u,\"key\":\"%s\",\"value\":%d}",
			             (unsigned)seq, wcmd_key_name(key), (int)value);
		}
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strncmp(path, "/api/v1/tunnels/", 16)) {
		char name[GS_NAME_MAX];
		const char *rest = path + 16;
		const char *slash = strchr(rest, '/');
		size_t nlen = slash ? (size_t)(slash - rest) : strlen(rest);
		if (nlen == 0 || nlen >= sizeof(name)) {
			api_send(cli->fd, 400, "text/plain", "bad name\n", -1); return;
		}
		memcpy(name, rest, nlen); name[nlen] = 0;
		Tunnel *t = cfg_find_tunnel(c, name);
		if (!t) { api_send(cli->fd, 404, "text/plain", "no such tunnel\n", -1); return; }
		const char *action = slash ? slash + 1 : "";
		if (!*action) {
			n = json_emit_tunnel(body, sizeof(body), t, true);
			if (n < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
			api_send(cli->fd, 200, "application/json", body, n);
			return;
		}
		if (!strcmp(action, "start")) {
			if (t->pid > 0) {
				api_send(cli->fd, 200, "application/json",
				         "{\"ok\":true,\"already\":true}", -1);
				return;
			}
			t->backoff_idx = 0;
			t->next_start_ms = 0;
			tunnel_spawn(t, c->key_file);
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "stop")) {
			tunnel_request_stop(t);
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "restart")) {
			tunnel_request_stop(t);
			t->autostart_on_exit = true;
			t->backoff_idx = 0;
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "control")) {
			if (strcmp(t->role, "tx") != 0) {
				api_send(cli->fd, 400, "text/plain",
				         "control only valid on tx tunnels\n", -1);
				return;
			}
			if (t->control_port <= 0) {
				api_send(cli->fd, 409, "text/plain",
				         "tunnel has no control_port set; add -C in config\n", -1);
				return;
			}
			size_t cl;
			const char *cmd = qs_get(qstr, "cmd", &cl);
			if (!cmd) {
				api_send(cli->fd, 400, "text/plain", "missing ?cmd=\n", -1);
				return;
			}
			char respbuf[64];
			uint32_t rc_out = 0xFFFFFFFFu;
			int rlen = 0;

			if (cl == 7 && !strncmp(cmd, "set_fec", 7)) {
				int k = -1, nn = -1, to = WFB_FEC_TIMEOUT_KEEP;
				if (qs_get_int(qstr, "k", &k) < 0 ||
				    qs_get_int(qstr, "n", &nn) < 0 ||
				    k < 0 || k > 255 || nn < 0 || nn > 255) {
					api_send(cli->fd, 400, "text/plain",
					         "set_fec needs k,n in [0,255]\n", -1);
					return;
				}
				int to_arg;
				if (qs_get_int(qstr, "fec_timeout_ms", &to_arg) == 0) {
					if (to_arg < 0 || to_arg > 0xFFFE) {
						api_send(cli->fd, 400, "text/plain",
						         "fec_timeout_ms out of range\n", -1);
						return;
					}
					to = to_arg;
				}
				WfbCmdSetFecReq req = {
					.req_id         = wfb_cmd_next_req_id(),
					.cmd_id         = WFB_CMD_SET_FEC,
					.k              = (uint8_t)k,
					.n              = (uint8_t)nn,
					.fec_timeout_ms = htons((uint16_t)to),
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
				if (rlen > 0 && rc_out == 0) {
					t->fec_k = k;
					t->fec_n = nn;
					if (to != WFB_FEC_TIMEOUT_KEEP) t->fec_timeout_ms = to;
					t->fec_cache_have = 1;
				}
			} else if (cl == 9 && !strncmp(cmd, "set_radio", 9)) {
				int stbc=0, ldpc=0, sgi=0, bw=20, mcs=1, vhtm=0, vhtn=1;
				qs_get_int(qstr, "stbc",      &stbc);
				qs_get_int(qstr, "ldpc",      &ldpc);
				qs_get_int(qstr, "short_gi",  &sgi);
				qs_get_int(qstr, "bandwidth", &bw);
				qs_get_int(qstr, "mcs_index", &mcs);
				qs_get_int(qstr, "vht_mode",  &vhtm);
				qs_get_int(qstr, "vht_nss",   &vhtn);
				WfbCmdSetRadioReq req = {
					.req_id    = wfb_cmd_next_req_id(),
					.cmd_id    = WFB_CMD_SET_RADIO,
					.stbc      = (uint8_t)stbc,
					.ldpc      = (uint8_t)(ldpc != 0),
					.short_gi  = (uint8_t)(sgi  != 0),
					.bandwidth = (uint8_t)bw,
					.mcs_index = (uint8_t)mcs,
					.vht_mode  = (uint8_t)(vhtm != 0),
					.vht_nss   = (uint8_t)vhtn,
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
				if (rlen > 0 && rc_out == 0) {
					t->stbc           = stbc;
					t->ldpc           = (ldpc != 0);
					t->short_gi       = (sgi  != 0);
					t->bandwidth_mhz  = bw;
					t->mcs_index      = mcs;
					t->vht_mode       = (vhtm != 0);
					t->vht_nss        = vhtn;
					t->radio_cache_have = 1;
					t->radio_cache_us   = now_us();
				}
			} else if ((cl == 7 && !strncmp(cmd, "get_fec",   7)) ||
			           (cl == 9 && !strncmp(cmd, "get_radio", 9))) {
				uint8_t cmd_id = (cl == 7) ? WFB_CMD_GET_FEC : WFB_CMD_GET_RADIO;
				WfbCmdGetReq req = {
					.req_id = wfb_cmd_next_req_id(),
					.cmd_id = cmd_id,
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
			} else {
				api_send(cli->fd, 400, "text/plain",
				         "cmd must be set_fec|set_radio|get_fec|get_radio\n", -1);
				return;
			}

			if (rlen < 0) {
				api_send(cli->fd, 502, "application/json",
				         "{\"ok\":false,\"error\":\"sendto failed\"}", -1);
				return;
			}
			if (rlen == 0) {
				api_send(cli->fd, 504, "application/json",
				         "{\"ok\":false,\"error\":\"timeout\"}", -1);
				return;
			}
			/* JSON-emit the response. Body interpretation depends on cmd. */
			char body2[256];
			int p = snprintf(body2, sizeof(body2),
			                 "{\"ok\":%s,\"rc\":%u",
			                 rc_out == 0 ? "true" : "false", rc_out);
			if (rc_out == 0) {
				if (cl == 7 && !strncmp(cmd, "get_fec", 7) &&
				    rlen >= (int)sizeof(WfbCmdGetFecResp)) {
					WfbCmdGetFecResp gf;
					memcpy(&gf, respbuf, sizeof(gf));
					p += snprintf(body2 + p, sizeof(body2) - p,
					              ",\"k\":%u,\"n\":%u,\"fec_timeout_ms\":%u",
					              gf.k, gf.n, ntohs(gf.fec_timeout_ms));
					/* Refresh runtime cache on the way through. */
					t->fec_k          = gf.k;
					t->fec_n          = gf.n;
					t->fec_timeout_ms = ntohs(gf.fec_timeout_ms);
					t->fec_cache_have = 1;
				} else if (cl == 9 && !strncmp(cmd, "get_radio", 9) &&
				           rlen >= (int)sizeof(WfbCmdGetRadioResp)) {
					WfbCmdGetRadioResp gr;
					memcpy(&gr, respbuf, sizeof(gr));
					p += snprintf(body2 + p, sizeof(body2) - p,
					              ",\"stbc\":%u,\"ldpc\":%u,\"short_gi\":%u,"
					              "\"bandwidth\":%u,\"mcs_index\":%u,"
					              "\"vht_mode\":%u,\"vht_nss\":%u",
					              gr.stbc, gr.ldpc, gr.short_gi,
					              gr.bandwidth, gr.mcs_index,
					              gr.vht_mode, gr.vht_nss);
					t->stbc             = gr.stbc;
					t->ldpc             = gr.ldpc;
					t->short_gi         = gr.short_gi;
					t->bandwidth_mhz    = gr.bandwidth;
					t->mcs_index        = gr.mcs_index;
					t->vht_mode         = gr.vht_mode;
					t->vht_nss          = gr.vht_nss;
					t->radio_cache_have = 1;
					t->radio_cache_us   = now_us();
				}
			}
			snprintf(body2 + p, sizeof(body2) - p, "}");
			api_send(cli->fd, 200, "application/json", body2, -1);
			return;
		}
		api_send(cli->fd, 404, "text/plain", "unknown action\n", -1);
		return;
	}

	api_send(cli->fd, 404, "text/plain", "not found\n", -1);
}

/* ---------- main loop ------------------------------------------------ */

static void shutdown_all(Config *c)
{
	for (int i = 0; i < c->tunnel_count; i++) {
		Tunnel *t = &c->tunnels[i];
		if (t->pid > 0) {
			kill(t->pid, SIGTERM);
			t->stop_deadline_ms = now_ms() + GS_STOP_GRACE_MS;
			t->autostart_on_exit = false;
		}
	}
	uint64_t deadline = now_ms() + GS_STOP_GRACE_MS + 500;
	while (now_ms() < deadline) {
		bool any = false;
		for (int i = 0; i < c->tunnel_count; i++)
			if (c->tunnels[i].pid > 0) any = true;
		if (!any) break;
		supervisor_reap(c);
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	for (int i = 0; i < c->tunnel_count; i++) {
		if (c->tunnels[i].pid > 0) {
			kill(c->tunnels[i].pid, SIGKILL);
			waitpid(c->tunnels[i].pid, NULL, 0);
			c->tunnels[i].pid = -1;
		}
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	    "usage: %s [--config PATH] [--port N] [-v]\n"
	    "  -c, --config PATH   JSON config (default %s)\n"
	    "  -p, --port N        override http.port from config\n"
	    "  -v, --verbose       inherit wfb_rx/wfb_tx stdout/stderr (default: /dev/null)\n",
	    argv0, GS_DEFAULT_CONFIG);
}

int main(int argc, char **argv)
{
	const char *cfg_path = GS_DEFAULT_CONFIG;
	int port_override = -1;

	static const struct option longopts[] = {
		{ "config",  required_argument, 0, 'c' },
		{ "port",    required_argument, 0, 'p' },
		{ "verbose", no_argument,       0, 'v' },
		{ "help",    no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	for (int o; (o = getopt_long(argc, argv, "c:p:vh", longopts, NULL)) != -1;) {
		switch (o) {
		case 'c': cfg_path = optarg; break;
		case 'p': port_override = atoi(optarg); break;
		case 'v': g_verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	Config cfg;
	if (cfg_load(cfg_path, &cfg) < 0) return 1;
	if (port_override > 0 && port_override < 65536) cfg.http_port = (uint16_t)port_override;

	LOG_INFO("loaded config: %d tunnel(s), http=%s:%u",
	         cfg.tunnel_count, cfg.http_bind, cfg.http_port);
	for (int i = 0; i < cfg.tunnel_count; i++) {
		Tunnel *t = &cfg.tunnels[i];
		LOG_INFO("  [%d] %s role=%s link=%d port=%d ifaces=%d autostart=%d",
		         i, t->name, t->role, t->link_id, t->radio_port,
		         t->iface_count, (int)t->autostart);
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	run_system_block("system.up", cfg.system_up, cfg.system_up_count);

	/* Drivers (notably rtl88xxau and friends) take a variable amount of
	 * time to flip an iface UP after `iw set monitor otherbss` — fixed
	 * sleeps in system.up are fragile.  Poll up to 5 s before giving up. */
	if (wait_iface_state(&cfg, 5000, 100) != 0) {
		/* Bring back what system.up touched, then exit. The operator
		 * needs to fix the bring-up sequence (typo'd iface name, missing
		 * sudo, kernel module not loaded, etc.) before we can serve. */
		run_system_block("system.down", cfg.system_down, cfg.system_down_count);
		return 1;
	}

	int api_fd = api_listen_open(cfg.http_bind, cfg.http_port);
	if (api_fd < 0) {
		LOG_ERR("api listen %s:%u: %s", cfg.http_bind, cfg.http_port, strerror(errno));
		return 1;
	}
	LOG_INFO("REST API listening on http://%s:%u", cfg.http_bind, cfg.http_port);

	ApiClient clients[API_MAX_CLIENTS];
	for (int i = 0; i < API_MAX_CLIENTS; i++) clients[i].fd = -1;

	uint64_t startup_us = now_us();

	for (int i = 0; i < cfg.tunnel_count; i++) {
		if (cfg.tunnels[i].autostart)
			tunnel_spawn(&cfg.tunnels[i], cfg.key_file);
	}

	while (!g_shutdown) {
		/* pfd_slot encodes:
		 *   -1  : api listen socket (slot 0)
		 *   0..API_MAX_CLIENTS-1 : api client at clients[idx]
		 *   -1000-i : rx tunnel stats listener for tunnel i  */
		#define GS_SLOT_LISTEN  (-1)
		#define GS_SLOT_STATS_BASE (-1000)
		struct pollfd pfds[1 + API_MAX_CLIENTS + GS_MAX_TUNNELS];
		int           pfd_slot[1 + API_MAX_CLIENTS + GS_MAX_TUNNELS];
		int nfds = 0;
		pfds[nfds].fd = api_fd; pfds[nfds].events = POLLIN;
		pfd_slot[nfds] = GS_SLOT_LISTEN;
		nfds++;
		for (int i = 0; i < API_MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				pfds[nfds].fd = clients[i].fd;
				pfds[nfds].events = POLLIN;
				pfd_slot[nfds] = i;
				nfds++;
			}
		}
		for (int i = 0; i < cfg.tunnel_count; i++) {
			if (cfg.tunnels[i].stats_local_fd >= 0) {
				pfds[nfds].fd = cfg.tunnels[i].stats_local_fd;
				pfds[nfds].events = POLLIN;
				pfd_slot[nfds] = GS_SLOT_STATS_BASE - i;
				nfds++;
			}
		}

		int r = poll(pfds, nfds, 250);
		/* Capture poll's errno before subsequent syscalls clobber it
		 * (waitpid in supervisor_reap leaves ECHILD on success). */
		int poll_err = (r < 0) ? errno : 0;

		if (g_sigchld) { g_sigchld = 0; supervisor_reap(&cfg); }
		supervisor_tick(&cfg);

		if (r < 0) {
			if (poll_err == EINTR) continue;
			LOG_ERR("poll: %s", strerror(poll_err));
			break;
		}
		if (r == 0) continue;

		if (pfds[0].revents & POLLIN) {
			struct sockaddr_in peer; socklen_t plen = sizeof(peer);
			int cfd = accept(api_fd, (struct sockaddr *)&peer, &plen);
			if (cfd >= 0) {
				int slot = -1;
				for (int i = 0; i < API_MAX_CLIENTS; i++)
					if (clients[i].fd < 0) { slot = i; break; }
				if (slot < 0) {
					close(cfd);
				} else {
					int fl = fcntl(cfd, F_GETFL, 0);
					if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
					clients[slot].fd = cfd;
					clients[slot].pos = 0;
					clients[slot].accepted_us = now_us();
				}
			}
		}
		/* Iterate over the pfds we actually polled, not over slot indices.
		 * A client just accepted in this iteration is NOT in pfds; reading
		 * pfds[idx] for it was an out-of-bounds garbage fetch that could
		 * trigger a spurious recv() returning EAGAIN, then close the fresh
		 * socket — symptom: "Empty reply from server". */
		for (int p = 1; p < nfds; p++) {
			int slot = pfd_slot[p];
			short re = pfds[p].revents;
			if (!(re & (POLLIN | POLLERR | POLLHUP))) continue;

			if (slot <= GS_SLOT_STATS_BASE) {
				int ti = GS_SLOT_STATS_BASE - slot;
				if (ti >= 0 && ti < cfg.tunnel_count)
					stats_drain(&cfg.tunnels[ti]);
				continue;
			}

			int i = slot;
			if (i < 0 || clients[i].fd < 0) continue;
			ssize_t got = recv(clients[i].fd,
			                   clients[i].buf + clients[i].pos,
			                   sizeof(clients[i].buf) - clients[i].pos - 1, 0);
			if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
			if (got <= 0) {
				close(clients[i].fd); clients[i].fd = -1; continue;
			}
			clients[i].pos += (size_t)got;
			clients[i].buf[clients[i].pos] = 0;
			if (strstr(clients[i].buf, "\r\n\r\n") || strstr(clients[i].buf, "\n\n")) {
				api_handle(&clients[i], &cfg, startup_us);
				close(clients[i].fd); clients[i].fd = -1;
			} else if (clients[i].pos >= sizeof(clients[i].buf) - 1) {
				api_send(clients[i].fd, 400, "text/plain", "request too long\n", -1);
				close(clients[i].fd); clients[i].fd = -1;
			}
		}
		#undef GS_SLOT_LISTEN
		#undef GS_SLOT_STATS_BASE
	}

	LOG_INFO("shutdown signal received");
	for (int i = 0; i < API_MAX_CLIENTS; i++)
		if (clients[i].fd >= 0) close(clients[i].fd);
	close(api_fd);
	shutdown_all(&cfg);
	for (int i = 0; i < cfg.tunnel_count; i++)
		stats_listener_close(&cfg.tunnels[i]);
	run_system_block("system.down", cfg.system_down, cfg.system_down_count);
	LOG_INFO("bye");
	return 0;
}
