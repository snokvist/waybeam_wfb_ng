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
#define GS_MAX_SYSTEM_CMDS   8
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
	int         link_id;
	int         radio_port;

	int         iface_count;
	char        ifaces[GS_MAX_IFACES][IFNAMSIZ];

	/* rx-only */
	char        udp_out_ip[GS_ARG_MAX];
	int         udp_out_port;
	char        stats_out[GS_ARG_MAX]; /* "ip:port" or empty */

	/* tx-only */
	int         udp_in_port;
	int         control_port;
	int         fec_k, fec_n;
	int         bandwidth_mhz;
	int         mcs_index;
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
	t->stbc = -1; t->ldpc = -1;
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

	if (!strcmp(t->role, "rx")) {
		if ((v = jfind(js, toks, n, t_idx, "udp_out")) < 0) return -1;
		if (jstr(js, &toks[v], tmp, sizeof(tmp)) < 0) return -1;
		char *colon = strchr(tmp, ':');
		if (!colon) return -1;
		*colon = 0;
		snprintf(t->udp_out_ip, sizeof(t->udp_out_ip), "%s", tmp);
		t->udp_out_port = atoi(colon + 1);
		if (t->udp_out_port <= 0 || t->udp_out_port > 65535) return -1;

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
			if (kids > GS_MAX_SYSTEM_CMDS) kids = GS_MAX_SYSTEM_CMDS;
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
			if (kids > GS_MAX_SYSTEM_CMDS) kids = GS_MAX_SYSTEM_CMDS;
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
	if (ab_push(ab, "wfb_rx") < 0) return -1;
	if (key_file && key_file[0] && (ab_push(ab, "-K") < 0 || ab_push(ab, "%s", key_file) < 0))
		return -1;
	if (ab_push(ab, "-c") < 0 || ab_push(ab, "%s", t->udp_out_ip) < 0) return -1;
	if (ab_push(ab, "-u") < 0 || ab_push(ab, "%d", t->udp_out_port) < 0) return -1;
	if (ab_push(ab, "-i") < 0 || ab_push(ab, "%d", t->link_id) < 0) return -1;
	if (ab_push(ab, "-p") < 0 || ab_push(ab, "%d", t->radio_port) < 0) return -1;
	if (t->stats_out[0]) {
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
	if (ab_push(ab, "wfb_tx") < 0) return -1;
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
	for (int i = 0; i < t->extra_arg_count; i++)
		if (ab_push(ab, "%s", t->extra_args[i]) < 0) return -1;
	for (int i = 0; i < t->iface_count; i++)
		if (ab_push(ab, "%s", t->ifaces[i]) < 0) return -1;
	ab->argv[ab->argc] = NULL;
	return 0;
}

/* ---------- supervisor ----------------------------------------------- */

static int tunnel_spawn(Tunnel *t, const char *key_file)
{
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
		execvp(ab.argv[0], ab.argv);
		fprintf(stderr, "[gs] exec %s: %s\n", ab.argv[0], strerror(errno));
		_exit(127);
	}

	t->pid           = pid;
	t->state         = TS_STARTING;
	t->started_us    = now_us();
	t->autostart_on_exit = true;
	t->stop_deadline_ms  = 0;

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

/* Periodic tick: handle SIGKILL escalation + backoff respawn. */
static void supervisor_tick(Config *c)
{
	uint64_t t_ms = now_ms();
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
		if (!strcmp(t->role, "rx")) {
			APP(",\"udp_out\":\"%s:%d\"", t->udp_out_ip, t->udp_out_port);
			if (t->stats_out[0]) APP(",\"stats_out\":\"%s\"", t->stats_out);
		} else {
			APP(",\"udp_in_port\":%d,\"control_port\":%d,"
			    "\"fec\":{\"k\":%d,\"n\":%d},"
			    "\"radio\":{\"bw\":%d,\"mcs\":%d,\"stbc\":%d,\"ldpc\":%d}",
			    t->udp_in_port, t->control_port,
			    t->fec_k, t->fec_n,
			    t->bandwidth_mhz, t->mcs_index, t->stbc, t->ldpc);
		}
		APP(",\"autostart\":%s", t->autostart ? "true" : "false");
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
 */
static void api_handle(ApiClient *cli, Config *c, uint64_t startup_us)
{
	cli->buf[cli->pos < API_BUF_BYTES ? cli->pos : API_BUF_BYTES - 1] = 0;

	if (strncmp(cli->buf, "GET ", 4) != 0) {
		api_send(cli->fd, 405, "text/plain", "expected GET\n", -1);
		return;
	}
	char *path = cli->buf + 4;
	char *sp = strchr(path, ' ');
	if (!sp) { api_send(cli->fd, 400, "text/plain", "bad request\n", -1); return; }
	*sp = 0;
	char *qs = strchr(path, '?');
	if (qs) *qs = 0;

	char body[8192];
	int n;

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
	    "usage: %s [--config PATH] [--port N]\n"
	    "  --config PATH   JSON config (default %s)\n"
	    "  --port N        override http.port from config\n",
	    argv0, GS_DEFAULT_CONFIG);
}

int main(int argc, char **argv)
{
	const char *cfg_path = GS_DEFAULT_CONFIG;
	int port_override = -1;

	static const struct option longopts[] = {
		{ "config", required_argument, 0, 'c' },
		{ "port",   required_argument, 0, 'p' },
		{ "help",   no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	for (int o; (o = getopt_long(argc, argv, "c:p:h", longopts, NULL)) != -1;) {
		switch (o) {
		case 'c': cfg_path = optarg; break;
		case 'p': port_override = atoi(optarg); break;
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
		struct pollfd pfds[1 + API_MAX_CLIENTS];
		int nfds = 0;
		pfds[nfds].fd = api_fd; pfds[nfds].events = POLLIN; nfds++;
		for (int i = 0; i < API_MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				pfds[nfds].fd = clients[i].fd;
				pfds[nfds].events = POLLIN;
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

		int idx = 0;
		if (pfds[idx++].revents & POLLIN) {
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
		for (int i = 0; i < API_MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) continue;
			if (!(pfds[idx].revents & (POLLIN | POLLERR | POLLHUP))) { idx++; continue; }
			idx++;
			ssize_t got = recv(clients[i].fd,
			                   clients[i].buf + clients[i].pos,
			                   sizeof(clients[i].buf) - clients[i].pos - 1, 0);
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
	}

	LOG_INFO("shutdown signal received");
	for (int i = 0; i < API_MAX_CLIENTS; i++)
		if (clients[i].fd >= 0) close(clients[i].fd);
	close(api_fd);
	shutdown_all(&cfg);
	run_system_block("system.down", cfg.system_down, cfg.system_down_count);
	LOG_INFO("bye");
	return 0;
}
