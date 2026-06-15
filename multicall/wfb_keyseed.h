/* wfb_keyseed.h — deterministic bring-up key derivation for the mega binary.
 *
 * wfb-ng's rx/tx only ever *read* a key file (-K, default rx.key/tx.key) and
 * abort if it is missing. For an autonomous first boot we instead derive a
 * deterministic pair from a fixed passphrase (default "Waybeam"), exactly as
 * `wfb_keygen <seed>` does (Argon2i, salt "wifibroadcastkey", then
 * crypto_box_seed_keypair), and write the role-correct 64-byte key file when
 * one is absent.
 *
 * This is an INSECURE shared default — every unit built from the same firmware
 * shares the key. It exists only so an unconfigured pair links up out of the
 * box; production deployments must install a unique key (see the key-management
 * WebUI). The ensure path NEVER overwrites an existing file.
 *
 * Key file layout (matches wfb_keygen):
 *   drone.key = drone_secretkey(32) ‖ gs_publickey(32)     (role DRONE — air)
 *   gs.key    = gs_secretkey(32)    ‖ drone_publickey(32)  (role GS — ground)
 */
#ifndef WFB_KEYSEED_H
#define WFB_KEYSEED_H

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#define WFB_ROLE_DRONE 0   /* air: writes drone_sk ‖ gs_pk  */
#define WFB_ROLE_GS    1   /* ground: writes gs_sk ‖ drone_pk */

/* Ensure a key file exists at `path`.
 *   - file already present  -> no-op, returns 0 (never overwrites).
 *   - file absent           -> derive the deterministic pair from `seed`
 *                              (NULL/empty -> "Waybeam"), write the role-correct
 *                              64-byte file (mode 0600, atomic via temp+rename),
 *                              emit a loud INSECURE-DEFAULT warning on stderr.
 * Returns 0 on success/no-op, -1 on error. `role` is WFB_ROLE_DRONE/WFB_ROLE_GS.
 */
int wfb_ensure_key(const char *path, int role, const char *seed);

/* `keygen-ensure` applet body. argv is the applet's own (argv[0] == applet
 * name). Usage: keygen-ensure [--seed SEED] [--role drone|gs] [--force] <file>
 * `default_role` is used when --role is omitted (each side passes its own).
 * Returns 0 on success (incl. existing file without --force), non-zero on error.
 */
int wfb_keygen_ensure_main(int argc, char **argv, int default_role);

/* --- key management (used by the WebUI HTTP handlers) -------------------- */

/* 8-hex pair fingerprint: BLAKE2b of the canonical drone_pk‖gs_pk, recovered
 * from this side's key file. Identical on a matched air/ground pair, so it
 * confirms both ends share a key without revealing it. `out` needs >=9 bytes.
 * Writes "none" if the file is absent (returns 0); -1 on read/derive error. */
int wfb_key_fingerprint(const char *path, int role, char *out, size_t cap);

/* (Re)write the key file — these OVERWRITE (unlike wfb_ensure_key). role is
 * WFB_ROLE_DRONE/WFB_ROLE_GS. Return 0 on success, -1 on error. */
int wfb_write_key_seed(const char *path, int role, const char *seed); /* deterministic from passphrase */
int wfb_write_key_random(const char *path, int role);                 /* fresh random pair */
int wfb_write_key_raw(const char *path, const unsigned char buf[64]); /* verbatim 64 bytes (upload) */

/* Parse exactly 128 hex nibbles (optional surrounding whitespace) -> 64 bytes.
 * Returns 0 on success, -1 on malformed input. */
int wfb_hex_to_key(const char *hex, unsigned char out[64]);

#ifdef __cplusplus
}
#endif

#endif /* WFB_KEYSEED_H */
