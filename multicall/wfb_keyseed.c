/* wfb_keyseed.c — deterministic bring-up key derivation + key management.
 * See wfb_keyseed.h. */
#include "wfb_keyseed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <sodium.h>

#define WFB_KEY_DEFAULT_SEED "Waybeam"
#define WFB_KEY_BYTES (crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES) /* 64 */

/* Derive the matched drone/gs pair from `seed`, bit-identical to
 * `wfb_keygen <seed>` (Argon2i, fixed salt, then crypto_box_seed_keypair). */
static int derive_pair(const char *seed,
                       unsigned char drone_sk[crypto_box_SECRETKEYBYTES],
                       unsigned char drone_pk[crypto_box_PUBLICKEYBYTES],
                       unsigned char gs_sk[crypto_box_SECRETKEYBYTES],
                       unsigned char gs_pk[crypto_box_PUBLICKEYBYTES])
{
	if (sodium_init() < 0) {
		fprintf(stderr, "[keyseed] libsodium init failed\n");
		return -1;
	}
	unsigned char salt[crypto_pwhash_argon2i_SALTBYTES] =
		{'w','i','f','i','b','r','o','a','d','c','a','s','t','k','e','y'};
	unsigned char kseed[crypto_box_SEEDBYTES * 2];
	if (crypto_pwhash_argon2i(kseed, sizeof(kseed), seed, strlen(seed), salt,
	                          crypto_pwhash_argon2i_OPSLIMIT_INTERACTIVE,
	                          crypto_pwhash_argon2i_MEMLIMIT_INTERACTIVE,
	                          crypto_pwhash_ALG_ARGON2I13) != 0) {
		fprintf(stderr, "[keyseed] unable to derive seed from passphrase\n");
		return -1;
	}
	if (crypto_box_seed_keypair(drone_pk, drone_sk, kseed) != 0 ||
	    crypto_box_seed_keypair(gs_pk, gs_sk, kseed + crypto_box_SEEDBYTES) != 0) {
		fprintf(stderr, "[keyseed] unable to derive keypair\n");
		return -1;
	}
	return 0;
}

/* Atomically write `len` bytes to `path` (temp file in the same dir + rename,
 * mode 0600). Returns 0 on success, -1 on error. */
static int write_atomic(const char *path, const unsigned char *buf, size_t len)
{
	char *dup = strdup(path);
	if (!dup)
		return -1;
	char tmpl[1024];
	int n = snprintf(tmpl, sizeof(tmpl), "%s/.wfbkey.XXXXXX", dirname(dup));
	free(dup);
	if (n < 0 || (size_t)n >= sizeof(tmpl))
		return -1;

	int fd = mkstemp(tmpl);
	if (fd < 0) {
		fprintf(stderr, "[keyseed] mkstemp(%s): %s\n", tmpl, strerror(errno));
		return -1;
	}
	int ok = 1;
	if (fchmod(fd, 0600) != 0)
		ok = 0;
	size_t off = 0;
	while (ok && off < len) {
		ssize_t w = write(fd, buf + off, len - off);
		if (w <= 0) { ok = 0; break; }
		off += (size_t)w;
	}
	if (close(fd) != 0)
		ok = 0;
	if (!ok || rename(tmpl, path) != 0) {
		fprintf(stderr, "[keyseed] failed to write %s: %s\n", path, strerror(errno));
		unlink(tmpl);
		return -1;
	}
	return 0;
}

/* Assemble + write the role-correct 64-byte key file from a derived pair.
 *   drone.key = drone_sk ‖ gs_pk ; gs.key = gs_sk ‖ drone_pk */
static int write_role_file(const char *path, int role,
                           const unsigned char *drone_sk, const unsigned char *drone_pk,
                           const unsigned char *gs_sk,    const unsigned char *gs_pk)
{
	unsigned char keyfile[WFB_KEY_BYTES];
	if (role == WFB_ROLE_GS) {
		memcpy(keyfile, gs_sk, crypto_box_SECRETKEYBYTES);
		memcpy(keyfile + crypto_box_SECRETKEYBYTES, drone_pk, crypto_box_PUBLICKEYBYTES);
	} else {
		memcpy(keyfile, drone_sk, crypto_box_SECRETKEYBYTES);
		memcpy(keyfile + crypto_box_SECRETKEYBYTES, gs_pk, crypto_box_PUBLICKEYBYTES);
	}
	int rc = write_atomic(path, keyfile, sizeof(keyfile));
	sodium_memzero(keyfile, sizeof(keyfile));
	return rc;
}

int wfb_ensure_key(const char *path, int role, const char *seed)
{
	if (!path || !*path)
		return -1;
	if (access(path, F_OK) == 0)
		return 0;  /* present — never overwrite */
	if (!seed || !*seed)
		seed = WFB_KEY_DEFAULT_SEED;

	unsigned char dsk[crypto_box_SECRETKEYBYTES], dpk[crypto_box_PUBLICKEYBYTES];
	unsigned char gsk[crypto_box_SECRETKEYBYTES], gpk[crypto_box_PUBLICKEYBYTES];
	if (derive_pair(seed, dsk, dpk, gsk, gpk) != 0)
		return -1;
	int rc = write_role_file(path, role, dsk, dpk, gsk, gpk);
	sodium_memzero(dsk, sizeof(dsk));
	sodium_memzero(gsk, sizeof(gsk));
	if (rc != 0)
		return -1;

	fprintf(stderr,
		"[keyseed] *** INSECURE DEFAULT *** %s was missing; generated a shared\n"
		"[keyseed]   %s key from the built-in seed \"%s\". Every unit built from\n"
		"[keyseed]   this firmware shares this key — install a unique key for production\n"
		"[keyseed]   (key-management WebUI, or `keygen` + scp).\n",
		path, role == WFB_ROLE_GS ? "ground" : "drone", seed);
	return 0;
}

int wfb_write_key_seed(const char *path, int role, const char *seed)
{
	if (!path || !*path)
		return -1;
	if (!seed || !*seed)
		seed = WFB_KEY_DEFAULT_SEED;
	unsigned char dsk[crypto_box_SECRETKEYBYTES], dpk[crypto_box_PUBLICKEYBYTES];
	unsigned char gsk[crypto_box_SECRETKEYBYTES], gpk[crypto_box_PUBLICKEYBYTES];
	if (derive_pair(seed, dsk, dpk, gsk, gpk) != 0)
		return -1;
	int rc = write_role_file(path, role, dsk, dpk, gsk, gpk);
	sodium_memzero(dsk, sizeof(dsk));
	sodium_memzero(gsk, sizeof(gsk));
	return rc;
}

int wfb_write_key_random(const char *path, int role)
{
	if (!path || !*path)
		return -1;
	if (sodium_init() < 0)
		return -1;
	unsigned char dsk[crypto_box_SECRETKEYBYTES], dpk[crypto_box_PUBLICKEYBYTES];
	unsigned char gsk[crypto_box_SECRETKEYBYTES], gpk[crypto_box_PUBLICKEYBYTES];
	if (crypto_box_keypair(dpk, dsk) != 0 || crypto_box_keypair(gpk, gsk) != 0)
		return -1;
	int rc = write_role_file(path, role, dsk, dpk, gsk, gpk);
	sodium_memzero(dsk, sizeof(dsk));
	sodium_memzero(gsk, sizeof(gsk));
	return rc;
}

int wfb_write_key_raw(const char *path, const unsigned char buf[64])
{
	if (!path || !*path || !buf)
		return -1;
	return write_atomic(path, buf, WFB_KEY_BYTES);
}

int wfb_hex_to_key(const char *hex, unsigned char out[64])
{
	if (!hex || !out)
		return -1;
	/* tolerate surrounding whitespace; require exactly 128 hex nibbles */
	while (*hex == ' ' || *hex == '\t' || *hex == '\n' || *hex == '\r') hex++;
	size_t got = 0;
	for (; hex[0] && hex[1] && got < WFB_KEY_BYTES; got++) {
		char b[3] = { hex[0], hex[1], 0 };
		char *end = NULL;
		long v = strtol(b, &end, 16);
		if (end != b + 2 || v < 0 || v > 255)
			return -1;
		out[got] = (unsigned char)v;
		hex += 2;
	}
	while (*hex == ' ' || *hex == '\t' || *hex == '\n' || *hex == '\r') hex++;
	if (got != WFB_KEY_BYTES || *hex != 0)
		return -1;
	return 0;
}

int wfb_key_fingerprint(const char *path, int role, char *out, size_t cap)
{
	if (!out || cap < 9)
		return -1;
	out[0] = 0;
	FILE *fp = fopen(path, "r");
	if (!fp) { snprintf(out, cap, "none"); return 0; }
	unsigned char kf[WFB_KEY_BYTES];
	size_t r = fread(kf, 1, sizeof(kf), fp);
	fclose(fp);
	if (r != sizeof(kf)) { snprintf(out, cap, "invalid"); return -1; }
	if (sodium_init() < 0)
		return -1;

	/* kf = own_sk(32) ‖ peer_pk(32). Recover own_pk; assemble the canonical
	 * pair (drone_pk ‖ gs_pk) so both ends of a matched pair hash the same. */
	const unsigned char *own_sk  = kf;
	const unsigned char *peer_pk = kf + crypto_box_SECRETKEYBYTES;
	unsigned char own_pk[crypto_box_PUBLICKEYBYTES];
	if (crypto_scalarmult_base(own_pk, own_sk) != 0) {
		sodium_memzero(kf, sizeof(kf));
		return -1;
	}
	unsigned char pair[crypto_box_PUBLICKEYBYTES * 2];
	if (role == WFB_ROLE_GS) {            /* gs.key: own=gs, peer=drone */
		memcpy(pair, peer_pk, crypto_box_PUBLICKEYBYTES);                          /* drone_pk */
		memcpy(pair + crypto_box_PUBLICKEYBYTES, own_pk, crypto_box_PUBLICKEYBYTES); /* gs_pk */
	} else {                              /* drone.key: own=drone, peer=gs */
		memcpy(pair, own_pk, crypto_box_PUBLICKEYBYTES);                           /* drone_pk */
		memcpy(pair + crypto_box_PUBLICKEYBYTES, peer_pk, crypto_box_PUBLICKEYBYTES); /* gs_pk */
	}
	unsigned char h[16];
	crypto_generichash(h, sizeof(h), pair, sizeof(pair), NULL, 0);
	snprintf(out, cap, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
	sodium_memzero(kf, sizeof(kf));
	return 0;
}

int wfb_keygen_ensure_main(int argc, char **argv, int default_role)
{
	const char *seed = WFB_KEY_DEFAULT_SEED;
	const char *path = NULL;
	int role = default_role;
	int force = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = argv[++i];
		} else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			const char *r = argv[++i];
			if (!strcmp(r, "drone")) role = WFB_ROLE_DRONE;
			else if (!strcmp(r, "gs")) role = WFB_ROLE_GS;
			else { fprintf(stderr, "keygen-ensure: --role must be drone|gs\n"); return 2; }
		} else if (!strcmp(argv[i], "--force")) {
			force = 1;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "keygen-ensure: unknown option %s\n", argv[i]);
			return 2;
		} else if (!path) {
			path = argv[i];
		} else {
			fprintf(stderr, "keygen-ensure: extra argument %s\n", argv[i]);
			return 2;
		}
	}
	if (!path) {
		fprintf(stderr, "usage: keygen-ensure [--seed SEED] [--role drone|gs] [--force] <keyfile>\n");
		return 2;
	}
	if (force) {
		if (wfb_write_key_seed(path, role, seed) != 0)
			return 1;
		return 0;
	}
	return wfb_ensure_key(path, role, seed) != 0 ? 1 : 0;
}
