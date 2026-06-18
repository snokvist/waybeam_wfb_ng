/* test_fec_loss_adapt.c — host unit test for Adaptive-n loss-driven
 * redundancy (controller_update / fec_compute in link_controller.c).
 *
 * Drives controller_update() directly with synthetic frame sizes + loss
 * EWMAs + clock so each branch of the loss-bias law is exercised
 * deterministically (no sockets, no real clock).  Built by `make test` in
 * vehicle/ with -DLC_TEST_NO_MAIN so the daemon's main() is excluded and the
 * whole unit is #included here (same pattern as test_selector_law.c).
 *
 * Covers:
 *   regression — loss_adapt=false ignores loss entirely (byte-identical n vs.
 *                the static-curve path; the lock that lets us ship it off).
 *   attack     — fresh loss raises n above the curve immediately.
 *   ceiling    — effective redundancy is hard-capped at loss_adapt_ceiling.
 *   stale      — loss_fresh=false adds no parity (dead feedback path).
 *   decay      — parity holds right after loss clears, then bleeds back to the
 *                curve over loss_decay_s (slow release, not an instant drop).
 *   airtime    — the n·fps rail caps loss parity but never undercuts sizing.
 *
 * See docs/design/adaptive-n-rs-peek.md.
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

#define FRAME_SIZE  16000u   /* with mtu=1446, headroom 1.05 → k=12, curve n=18 */
#define DT_US       (100 * 1000ULL)   /* 100 ms between fed frames */

/* Feed `frames` constant-size frames spaced DT_US apart, advancing *now.
 * Controller state persists across calls; the last committed k/n live in
 * c->current. */
static void feed(Controller *c, const Config *cfg, HeadroomRing *ring,
                 uint64_t *now, int frames, uint32_t fsize,
                 float loss, float recov, bool fresh, float fps)
{
	FecParams out;
	for (int i = 0; i < frames; i++) {
		controller_update(c, cfg, fsize, ring, *now,
		                   loss, recov, fresh, fps, &out);
		*now += DT_US;
	}
}

/* Baseline config: Adaptive-n armed, recov term isolated off, airtime rail
 * disabled (individual tests re-enable what they assert on). */
static Config base_cfg(void)
{
	Config cfg;
	config_defaults(&cfg);
	cfg.fec.loss_adapt            = true;
	cfg.fec.loss_adapt_gain       = 3.0f;
	cfg.fec.loss_adapt_recov_gain = 0.0f;
	cfg.fec.loss_adapt_ceiling    = 0.60f;
	cfg.fec.loss_decay_s          = 2.0f;
	cfg.fec.loss_stale_s          = 2.0f;
	cfg.fec.airtime_max_pps       = 0;       /* off for the law tests */
	return cfg;
}

int main(void)
{
	Config cfg = base_cfg();

	/* ── regression lock: loss_adapt=false ignores loss ── */
	{
		printf("[regression — loss_adapt=false]\n");
		Config off = cfg;
		off.fec.loss_adapt = false;
		Controller a = {0}, b = {0};
		HeadroomRing ra = {0}, rb = {0};
		uint64_t ta = BASE, tb = BASE;
		feed(&a, &off, &ra, &ta, 70, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		feed(&b, &off, &rb, &tb, 70, FRAME_SIZE, 0.30f, 0.10f, true, 30.0f);
		CHECK(a.current.k == b.current.k && a.current.n == b.current.n,
		      "heavy loss does not change k/n when disabled");
		CHECK(a.current.k == 12 && a.current.n == 18,
		      "curve baseline is k=12 n=18");
		CHECK(b.current.r_loss_bias == 0.0f,
		      "no loss bias recorded when disabled");
	}

	/* ── attack: fresh loss raises n above the curve ── */
	int n0;
	{
		printf("[attack — fresh loss raises parity]\n");
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		feed(&c, &cfg, &r, &t, 60, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		n0 = c.current.n;
		CHECK(n0 == 18, "clean-link n sits on the curve (n=18)");
		feed(&c, &cfg, &r, &t, 3, FRAME_SIZE, 0.05f, 0.00f, true, 30.0f);
		CHECK(c.current.n > n0, "5% residual loss raises n above the curve");
		CHECK(c.current.k == 12, "k is unchanged by loss adaptation");
		CHECK(c.current.r_loss_bias > 0.0f, "loss bias is recorded for telemetry");

		/* ── decay: hold then slow release back to the curve ── */
		printf("[decay — hold then slow release]\n");
		feed(&c, &cfg, &r, &t, 1, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		CHECK(c.current.n > n0,
		      "parity held immediately after loss clears (no instant drop)");
		t += 30 * SEC_US;   /* well past loss_decay_s + the down-cooldown */
		feed(&c, &cfg, &r, &t, 1, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		CHECK(c.current.n == n0,
		      "parity decays back to the curve after a sustained clean link");
	}

	/* ── ceiling: effective redundancy is hard-capped ── */
	{
		printf("[ceiling — redundancy capped at loss_adapt_ceiling]\n");
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		feed(&c, &cfg, &r, &t, 60, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		feed(&c, &cfg, &r, &t, 3, FRAME_SIZE, 0.50f, 0.00f, true, 30.0f);
		float red = 1.0f - (float)c.current.k / (float)c.current.n;
		CHECK(red <= cfg.fec.loss_adapt_ceiling + 0.02f,
		      "huge loss does not exceed the redundancy ceiling");
		CHECK(red > 0.50f, "huge loss does drive redundancy up to the ceiling");
	}

	/* ── staleness: no rx_ant → no parity ── */
	{
		printf("[stale — dead feedback path adds no parity]\n");
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		feed(&c, &cfg, &r, &t, 60, FRAME_SIZE, 0.00f, 0.00f, true, 30.0f);
		int nb = c.current.n;
		feed(&c, &cfg, &r, &t, 5, FRAME_SIZE, 0.20f, 0.00f, false, 30.0f);
		CHECK(c.current.n == nb, "stale loss feedback is treated as zero");
	}

	/* ── airtime rail: caps loss parity, never undercuts sizing ── */
	{
		printf("[airtime — n*fps rail caps loss parity]\n");
		Config ar = cfg;
		ar.fec.airtime_max_pps = 1200;       /* with fps=60 → n_cap = 20 */
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		feed(&c, &ar, &r, &t, 60, FRAME_SIZE, 0.00f, 0.00f, true, 60.0f);
		int nb = c.current.n;     /* curve n=18; rail (cap 20) must not cut it */
		CHECK(nb == 18, "rail does not undercut the curve baseline");
		feed(&c, &ar, &r, &t, 3, FRAME_SIZE, 0.05f, 0.00f, true, 60.0f);
		CHECK(c.current.n > nb && c.current.n <= 20,
		      "loss parity is capped at floor(airtime_max_pps / fps)");
	}

	printf(g_fail ? "\nFAILED (%d failures)\n" : "\nPASSED (0 failures)\n", g_fail);
	return g_fail ? 1 : 0;
}
