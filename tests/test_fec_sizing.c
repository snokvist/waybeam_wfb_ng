/* test_fec_sizing.c — host unit test for the minimal frame-size-driven FEC
 * sizing in controller_update() / fec_compute() (link_controller.c).
 *
 * The vehicle FEC controller is deliberately simple: k tracks the frame
 * size, n = curve(k), and nothing is loss-reactive — the MCS selector is
 * the sole loss-response loop.  These tests lock that contract:
 *
 *   stable     — on a constant frame size the controller commits ONCE and
 *                then stays silent (no n-flip, no re-emit).  This is the
 *                property the old Adaptive-n path violated.
 *   curve      — n is a pure function of k (the static REDUNDANCY_CURVE).
 *   min_k      — a sub-MTU frame floors k to fec.min_k (no collapse to k=1/2).
 *   resize     — a committed-bitrate step (an MCS transition) re-sizes k
 *                promptly via feed-forward, then goes silent again.
 *
 * Built by `make test` in vehicle/ with -DLC_TEST_NO_MAIN so the daemon's
 * main() is excluded and the whole unit is #included here (same pattern as
 * test_selector_law.c).
 */
#ifndef LC_TEST_NO_MAIN
#define LC_TEST_NO_MAIN
#endif
#include "../vehicle/link_controller.c"

#include <stdio.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
	else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

#define BASE    ((uint64_t)1000 * 1000 * 1000)   /* 1000 s in us */
#define SEC_US  ((uint64_t)1000 * 1000)
#define DT_US   (100 * 1000ULL)                  /* 100 ms between frames */

/* Feed `frames` constant-size frames spaced DT_US apart, advancing *now.
 * committed_kbps=0 selects the measurement (frame-EWMA) sizing path.
 * Returns the number of commits (emits) observed. */
static int feed(Controller *c, const Config *cfg, HeadroomRing *ring,
                uint64_t *now, int frames, uint32_t fsize, float fps)
{
	FecParams out;
	int emits = 0;
	for (int i = 0; i < frames; i++) {
		if (controller_update(c, cfg, fsize, ring, *now, fps, 0, &out))
			emits++;
		*now += DT_US;
	}
	return emits;
}

/* Like feed(), but with an explicit committed venc bitrate so the
 * feed-forward sizing path (cfg.fec.blocksize_feedforward) is exercised —
 * this is the MCS-transition path (k sized from the commanded operating
 * point, not the lagging frame EWMA). */
static int feed_ff(Controller *c, const Config *cfg, HeadroomRing *ring,
                   uint64_t *now, int frames, uint32_t fsize,
                   long kbps, float fps)
{
	FecParams out;
	int emits = 0;
	for (int i = 0; i < frames; i++) {
		if (controller_update(c, cfg, fsize, ring, *now, fps, kbps, &out))
			emits++;
		*now += DT_US;
	}
	return emits;
}

int main(void)
{
	Config cfg;
	config_defaults(&cfg);

	printf("test_fec_sizing — minimal frame-size-driven FEC\n");

	/* ── n = curve(k) is a pure function of k ── */
	{
		printf("[curve — n is a pure function of k]\n");
		/* fec_compute(avg, headroom=1.0, cfg, cur_ppf=0): ppf = ceil(avg/mtu). */
		int mtu = cfg.fec.mtu;   /* 1446 */
		FecParams a = fec_compute((float)(4 * mtu),  1.0f, &cfg, 0);
		FecParams b = fec_compute((float)(8 * mtu),  1.0f, &cfg, 0);
		FecParams d = fec_compute((float)(12 * mtu), 1.0f, &cfg, 0);
		FecParams e = fec_compute((float)(16 * mtu), 1.0f, &cfg, 0);
		CHECK(a.k == 4  && a.n == 6,  "k=4  -> n=6  (curve r=0.33, 4/6 MCS0)");
		CHECK(b.k == 8  && b.n == 12, "k=8  -> n=12 (curve r=0.33)");
		CHECK(d.k == 12 && d.n == 18, "k=12 -> n=18 (curve interp r=0.315)");
		CHECK(e.k == 16 && e.n == 23, "k=16 -> n=23 (curve r=0.30)");
	}

	/* ── min_k floor: a sub-MTU frame never collapses to k=1/2 ── */
	{
		printf("[min_k — sub-MTU frame floors k to fec.min_k]\n");
		CHECK(cfg.fec.min_k == 4, "default min_k is 4");
		CHECK(cfg.fec.min_n > cfg.fec.min_k, "default min_n > min_k (parity floor)");
		FecParams p = fec_compute(1200.0f, 1.05f, &cfg, 0);   /* ppf=1 */
		CHECK(p.k == 4 && p.n == 6, "tiny frame -> k=4 n=6 (floored, not 1/2)");
	}

	/* ── stable link: commit once, then silence (no n-flip) ── */
	{
		printf("[stable — constant frame size emits once then goes quiet]\n");
		Controller c = {0};
		HeadroomRing ring = {0};
		uint64_t t = BASE;
		/* 16 kB frames @120fps, measurement path. headroom collapses to
		 * headroom_min (constant size) -> k=12, n=18. */
		int warm = feed(&c, &cfg, &ring, &t, 40, 16000u, 120.0f);
		CHECK(warm == 1, "exactly one commit during warmup (after startup grace)");
		CHECK(c.current.k == 12 && c.current.n == 18, "settled at k=12 n=18");
		uint32_t upd_before = c.update_count;
		/* Run a long stable stretch (~30 s): the controller must not move. */
		int churn = feed(&c, &cfg, &ring, &t, 300, 16000u, 120.0f);
		CHECK(churn == 0, "ZERO re-emits across 30 s of stable link");
		CHECK(c.update_count == upd_before, "update_count frozen on stable link");
		CHECK(c.current.k == 12 && c.current.n == 18, "k/n unchanged");
	}

	/* ── live min_n edit re-emits even at a constant frame size ──
	 * n=curve(k) is clamped to fec.min_n/max_n, both live-mutable.  A
	 * steady link is silent, but an operator raising min_n must take
	 * effect — this is deliberate config, not noise, so it can't churn. */
	{
		printf("[min_n — live bound edit re-emits at constant frame size]\n");
		Config c2 = cfg;
		Controller c = {0};
		HeadroomRing ring = {0};
		uint64_t t = BASE;
		feed(&c, &c2, &ring, &t, 40, 16000u, 120.0f);   /* settle k=12 n=18 */
		CHECK(c.current.k == 12 && c.current.n == 18, "settled k=12 n=18");
		uint32_t upd = c.update_count;
		c2.fec.min_n = 24;                              /* raise the floor live */
		int emits = feed(&c, &c2, &ring, &t, 5, 16000u, 120.0f);
		CHECK(emits == 1, "exactly one re-emit on the min_n bump");
		CHECK(c.current.k == 12 && c.current.n == 24, "n lifted to new min_n, k held");
		CHECK(c.update_count == upd + 1, "single commit, then quiet again");
	}

	/* ── MCS transition: a committed-bitrate step re-sizes k, then quiets ── */
	{
		printf("[resize — committed-bitrate step re-sizes k via feed-forward]\n");
		Controller c = {0};
		HeadroomRing ring = {0};
		uint64_t t = BASE;
		/* MCS0 operating point: ~1625 kbps @120fps -> ~1693 B/frame -> ppf=2,
		 * floored to k=4 (n=6, 4/6). */
		feed_ff(&c, &cfg, &ring, &t, 40, 1700u, 1625, 120.0f);
		CHECK(c.current.k == 4 && c.current.n == 6, "MCS0: k floored to 4, n=6");
		int k_low = c.current.k;

		/* Jump to an MCS5-class operating point: ~17333 kbps @120fps ->
		 * ~18055 B/frame -> k climbs well above the floor. */
		int up = feed_ff(&c, &cfg, &ring, &t, 40, 18000u, 17333, 120.0f);
		CHECK(up >= 1, "upward bitrate step produced at least one resize");
		CHECK(c.current.k > k_low, "k grew to track the larger frame");
		CHECK(c.current.n == fec_compute((float)c.current.k * cfg.fec.mtu, 1.0f,
		                                 &cfg, 0).n,
		      "n still equals curve(k) after resize");

		/* Let it fully settle: after the step, the measured-ring headroom is
		 * still draining old small frames, so k trims down through the normal
		 * hysteresis path (k_down_dwell). Run well past that window. */
		feed_ff(&c, &cfg, &ring, &t, 150, 18000u, 17333, 120.0f);
		/* Now the steady tail must be silent. */
		uint32_t upd_hi = c.update_count;
		int churn_hi = feed_ff(&c, &cfg, &ring, &t, 200, 18000u, 17333, 120.0f);
		CHECK(churn_hi == 0, "ZERO re-emits once the high rung has settled");
		CHECK(c.update_count == upd_hi, "update_count frozen at the settled high rung");
	}

	/* ── constant bitrate + fps noise must NOT churn k ──
	 * Feed-forward sizes k from committed_kbps/fps, so fps jitter at a held
	 * MCS rung makes cand.k wiggle ±1 across a packet boundary.  That is not
	 * an operating-point change, so the prompt feed-forward path must stay
	 * gated and the controller must stay silent (the bug: "FEC updates keep
	 * coming in, needlessly small"). */
	{
		printf("[stable bitrate — fps jitter must not churn k]\n");
		Config c2 = cfg;
		Controller c = {0};
		HeadroomRing ring = {0};
		uint64_t t = BASE;
		FecParams out;
		/* Settle at a held rung. kbps=12555 sits right on a packet boundary:
		 * fps=120 -> ppf 10, fps=108 -> ppf 11 (constant frame_size feeds a
		 * flat headroom so fps is the only variable). */
		const long KBPS = 12555;
		for (int i = 0; i < 40; i++) {
			controller_update(&c, &c2, 12000u, &ring, t, 114.0f, KBPS, &out);
			t += DT_US;
		}
		int k0 = c.current.k;
		uint32_t upd = c.update_count;
		/* Now jitter fps across the boundary at a CONSTANT bitrate. */
		int emits = 0;
		for (int i = 0; i < 200; i++) {
			float f = (i & 1) ? 108.0f : 120.0f;
			if (controller_update(&c, &c2, 12000u, &ring, t, f, KBPS, &out))
				emits++;
			t += DT_US;
		}
		CHECK(emits == 0, "ZERO re-emits from fps jitter at a constant bitrate");
		CHECK(c.update_count == upd, "update_count frozen across fps jitter");
		CHECK(c.current.k == k0, "k held despite fps wiggle");

		/* But a genuine bitrate step (operating-point move) still resizes
		 * promptly via feed-forward. */
		int step = 0;
		for (int i = 0; i < 5; i++) {
			if (controller_update(&c, &c2, 4000u, &ring, t, 114.0f, 4000, &out))
				step++;
			t += DT_US;
		}
		CHECK(step >= 1, "a real bitrate step still triggers a prompt resize");
		CHECK(c.current.k < k0, "k dropped to track the lower operating point");
	}

	printf(g_fail ? "\nFAILED (%d failures)\n" : "\nPASSED (0 failures)\n", g_fail);
	return g_fail ? 1 : 0;
}
