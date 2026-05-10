/* csa_agent — standalone CSA receiver.
 *
 * Binds UDP <port> on the host (replaces link_controller's 5801 binding
 * during the bench test). Parses csa_commit JSON frames, schedules a single
 * `iw set channel` at the target monotonic deadline, and reverts if no UDP
 * traffic is observed within t_revert_ms after the switch.
 *
 * Build:  arm-linux-gnueabihf-gcc -static -Os -o csa_agent.armhf csa.c csa_agent.c
 *
 * All state-machine logic lives in csa.c so link_controller can reuse it.
 */
#define _POSIX_C_SOURCE 200809L
#include "csa.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--no-revert] [--allowlist CH,...] [--bandwidth BW,...] "
        "[--cooldown-ms N] <port> <iface>\n"
        "  --allowlist   comma-separated channels accepted as targets\n"
        "                (e.g. 149,153,157,161); empty = any channel\n"
        "  --bandwidth   comma-separated bandwidths accepted as targets\n"
        "                (e.g. HT20,HT40+); empty = any bandwidth\n"
        "  --cooldown-ms minimum gap between channel changes\n"
        "                (default 2000, 0 disables)\n",
        argv0);
}

int main(int argc, char **argv) {
    CsaCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cooldown_ms = 2000;
    cfg.no_revert = false;

    const char *allowlist_arg = NULL;
    const char *bandwidth_arg = NULL;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        const char *flag = argv[argi];
        if (strcmp(flag, "--no-revert") == 0) {
            cfg.no_revert = true; argi++;
        } else if (strcmp(flag, "--allowlist") == 0 && argi + 1 < argc) {
            allowlist_arg = argv[argi + 1]; argi += 2;
        } else if (strcmp(flag, "--bandwidth") == 0 && argi + 1 < argc) {
            bandwidth_arg = argv[argi + 1]; argi += 2;
        } else if (strcmp(flag, "--cooldown-ms") == 0 && argi + 1 < argc) {
            cfg.cooldown_ms = strtoll(argv[argi + 1], NULL, 10); argi += 2;
        } else {
            fprintf(stderr, "unknown flag: %s\n", flag);
            usage(argv[0]);
            return 2;
        }
    }
    if (allowlist_arg) {
        int n = csa_parse_chan_list(allowlist_arg, cfg.allow_chan, CSA_MAX_ALLOW);
        if (n < 0) { fprintf(stderr, "bad --allowlist: %s\n", allowlist_arg); return 2; }
        cfg.allow_chan_n = n;
    }
    if (bandwidth_arg) {
        int n = csa_parse_bw_list(bandwidth_arg, cfg.allow_bw, CSA_MAX_ALLOW);
        if (n < 0) { fprintf(stderr, "bad --bandwidth: %s\n", bandwidth_arg); return 2; }
        cfg.allow_bw_n = n;
    }
    if (argc - argi < 2) { usage(argv[0]); return 2; }
    int port = atoi(argv[argi++]);
    snprintf(cfg.iface, sizeof(cfg.iface), "%s", argv[argi++]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }

    CsaState st;
    csa_init(&st);
    fprintf(stderr,
        "csa_agent listening port=%d iface=%s no_revert=%d "
        "cooldown_ms=%lld allowlist=%d chan(s) bandwidth=%d entries\n",
        port, cfg.iface, cfg.no_revert ? 1 : 0, cfg.cooldown_ms,
        cfg.allow_chan_n, cfg.allow_bw_n);

    char buf[4096];
    struct timeval tv;
    fd_set rfds;
    while (1) {
        FD_ZERO(&rfds); FD_SET(s, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 5000;  /* 5 ms tick */
        int rv = select(s + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0 && errno == EINTR) continue;
        if (rv > 0 && FD_ISSET(s, &rfds)) {
            ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                if (!csa_feed(&st, &cfg, buf, (size_t)n, now_ms())) {
                    csa_link_alive(&st, now_ms());
                }
            }
        }
        csa_tick(&st, &cfg, now_ms());
    }
    return 0;
}
