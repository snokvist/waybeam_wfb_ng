/* csa.h — Channel Switch Announcement receiver, reusable library.
 *
 * Public API for the CSA state machine, allowlist/bandwidth parsers, and
 * `iw` runner. Used by the standalone csa_agent and (PR 2) by link_controller.
 *
 * Design:
 *   - All state lives in CsaState; all configuration in CsaCfg. No file-
 *     static state, so multiple agents can coexist in one process if needed.
 *   - csa_feed() / csa_tick() take an explicit now_ms so callers can drive
 *     a simulated clock from tests.
 *   - csa_run_iw() is synchronous fork+waitpid (~50–300 ms). Acceptable for
 *     callers that treat the channel switch as a planned dead-zone.
 */
#ifndef CSA_H
#define CSA_H

#include <stdbool.h>
#include <stddef.h>

#define CSA_MAX_ALLOW   32
#define CSA_HT_LEN      8
#define CSA_IFACE_LEN   16

typedef struct {
    char iface[CSA_IFACE_LEN];
    int  allow_chan[CSA_MAX_ALLOW];
    int  allow_chan_n;          /* 0 = permissive */
    char allow_bw[CSA_MAX_ALLOW][CSA_HT_LEN];
    int  allow_bw_n;            /* 0 = permissive */
    long long cooldown_ms;      /* 0 disables */
    bool no_revert;
} CsaCfg;

typedef enum { CSA_ST_IDLE, CSA_ST_ARMED, CSA_ST_VERIFY } CsaPhase;

typedef struct {
    CsaPhase  st;
    long long sess;
    long long t_switch_ms;
    long long t_revert_ms;
    int       target_chan;
    int       prev_chan;
    char      target_ht[CSA_HT_LEN];
    char      prev_ht[CSA_HT_LEN];
    long long last_switch_ms;   /* -1 = no prior switch */
} CsaState;

/* Logger callback. NULL = default stderr-with-timestamp logger. */
typedef void (*CsaLogFn)(const char *line);
void csa_set_logger(CsaLogFn fn);

void csa_init(CsaState *s);

/* Parse "149,153,157,161" / "HT20,HT40+,HT40-" into out arrays.
 * Returns count (>=0) on success, -1 on parse error. */
int  csa_parse_chan_list(const char *s, int *out, int max_n);
int  csa_parse_bw_list(const char *s, char out[][CSA_HT_LEN], int max_n);

/* Feed one received UDP datagram (NUL-terminated, len excludes NUL).
 * Returns true iff the line was a CSA frame and was consumed; false means
 * the caller should handle as non-CSA traffic (and probably call
 * csa_link_alive afterwards to count it as a heartbeat). */
bool csa_feed(CsaState *s, const CsaCfg *cfg,
              const char *buf, size_t len, long long now_ms);

/* Notify the state machine that any non-CSA datagram arrived. If state is
 * VERIFY, transitions to IDLE (link confirmed). Returns true iff a state
 * transition happened. */
bool csa_link_alive(CsaState *s, long long now_ms);

/* Periodic tick. Runs scheduled iw set-channel and reverts. Returns the
 * next monotonic deadline in ms, or 0 if no timer is pending (IDLE). */
long long csa_tick(CsaState *s, const CsaCfg *cfg, long long now_ms);

/* Synchronous fork+exec of `iw dev <iface> set channel <chan> <ht>`. */
int  csa_run_iw(const char *iface, int chan, const char *ht);

#endif
