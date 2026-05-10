/* udp_ts_recv: bind UDP port, print "<sec>.<ms> <bytes> <payload>\n" per packet.
 * Used to measure CSA inject-to-arrival jitter without disturbing the rx path.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <port>\n", argv[0]); return 2; }
    int port = atoi(argv[1]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char buf[4096];
    for (;;) {
        ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n < 0) { perror("recv"); return 1; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        buf[n] = 0;
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        printf("%lld.%03ld %zd %s\n",
               (long long)ts.tv_sec, ts.tv_nsec / 1000000L, n, buf);
    }
    return 0;
}
