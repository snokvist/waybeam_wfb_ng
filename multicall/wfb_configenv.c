/* wfb_configenv.c — `config-env` applet. See wfb_configenv.h. */
#include "wfb_configenv.h"
#include "wfb_keyseed.h"   /* WFB_ROLE_DRONE / WFB_ROLE_GS */
#include "wfb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIGENV_DEFAULT_PATH "/etc/wfb-link.json"
#define CONFIGENV_MAX_BYTES    (64 * 1024)

/* --- field getters (section.key with default), tolerant of a missing file --- */

static long ce_int(const char *js, JTok *t, int n, const char *sec, const char *key, long def)
{
	if (!js) return def;
	int s = jfind(js, t, n, 0, sec);
	if (s < 0) return def;
	int v = jfind(js, t, n, s, key);
	if (v < 0) return def;
	long out;
	return jint(js, &t[v], &out) == 0 ? out : def;
}

/* bool with int tolerance: true/false, or any nonzero/zero integer. */
static int ce_bool01(const char *js, JTok *t, int n, const char *sec, const char *key, int def)
{
	if (!js) return def;
	int s = jfind(js, t, n, 0, sec);
	if (s < 0) return def;
	int v = jfind(js, t, n, s, key);
	if (v < 0) return def;
	bool b;
	if (jbool(js, &t[v], &b) == 0) return b ? 1 : 0;
	long iv;
	if (jint(js, &t[v], &iv) == 0) return iv != 0 ? 1 : 0;
	return def;
}

static void ce_str(const char *js, JTok *t, int n, const char *sec, const char *key,
                   const char *def, char *out, size_t cap)
{
	snprintf(out, cap, "%s", def);
	if (!js) return;
	int s = jfind(js, t, n, 0, sec);
	if (s < 0) return;
	int v = jfind(js, t, n, s, key);
	if (v < 0) return;
	char tmp[256];
	if (jstr(js, &t[v], tmp, sizeof(tmp)) >= 0 && tmp[0])
		snprintf(out, cap, "%s", tmp);
}

/* Raw primitive text (for fractional numbers like failsafe seconds, which
 * jint would truncate and jstr rejects as non-string). Copies the literal
 * token bytes, e.g. JSON `2.5` -> "2.5". Falls back to def for any other
 * token type or a missing key. */
static void ce_prim(const char *js, JTok *t, int n, const char *sec, const char *key,
                    const char *def, char *out, size_t cap)
{
	snprintf(out, cap, "%s", def);
	if (!js) return;
	int s = jfind(js, t, n, 0, sec);
	if (s < 0) return;
	int v = jfind(js, t, n, s, key);
	if (v < 0) return;
	if (t[v].type != JT_PRIM) return;
	int len = t[v].end - t[v].start;
	if (len <= 0 || (size_t)len >= cap) return;
	memcpy(out, js + t[v].start, (size_t)len);
	out[len] = 0;
}

/* --- shell-safe emitters --- */

static void emit_int(const char *name, long v)  { printf("%s=%ld\n", name, v); }
static void emit_b01(const char *name, int v)    { printf("%s=%d\n", name, v ? 1 : 0); }

/* single-quote the value so `eval` can never run embedded shell. */
static void emit_str(const char *name, const char *v)
{
	printf("%s='", name);
	for (const char *p = v; *p; p++) {
		if (*p == '\'') fputs("'\\''", stdout);
		else            putchar(*p);
	}
	fputs("'\n", stdout);
}

int wfb_configenv_main(int argc, char **argv, int role)
{
	const char *path = CONFIGENV_DEFAULT_PATH;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			fprintf(stderr, "config-env: unknown option %s\n", argv[i]);
			return 2;
		}
		path = argv[i];  /* last positional wins */
	}

	char  *buf = NULL;
	JTok   toks[JSON_MAX_TOKS];
	const char *js = NULL;   /* stays NULL -> pure defaults */
	int    ntok = 0;
	const char *src = "built-in defaults";

	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "config-env: %s not found — using preset defaults\n", path);
	} else {
		buf = (char *)malloc(CONFIGENV_MAX_BYTES + 1);
		if (!buf) { fclose(fp); fprintf(stderr, "config-env: out of memory\n"); return 1; }
		size_t len = fread(buf, 1, CONFIGENV_MAX_BYTES, fp);
		int truncated = !feof(fp);
		fclose(fp);
		buf[len] = 0;
		if (truncated) {
			fprintf(stderr, "config-env: %s larger than %d bytes — using defaults\n",
			        path, CONFIGENV_MAX_BYTES);
		} else {
			ntok = json_parse(buf, (int)len, toks, JSON_MAX_TOKS);
			if (ntok < 1 || toks[0].type != JT_OBJ) {
				fprintf(stderr, "config-env: %s is not valid JSON object — using defaults\n", path);
			} else {
				js = buf;
				src = path;
			}
		}
	}

	const char *key_def = (role == WFB_ROLE_GS) ? "/etc/gs.key" : "/etc/drone.key";
	char s[256];

	printf("# wfb-link preset (source: %s)\n", src);
	emit_int("WFB_CHANNEL", ce_int(js, toks, ntok, "radio", "channel", 161));
	ce_str(js, toks, ntok, "radio", "htmode", "HT20", s, sizeof(s)); emit_str("WFB_HTMODE", s);
	emit_int("WFB_BW",      ce_int(js, toks, ntok, "radio", "bw", 20));
	emit_int("WFB_TXPOWER", ce_int(js, toks, ntok, "radio", "txpower_mbm", 2000));
	ce_str(js, toks, ntok, "key", "file", key_def, s, sizeof(s)); emit_str("KEY", s);
	ce_str(js, toks, ntok, "key", "seed", "Waybeam", s, sizeof(s)); emit_str("WFB_KEY_SEED", s);
	emit_int("WFB_TX_LINK",    ce_int(js, toks, ntok, "links", "video", 207));
	emit_int("WFB_RX_LINK",    ce_int(js, toks, ntok, "links", "uplink", 208));
	emit_int("WFB_PROBE_LINK", ce_int(js, toks, ntok, "links", "probe", 50));
	emit_int("WFB_K", ce_int(js, toks, ntok, "fec", "k", 8));
	emit_int("WFB_N", ce_int(js, toks, ntok, "fec", "n", 12));
	emit_int("WFB_MCS",     ce_int(js, toks, ntok, "mcs", "boot", 2));
	emit_int("WFB_MCS_MIN", ce_int(js, toks, ntok, "mcs", "min", 1));
	emit_int("WFB_MCS_MAX", ce_int(js, toks, ntok, "mcs", "max", 7));
	emit_b01("WFB_PROBE", ce_bool01(js, toks, ntok, "probe", "enabled", 1));
	ce_str(js, toks, ntok, "peek", "profile", "close", s, sizeof(s)); emit_str("WFB_PEEK_PROFILE", s);
	emit_b01("WFB_LOG", ce_bool01(js, toks, ntok, "log", "enabled", 1));
	ce_str(js, toks, ntok, "venc", "shm", "local_shm", s, sizeof(s)); emit_str("SHM_RING", s);
	emit_b01("WFB_VENC_AUTOWIRE", ce_bool01(js, toks, ntok, "venc", "auto_wire", 1));

	/* Vehicle wiring (AIR-only; S99wfb consumes these). The ground supervisor
	 * ignores any section it does not know, so these are safe in the shared
	 * /etc/wfb-link.json. failsafe is fractional seconds -> ce_prim. */
	ce_prim(js, toks, ntok, "failsafe", "uplink_s", "2.0", s, sizeof(s)); emit_str("WFB_FAILSAFE_S", s);
	emit_int("SAFE_BITRATE",       ce_int(js, toks, ntok, "venc", "safe_startup_kbps", 4096));
	ce_str(js, toks, ntok, "venc", "host", "127.0.0.1:80", s, sizeof(s)); emit_str("WFB_VENC_HOST", s);
	emit_int("WFB_PROBE_PORT",     ce_int(js, toks, ntok, "links", "probe_port", 50));
	emit_int("WFB_CTRL",           ce_int(js, toks, ntok, "ports", "video_ctrl", 8000));
	emit_int("WFB_RX_FWD_PORT",    ce_int(js, toks, ntok, "ports", "uplink_fwd", 5801));
	emit_int("WFB_UPLINK_RX_PORT", ce_int(js, toks, ntok, "ports", "uplink_rx_stats", 5811));
	emit_int("WFB_PROBE_CTRL",     ce_int(js, toks, ntok, "ports", "probe_ctrl", 8001));
	emit_int("WFB_PROBE_FEED",     ce_int(js, toks, ntok, "ports", "probe_feed", 5750));

	free(buf);
	return 0;
}
