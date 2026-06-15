/* wfb_multicall.h — multi-call ("mega binary") dispatcher interface.
 *
 * See docs/design/mega-binary.md.  One executable per side (wfb-gs on the
 * ground, wfb-air on the vehicle) bundles several applets; the role is chosen
 * by argv[0] (busybox-style symlink/exec) or by a leading subcommand token.
 * Each applet still runs as its own process — the supervisor keeps fork/exec
 * but re-execs /proc/self/exe <applet> ... — so there is no shared global
 * state between applets and getopt/optind start fresh every time.
 *
 * Each side provides its own NULL-terminated wfb_applets[] table (see
 * ground/gs_applets.cpp, vehicle/air_applets.cpp).
 */
#ifndef WFB_MULTICALL_H
#define WFB_MULTICALL_H

#ifdef __cplusplus
extern "C" {
#endif

struct wfb_applet {
	const char *name;            /* canonical subcommand, e.g. "rx"        */
	const char *alias;           /* legacy basename match, e.g. "wfb_rx"   */
	int       (*fn)(int, char **); /* applet entry (the renamed main)      */
	const char *help;            /* one-line description for usage         */
};

/* Provided per side. Terminated by a {0,0,0,0} sentinel. */
extern const struct wfb_applet wfb_applets[];

#ifdef __cplusplus
}
#endif

#endif /* WFB_MULTICALL_H */
