/* air_applets.cpp — wfb-air (vehicle) applet table for the multi-call binary.
 * See docs/design/mega-binary.md. */
#include "wfb_multicall.h"
#include "wfb_applets_common.h"   /* wfb-ng entry decls + rx_applet/tx_applet thunks */

/* C daemon entry — C linkage (link_controller.c built as C with -DWFB_MULTICALL). */
extern "C" int link_controller_main(int, char **);

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
