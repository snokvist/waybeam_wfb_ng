/* gs_applets.cpp — wfb-gs (ground) applet table for the multi-call binary.
 * See docs/design/mega-binary.md. */
#include "wfb_multicall.h"

/* C daemon entry — C linkage (gs_supervisor.c built as C with -DWFB_MULTICALL). */
extern "C" int gs_supervisor_main(int, char **);

#ifdef WFB_WITH_WFBNG
/* wfb-ng C++ tools — C++ linkage (renamed from main via -Dmain= at compile). */
int wfb_rx_main(int, char **);
int wfb_tx_main(int, char **);
/* wfb-ng C tools — C linkage. */
extern "C" int wfb_tx_cmd_main(int, char **);
extern "C" int wfb_keygen_main(int, char **);
#endif

const struct wfb_applet wfb_applets[] = {
	{ "supervisor", "gs_supervisor", gs_supervisor_main,
	  "fork/manage wfb_rx+wfb_tx, REST API, WebUI" },
#ifdef WFB_WITH_WFBNG
	{ "rx",     "wfb_rx",     wfb_rx_main,     "wfb-ng receiver (pcap capture, FEC decode)" },
	{ "tx",     "wfb_tx",     wfb_tx_main,     "wfb-ng transmitter (802.11 inject, FEC encode)" },
	{ "tx_cmd", "wfb_tx_cmd", wfb_tx_cmd_main, "wfb_tx runtime control client" },
	{ "keygen", "wfb_keygen", wfb_keygen_main, "generate wfb-ng keypair" },
#endif
	{ 0, 0, 0, 0 },
};
