/* air_applets.cpp — wfb-air (vehicle) applet table for the multi-call binary.
 * See docs/design/mega-binary.md. */
#include "wfb_multicall.h"

/* C daemon entry — C linkage (link_controller.c built as C with -DWFB_MULTICALL). */
extern "C" int link_controller_main(int, char **);

#ifdef WFB_WITH_WFBNG
/* wfb-ng C++ tools — C++ linkage (renamed from main via -Dmain= at compile). */
int wfb_rx_main(int, char **);
int wfb_tx_main(int, char **);
/* wfb-ng C tools — C linkage. */
extern "C" int wfb_tx_cmd_main(int, char **);
extern "C" int wfb_keygen_main(int, char **);
#endif

const struct wfb_applet wfb_applets[] = {
	{ "link", "link_controller", link_controller_main,
	  "adaptive FEC+MCS, WCMD proxy, CSA, WebUI" },
#ifdef WFB_WITH_WFBNG
	{ "tx",     "wfb_tx",     wfb_tx_main,     "wfb-ng transmitter (video + probe)" },
	{ "rx",     "wfb_rx",     wfb_rx_main,     "wfb-ng receiver (uplink)" },
	{ "tx_cmd", "wfb_tx_cmd", wfb_tx_cmd_main, "wfb_tx runtime control client" },
	{ "keygen", "wfb_keygen", wfb_keygen_main, "generate wfb-ng keypair" },
#endif
	{ 0, 0, 0, 0 },
};
