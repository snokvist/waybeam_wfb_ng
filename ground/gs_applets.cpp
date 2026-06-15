/* gs_applets.cpp — wfb-gs (ground) applet table for the multi-call binary.
 * See docs/design/mega-binary.md. */
#include "wfb_multicall.h"
#include "wfb_applets_common.h"   /* wfb-ng entry decls + rx_applet/tx_applet thunks */

/* C daemon entry — C linkage (gs_supervisor.c built as C with -DWFB_MULTICALL). */
extern "C" int gs_supervisor_main(int, char **);

const struct wfb_applet wfb_applets[] = {
	{ "supervisor", "gs_supervisor", gs_supervisor_main,
	  "fork/manage wfb_rx+wfb_tx, REST API, WebUI" },
#ifdef WFB_WITH_WFBNG
	{ "rx",     "wfb_rx",     rx_applet,       "wfb-ng receiver (pcap capture, FEC decode)" },
	{ "tx",     "wfb_tx",     tx_applet,       "wfb-ng transmitter (802.11 inject, FEC encode)" },
	{ "tx_cmd", "wfb_tx_cmd", wfb_tx_cmd_main, "wfb_tx runtime control client" },
	{ "keygen", "wfb_keygen", wfb_keygen_main, "generate wfb-ng keypair" },
#endif
	{ 0, 0, 0, 0 },
};
