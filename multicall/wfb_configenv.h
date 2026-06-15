/* wfb_configenv.h — `config-env` applet: render /etc/wfb-link.json as shell env.
 *
 * Reads the optional unified link-preset file and prints `NAME=value` lines for
 * the startup script to `eval`. Every field has a baked-in default, so a
 * missing/empty/invalid file yields exactly the shipped preset (link comes up
 * unchanged) — defaults live here in C, the scripts stay thin. Always exits 0
 * unless given bad arguments; parse/read problems warn on stderr and fall back
 * to defaults. `role` (WFB_ROLE_DRONE/WFB_ROLE_GS from wfb_keyseed.h) selects
 * the side-specific key-file default.
 */
#ifndef WFB_CONFIGENV_H
#define WFB_CONFIGENV_H

#ifdef __cplusplus
extern "C" {
#endif

int wfb_configenv_main(int argc, char **argv, int role);

#ifdef __cplusplus
}
#endif

#endif /* WFB_CONFIGENV_H */
