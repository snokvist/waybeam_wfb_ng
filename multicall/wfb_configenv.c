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
	/* Numbers only — reject true/false/null so a bool can't become a bogus
	 * numeric arg (e.g. --failsafe true). Non-numeric primitive -> keep def. */
	char c0 = js[t[v].start];
	if (c0 != '-' && c0 != '+' && c0 != '.' && (c0 < '0' || c0 > '9')) return;
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
			/* Never abort: in mega mode S99wfb does `eval "$(config-env …)"`
			 * with no fallback, so emitting nothing would leave every WFB_*
			 * unset. Warn and skip — we still print the full default set. */
			fprintf(stderr, "config-env: ignoring unknown option %s\n", argv[i]);
			continue;
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
		if (!buf) {
			/* Don't abort (see the unknown-option note) — fall through to
			 * the default emit with js == NULL. */
			fclose(fp);
			fprintf(stderr, "config-env: out of memory — using preset defaults\n");
		} else {
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
	}

	const char *key_def = (role == WFB_ROLE_GS) ? "/etc/gs.key" : "/etc/drone.key";
	char s[256];

	printf("# wfb-link preset (source: %s)\n", src);
	emit_int("WFB_CHANNEL", ce_int(js, toks, ntok, "radio", "channel", 161));
	ce_str(js, toks, ntok, "radio", "htmode", "HT20", s, sizeof(s)); emit_str("WFB_HTMODE", s);
	emit_int("WFB_BW",      ce_int(js, toks, ntok, "radio", "bw", 20));
	emit_int("WFB_TXPOWER", ce_int(js, toks, ntok, "radio", "txpower_mbm", 2000));
	/* Monitor-iface MTU. Default 4052 = WLAN_DATA_MAXLEN, the rtl88x2eu
	 * driver's hard max (rtl88x2eu/include/wifi.h). Larger RTP payloads from
	 * adaptive payload sizing plus wfb_tx/radiotap framing must not exceed the
	 * link MTU or injection wedges; the adaptive sizer caps at the 3200 tier
	 * so 4052 is ample. S99wfb sets it after monitor mode; clamps to
	 * [1500,4052] (this old kernel does not enforce max_mtu itself). */
	emit_int("WFB_MTU",     ce_int(js, toks, ntok, "radio", "mtu", 4052));
	ce_str(js, toks, ntok, "radio", "iface", "wlan0", s, sizeof(s)); emit_str("WFB_IFACE", s);
	ce_str(js, toks, ntok, "key", "file", key_def, s, sizeof(s)); emit_str("KEY", s);
	ce_str(js, toks, ntok, "key", "seed", "Waybeam", s, sizeof(s)); emit_str("WFB_KEY_SEED", s);
	/* AIR-only: video downlink session crypto. 1 -> wfb_tx -xx (open/keyless,
	 * WFB_PACKET_SESSION_PLAIN); 0 -> -x (encrypted session, keypair required on
	 * the RX). Default 1 matches the shipped open-video preset. */
	emit_b01("WFB_VIDEO_OPEN", ce_bool01(js, toks, ntok, "key", "open", 1));
	emit_int("WFB_TX_LINK",    ce_int(js, toks, ntok, "links", "video", 207));
	emit_int("WFB_RX_LINK",    ce_int(js, toks, ntok, "links", "uplink", 208));
	emit_int("WFB_PROBE_LINK", ce_int(js, toks, ntok, "links", "probe", 50));
	emit_int("WFB_K", ce_int(js, toks, ntok, "fec", "k", 8));
	emit_int("WFB_N", ce_int(js, toks, ntok, "fec", "n", 12));
	/* Adaptive RTP payload sizing (link_controller). payload_max=0 -> off
	 * (controller never writes outgoing.maxPayloadSize); >0 enables the
	 * bitrate->payload tier sizer. Also live-tunable via /set. */
	emit_int("WFB_PAYLOAD_MAX", ce_int(js, toks, ntok, "fec", "payload_max", 0));
	emit_int("WFB_PAYLOAD_MIN", ce_int(js, toks, ntok, "fec", "payload_min", 576));
	/* Airtime guard (link_controller). airtime_max_pct caps on-air airtime
	 * to this % of channel (0=off; per-MCS pps/airslot ceiling), default 80;
	 * airtime_preamble_us is the per-packet PHY preamble used in the calc.
	 * Both also live-tunable via /set fec.airtime_max_pct / *_preamble_us. */
	emit_int("WFB_AIRTIME_MAX_PCT",     ce_int(js, toks, ntok, "fec", "airtime_max_pct", 80));
	emit_int("WFB_AIRTIME_PREAMBLE_US", ce_int(js, toks, ntok, "fec", "airtime_preamble_us", 40));
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

	/* Recovery backdoor (AIR-only). Separate keyless/open -xx link so a lost or
	 * mismatched drone.key still leaves a way to arm APFPV-next-boot. enabled
	 * defaults ON; the hook command is fork+exec'd by link_controller. */
	emit_b01("WFB_RECOVERY",      ce_bool01(js, toks, ntok, "recovery", "enabled", 1));
	emit_int("WFB_RECOVERY_LINK", ce_int(js, toks, ntok, "links", "recovery", 209));
	emit_int("WFB_RECOVERY_PORT", ce_int(js, toks, ntok, "ports", "recovery", 5802));
	ce_str(js, toks, ntok, "recovery", "apfpv_cmd", "/etc/wfb/recovery-apfpv.sh", s, sizeof(s));
	emit_str("WFB_RECOVERY_CMD", s);

	free(buf);
	return 0;
}
