/* wfb_multicall.cpp — generic multi-call dispatcher (owns main()).
 *
 * Compiled as C++ so it can call into the C++ wfb-ng applet entries
 * (wfb_rx_main / wfb_tx_main, renamed from main via -Dmain= at the mega
 * compile line) by their C++-mangled names, while the C daemon and tool
 * entries are reached through the extern "C" declarations in the per-side
 * wfb_applets[] table.
 *
 * Dispatch order:
 *   1. basename(argv[0])  — busybox-style symlink / exec name (e.g. wfb_rx).
 *   2. argv[1]            — explicit subcommand (e.g. `wfb-gs supervisor ...`).
 *   3. otherwise          — print the applet list and exit non-zero.
 */
#include "wfb_multicall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

static const struct wfb_applet *find_applet(const char *tok)
{
	if (!tok || !*tok)
		return NULL;
	for (const struct wfb_applet *a = wfb_applets; a->name; ++a) {
		if (strcmp(tok, a->name) == 0)
			return a;
		if (a->alias && strcmp(tok, a->alias) == 0)
			return a;
	}
	return NULL;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s <applet> [args...]\n   or: <applet> [args...]"
	                "  (via symlink / exec name)\n\napplets:\n", prog);
	for (const struct wfb_applet *a = wfb_applets; a->name; ++a)
		fprintf(stderr, "  %-12s %s\n", a->name, a->help ? a->help : "");
}

int main(int argc, char **argv)
{
	const char *prog = (argc > 0 && argv[0]) ? argv[0] : "wfb";

	/* 1) dispatch by basename(argv[0]). basename() may mutate its argument,
	 *    so operate on a copy. */
	char *argv0_copy = strdup(prog);
	if (argv0_copy) {
		const struct wfb_applet *a = find_applet(basename(argv0_copy));
		free(argv0_copy);
		if (a)
			return a->fn(argc, argv);
	}

	/* 2) dispatch by leading subcommand; the applet sees itself as argv[0]. */
	if (argc >= 2) {
		const struct wfb_applet *a = find_applet(argv[1]);
		if (a)
			return a->fn(argc - 1, argv + 1);
	}

	usage(prog);
	return 2;
}
