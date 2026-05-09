/* csa.c — Channel Switch Announcement receiver implementation.
 *
 * See csa.h for the public API. The JSON parser is intentionally minimal:
 * scans for "key":<value> pairs with single-token values (string/int).
 * Robust enough for the line-delimited line-per-packet format we use, not
 * a general parser.
 */
#define _POSIX_C_SOURCE 200809L
#include "csa.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── logging ─────────────────────────────────────────────────────────── */

static void default_logger(const char *line) {
    char tbuf[64];
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] %s\n", tbuf, ts.tv_nsec / 1000000L, line);
}

static CsaLogFn g_log_fn = default_logger;

void csa_set_logger(CsaLogFn fn) {
    g_log_fn = fn ? fn : default_logger;
}

static void log_msg(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log_fn(buf);
}

/* ── minimal JSON field extractor ────────────────────────────────────── */

static const char *find_key(const char *buf, const char *key) {
    size_t klen = strlen(key);
    const char *p = buf;
    while ((p = strstr(p, key))) {
        if (p > buf && p[-1] == '"' && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ') q++;
            if (*q == ':') return q + 1;
        }
        p += klen;
    }
    return NULL;
}

static bool get_str(const char *buf, const char *key, char *out, size_t outlen) {
    const char *p = find_key(buf, key);
    if (!p) return false;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
    out[i] = 0;
    return true;
}

static bool get_int(const char *buf, const char *key, long long *out) {
    const char *p = find_key(buf, key);
    if (!p) return false;
    while (*p == ' ') p++;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

/* ── allowlist parsers ──────────────────────────────────────────────── */

int csa_parse_chan_list(const char *s, int *out, int max_n) {
    int n = 0;
    const char *p = s;
    while (*p && n < max_n) {
        while (*p == ' ') p++;
        char *end;
        long c = strtol(p, &end, 10);
        if (end == p) return -1;
        if (c < 1 || c > 200) return -1;
        out[n++] = (int)c;
        p = end;
        while (*p == ' ') p++;
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return n;
}

int csa_parse_bw_list(const char *s, char out[][CSA_HT_LEN], int max_n) {
    int n = 0;
    const char *p = s;
    while (*p && n < max_n) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        size_t len = (size_t)(end - start);
        if (len == 0 || len >= CSA_HT_LEN) return -1;
        memcpy(out[n], start, len);
        out[n][len] = 0;
        n++;
        if (*p == ',') p++;
    }
    return n;
}

static bool chan_ok(const CsaCfg *cfg, int chan) {
    if (cfg->allow_chan_n == 0) return true;
    for (int i = 0; i < cfg->allow_chan_n; i++)
        if (cfg->allow_chan[i] == chan) return true;
    return false;
}

static bool bw_ok(const CsaCfg *cfg, const char *bw) {
    if (cfg->allow_bw_n == 0) return true;
    for (int i = 0; i < cfg->allow_bw_n; i++)
        if (strcmp(cfg->allow_bw[i], bw) == 0) return true;
    return false;
}

/* ── iw runner ──────────────────────────────────────────────────────── */

int csa_run_iw(const char *iface, int chan, const char *ht) {
    char chan_s[16]; snprintf(chan_s, sizeof(chan_s), "%d", chan);
    log_msg("iw dev %s set channel %s %s", iface, chan_s, ht);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/usr/sbin/iw", "iw", "dev", iface, "set", "channel",
              chan_s, ht, (char*)NULL);
        execlp("iw", "iw", "dev", iface, "set", "channel",
               chan_s, ht, (char*)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ── state machine ──────────────────────────────────────────────────── */

void csa_init(CsaState *s) {
    memset(s, 0, sizeof(*s));
    s->st = CSA_ST_IDLE;
    s->last_switch_ms = -1;
}

static void to_idle(CsaState *s) {
    s->st = CSA_ST_IDLE;
    log_msg("state=IDLE");
}

/* Returns true iff the buf was a csa_commit and was processed (or rejected
 * with a known reason). Returns false if the buf is not a CSA frame. */
static bool on_commit(CsaState *s, const CsaCfg *cfg,
                      const char *buf, long long now) {
    long long sess, seq, dt, t_revert, target_chan, prev_chan;
    char target_ht[CSA_HT_LEN] = "HT20", prev_ht[CSA_HT_LEN] = "HT20";
    if (!get_int(buf, "sess", &sess)) return true;            /* malformed CSA */
    if (!get_int(buf, "seq", &seq)) seq = 0;
    if (!get_int(buf, "dt_to_switch_ms", &dt)) return true;
    if (!get_int(buf, "t_revert_ms", &t_revert)) t_revert = 3000;
    if (!get_int(buf, "target_chan", &target_chan)) return true;
    if (!get_int(buf, "prev_chan", &prev_chan)) prev_chan = 0;
    get_str(buf, "target_ht", target_ht, sizeof(target_ht));
    get_str(buf, "prev_ht", prev_ht, sizeof(prev_ht));

    long long t_switch = now + dt;

    /* Defense-in-depth target validation. Applied to every csa_commit so that
     * even refresh frames carrying a tampered target are caught. Range check
     * runs first so allowlist comparisons cannot be tricked by a value that
     * wraps under (int) truncation. */
    if (target_chan < 1 || target_chan > 200) {
        log_msg("REJECT sess=%lld seq=%lld: target_chan=%lld out of range",
                sess, seq, target_chan);
        return true;
    }
    if (!chan_ok(cfg, (int)target_chan)) {
        log_msg("REJECT sess=%lld seq=%lld: target ch%lld not in --allowlist",
                sess, seq, target_chan);
        return true;
    }
    if (!bw_ok(cfg, target_ht)) {
        log_msg("REJECT sess=%lld seq=%lld: target bandwidth %s not in --bandwidth",
                sess, seq, target_ht);
        return true;
    }

    /* Tail-of-burst csa_commit frames arriving after SWITCH on the new
     * channel confirm the link the same way other UDP traffic would. */
    if (s->st == CSA_ST_VERIFY && sess == s->sess) {
        log_msg("VERIFY heartbeat: csa_commit seq=%lld -> COMMITTED", seq);
        to_idle(s);
        return true;
    }

    if (s->st == CSA_ST_IDLE || sess > s->sess) {
        /* Cooldown only gates NEW sessions; same-session refreshes are always
         * allowed (they refine T_switch within ±20 ms of the original). */
        if (cfg->cooldown_ms > 0 && s->last_switch_ms >= 0) {
            long long since = now - s->last_switch_ms;
            if (since < cfg->cooldown_ms) {
                log_msg("REJECT sess=%lld seq=%lld: cooldown %lldms remaining",
                        sess, seq, cfg->cooldown_ms - since);
                return true;
            }
        }
        s->st = CSA_ST_ARMED;
        s->sess = sess;
        s->t_switch_ms = t_switch;
        s->t_revert_ms = t_revert;        /* duration; absolute deadline set on SWITCH */
        s->target_chan = (int)target_chan;
        s->prev_chan = (int)prev_chan;
        snprintf(s->target_ht, sizeof(s->target_ht), "%s", target_ht);
        snprintf(s->prev_ht, sizeof(s->prev_ht), "%s", prev_ht);
        log_msg("ARMED sess=%lld seq=%lld dt=%lldms target=ch%d %s prev=ch%d %s revert=%lldms",
                sess, seq, dt, s->target_chan, s->target_ht,
                s->prev_chan, s->prev_ht, t_revert);
        return true;
    }

    if (s->st == CSA_ST_ARMED && sess == s->sess) {
        long long delta = t_switch - s->t_switch_ms;
        long long abs_delta = delta < 0 ? -delta : delta;
        if (abs_delta <= 20) {
            log_msg("REFRESH seq=%lld dt=%lldms (drift=%lldms ok)",
                    seq, dt, delta);
        } else {
            log_msg("REFRESH seq=%lld dt=%lldms (drift=%lldms exceeds ±20ms, ignored)",
                    seq, dt, delta);
        }
    }
    return true;
}

bool csa_feed(CsaState *s, const CsaCfg *cfg,
              const char *buf, size_t len, long long now_ms) {
    (void)len;
    char type[32] = "";
    if (!get_str(buf, "type", type, sizeof(type))) return false;
    if (strncmp(type, "csa_", 4) != 0) return false;
    if (strncmp(type, "csa_commit", 10) == 0) {
        return on_commit(s, cfg, buf, now_ms);
    }
    /* Unknown csa_* subtype — consumed but no-op. */
    log_msg("ignore unknown csa frame type=%s", type);
    return true;
}

bool csa_link_alive(CsaState *s, long long now_ms) {
    (void)now_ms;
    if (s->st == CSA_ST_VERIFY) {
        log_msg("VERIFY heartbeat: non-CSA traffic -> COMMITTED");
        to_idle(s);
        return true;
    }
    return false;
}

long long csa_tick(CsaState *s, const CsaCfg *cfg, long long now_ms) {
    if (s->st == CSA_ST_ARMED) {
        if (now_ms >= s->t_switch_ms) {
            log_msg("SWITCH at +%lldms (target ch%d %s)",
                    now_ms - s->t_switch_ms, s->target_chan, s->target_ht);
            csa_run_iw(cfg->iface, s->target_chan, s->target_ht);
            /* Anchor cooldown at the scheduled switch instant, not after iw(8)
             * returns, so the gap is deterministic regardless of iw spawn time. */
            s->last_switch_ms = now_ms;
            if (cfg->no_revert) {
                log_msg("state=COMMITTED (no-revert mode)");
                to_idle(s);
                return 0;
            }
            s->st = CSA_ST_VERIFY;
            /* Revert deadline anchored on the injected now_ms (pre-iw).
             * Loses ~iw_runtime ms of "silence on the new channel" precision
             * (~100 ms out of 3000–5000 ms typical t_revert) — acceptable,
             * and keeps the state machine free of real-clock reads so tests
             * can drive it deterministically. */
            s->t_revert_ms = now_ms + s->t_revert_ms;
            log_msg("state=VERIFY revert_at=+%lldms", s->t_revert_ms - now_ms);
            return s->t_revert_ms;
        }
        return s->t_switch_ms;
    }
    if (s->st == CSA_ST_VERIFY) {
        if (now_ms >= s->t_revert_ms) {
            log_msg("REVERT triggered (no traffic seen on new channel)");
            csa_run_iw(cfg->iface, s->prev_chan, s->prev_ht);
            s->last_switch_ms = now_ms;  /* revert is itself a hop */
            to_idle(s);
            return 0;
        }
        return s->t_revert_ms;
    }
    return 0;
}
