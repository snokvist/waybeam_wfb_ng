/* air_applets.cpp — wfb-air (vehicle) applet table for the multi-call binary.
 * See docs/design/mega-binary.md. */
#include "wfb_multicall.h"

/* C daemon entry — C linkage (link_controller.c built as C with -DWFB_MULTICALL). */
extern "C" int link_controller_main(int, char **);

#ifdef WFB_WITH_WFBNG
/* wfb-ng C++ tools — C++ linkage (renamed from main via -Dmain= at compile).
 * Upstream declares main as `int main(int, char* const*)`, so the renamed
 * symbol mangles with that signature; declare it identically and forward
 * through a thunk so it fits the table's (int, char**) fn type (char** →
 * char* const* converts implicitly and legally at the call). */
int wfb_rx_main(int, char *const *);
int wfb_tx_main(int, char *const *);
static int rx_applet(int argc, char **argv) { return wfb_rx_main(argc, argv); }
static int tx_applet(int argc, char **argv) { return wfb_tx_main(argc, argv); }
/* wfb-ng C tools — C linkage (no mangling, char** matches). */
extern "C" int wfb_tx_cmd_main(int, char **);
extern "C" int wfb_keygen_main(int, char **);
#endif

const struct wfb_applet wfb_applets[] = {
	{ "link", "link_controller", link_controller_main,
	  "adaptive FEC+MCS, WCMD proxy, CSA, WebUI" },
#ifdef WFB_WITH_WFBNG
	{ "tx",     "wfb_tx",     tx_applet,       "wfb-ng transmitter (video + probe)" },
	{ "rx",     "wfb_rx",     rx_applet,       "wfb-ng receiver (uplink)" },
	{ "tx_cmd", "wfb_tx_cmd", wfb_tx_cmd_main, "wfb_tx runtime control client" },
	{ "keygen", "wfb_keygen", wfb_keygen_main, "generate wfb-ng keypair" },
#endif
	{ 0, 0, 0, 0 },
};
