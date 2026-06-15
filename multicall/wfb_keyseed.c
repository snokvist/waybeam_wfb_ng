/* wfb_keyseed.c — deterministic bring-up key derivation. See wfb_keyseed.h. */
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

int wfb_ensure_key(const char *path, int role, const char *seed)
{
	if (!path || !*path)
		return -1;
	if (access(path, F_OK) == 0)
		return 0;  /* present — never overwrite */
	if (!seed || !*seed)
		seed = WFB_KEY_DEFAULT_SEED;

	unsigned char drone_sk[crypto_box_SECRETKEYBYTES], drone_pk[crypto_box_PUBLICKEYBYTES];
	unsigned char gs_sk[crypto_box_SECRETKEYBYTES], gs_pk[crypto_box_PUBLICKEYBYTES];
	if (derive_pair(seed, drone_sk, drone_pk, gs_sk, gs_pk) != 0)
		return -1;

	/* drone.key = drone_sk ‖ gs_pk ; gs.key = gs_sk ‖ drone_pk */
	unsigned char keyfile[crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES];
	if (role == WFB_ROLE_GS) {
		memcpy(keyfile, gs_sk, crypto_box_SECRETKEYBYTES);
		memcpy(keyfile + crypto_box_SECRETKEYBYTES, drone_pk, crypto_box_PUBLICKEYBYTES);
	} else {
		memcpy(keyfile, drone_sk, crypto_box_SECRETKEYBYTES);
		memcpy(keyfile + crypto_box_SECRETKEYBYTES, gs_pk, crypto_box_PUBLICKEYBYTES);
	}

	int rc = write_atomic(path, keyfile, sizeof(keyfile));
	sodium_memzero(drone_sk, sizeof(drone_sk));
	sodium_memzero(gs_sk, sizeof(gs_sk));
	sodium_memzero(keyfile, sizeof(keyfile));
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
	if (force)
		unlink(path);  /* regenerate deterministically (testing / WebUI re-seed) */
	if (wfb_ensure_key(path, role, seed) != 0)
		return 1;
	if (access(path, F_OK) == 0 && !force)
		;  /* already present or just created — silent success */
	return 0;
}
