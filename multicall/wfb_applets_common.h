/* wfb_applets_common.h — shared wfb-ng applet entry declarations + thunks for
 * the per-side mega applet tables (ground/gs_applets.cpp, vehicle/air_applets.cpp).
 *
 * Only the per-side daemon row and the applet help strings differ between the
 * two tables; the wfb-ng tool wiring below is identical, so it lives here to
 * avoid drift. Included by both tables; effective only in WFB_WITH_WFBNG builds
 * (Phase 1 daemon-only links leave it inert).
 *
 * Each side defines WFB_SIDE_ROLE (WFB_ROLE_DRONE on air, WFB_ROLE_GS on
 * ground) BEFORE including this header — it is unambiguous because the wfb-air
 * binary is always the drone and wfb-gs is always the ground station. It drives
 * the auto-seed of a missing key file.
 */
#ifndef WFB_APPLETS_COMMON_H
#define WFB_APPLETS_COMMON_H

#ifdef WFB_WITH_WFBNG
#include <string.h>
#include "wfb_keyseed.h"

#ifndef WFB_SIDE_ROLE
#define WFB_SIDE_ROLE WFB_ROLE_DRONE   /* safe default; both sides override */
#endif

/* wfb-ng C++ tools — renamed from main via -Dmain= at the mega compile line.
 * Upstream declares `int main(int argc, char *const argv[])`, so the renamed
 * symbol mangles with the (int, char* const*) signature; declare it identically
 * and forward through a thunk so it fits the table's (int, char**) fn type
 * (char** -> char* const* converts implicitly and legally at the call). */
int wfb_rx_main(int, char *const *);
int wfb_tx_main(int, char *const *);

/* Before running rx/tx, seed a missing -K key file from the built-in passphrase
 * so an unconfigured unit still links up (INSECURE shared default; see
 * wfb_keyseed.h). No-op when the file exists or no -K is given. The role is the
 * binary's side, not the filename — wfb-air is always the drone, wfb-gs the GS.
 * This is a backstop; the startup script's explicit `keygen-ensure` runs first
 * (single writer, before tx/rx/probe race to create the file). */
static inline void wfb_seed_key_if_missing(int argc, char **argv) {
	const char *kp = 0;
	for (int i = 1; i < argc; i++)
		if (argv[i] && !strcmp(argv[i], "-K") && i + 1 < argc) { kp = argv[i + 1]; break; }
	if (kp)
		wfb_ensure_key(kp, WFB_SIDE_ROLE, 0);
}
static inline int rx_applet(int argc, char **argv) { wfb_seed_key_if_missing(argc, argv); return wfb_rx_main(argc, argv); }
static inline int tx_applet(int argc, char **argv) { wfb_seed_key_if_missing(argc, argv); return wfb_tx_main(argc, argv); }

/* keygen-ensure applet — defaults the role to this binary's side. */
static inline int keygen_ensure_applet(int argc, char **argv) { return wfb_keygen_ensure_main(argc, argv, WFB_SIDE_ROLE); }

/* wfb-ng C tools — C linkage (no mangling, char** matches). */
extern "C" int wfb_tx_cmd_main(int, char **);
extern "C" int wfb_keygen_main(int, char **);
#endif /* WFB_WITH_WFBNG */

#endif /* WFB_APPLETS_COMMON_H */
