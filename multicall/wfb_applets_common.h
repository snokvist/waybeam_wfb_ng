/* wfb_applets_common.h — shared wfb-ng applet entry declarations + thunks for
 * the per-side mega applet tables (ground/gs_applets.cpp, vehicle/air_applets.cpp).
 *
 * Only the per-side daemon row and the applet help strings differ between the
 * two tables; the wfb-ng tool wiring below is identical, so it lives here to
 * avoid drift. Included by both tables; effective only in WFB_WITH_WFBNG builds
 * (Phase 1 daemon-only links leave it inert).
 */
#ifndef WFB_APPLETS_COMMON_H
#define WFB_APPLETS_COMMON_H

#ifdef WFB_WITH_WFBNG
/* wfb-ng C++ tools — renamed from main via -Dmain= at the mega compile line.
 * Upstream declares `int main(int argc, char *const argv[])`, so the renamed
 * symbol mangles with the (int, char* const*) signature; declare it identically
 * and forward through a thunk so it fits the table's (int, char**) fn type
 * (char** -> char* const* converts implicitly and legally at the call). */
int wfb_rx_main(int, char *const *);
int wfb_tx_main(int, char *const *);
static inline int rx_applet(int argc, char **argv) { return wfb_rx_main(argc, argv); }
static inline int tx_applet(int argc, char **argv) { return wfb_tx_main(argc, argv); }
/* wfb-ng C tools — C linkage (no mangling, char** matches). */
extern "C" int wfb_tx_cmd_main(int, char **);
extern "C" int wfb_keygen_main(int, char **);
#endif /* WFB_WITH_WFBNG */

#endif /* WFB_APPLETS_COMMON_H */
