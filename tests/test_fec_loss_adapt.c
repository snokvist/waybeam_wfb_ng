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
		/* committed_kbps=0 -> feed-forward disabled (measurement path),
		 * matching this suite's assertions. */
		controller_update(c, cfg, fsize, ring, *now,
		                   loss, recov, fresh, fps, 0, &out);
		*now += DT_US;
	}
}

/* Like feed(), but with an explicit committed venc bitrate so the feed-forward
 * sizing path (cfg.fec.blocksize_feedforward) is exercised.  Loss feedback is
 * held off here so these cases isolate k behaviour. */
static void feed_ff(Controller *c, const Config *cfg, HeadroomRing *ring,
                    uint64_t *now, int frames, uint32_t fsize,
                    long kbps, float fps)
{
	FecParams out;
	for (int i = 0; i < frames; i++) {
		controller_update(c, cfg, fsize, ring, *now,
		                   0.0f, 0.0f, false, fps, kbps, &out);
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

	/* ── feed-forward sizing: the blocksize<->MCS desync fix ──
	 * These lock the prototype `fec.blocksize_feedforward` behaviour:
	 *   - OFF is byte-identical to the measurement path (regression).
	 *   - ON sizes k from the committed bitrate, not the frame EWMA.
	 *   - ON commits a k-down promptly *during* settle grace (the fix);
	 *     OFF freezes k high through grace (the bug we are fixing).
	 *   - ON still honors startup grace. */
	{
		printf("[feedforward — off is byte-identical to measurement]\n");
		Config ff = cfg; ff.fec.loss_adapt = false;
		Config on = ff;  on.fec.blocksize_feedforward = true;
		Config off = ff; off.fec.blocksize_feedforward = false;
		/* Same big frames; OFF must ignore the (huge) committed bitrate and
		 * size purely from frame size. */
		Controller a = {0}, b = {0};
		HeadroomRing ra = {0}, rb = {0};
		uint64_t ta = BASE, tb = BASE;
		feed_ff(&a, &off, &ra, &ta, 60, FRAME_SIZE, 999999, 30.0f);
		/* drive the measurement path on b with committed_kbps=0 via feed() */
		feed(&b, &off, &rb, &tb, 60, FRAME_SIZE, 0.0f, 0.0f, false, 30.0f);
		CHECK(a.current.k == b.current.k && a.current.n == b.current.n,
		      "ff OFF: committed bitrate is ignored (== measurement path)");
		CHECK(a.current.k == 12, "ff OFF: k stays on the frame-size curve (k=12)");

		printf("[feedforward — k sized from committed bitrate]\n");
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		/* Tiny frames (1 pkt) but a high committed bitrate: ff must size k
		 * up from the bitrate, NOT down from the small measured frame. */
		feed_ff(&c, &on, &r, &t, 60, 1200u, 16000, 120.0f);
		/* 16000 kbps / 120 fps = ~16.7 KB/frame -> ~12 pkts -> k ~ 12. */
		CHECK(c.current.k >= 8,
		      "ff ON: k sized from committed bitrate, not the 1-pkt frame");
		int k_hi = c.current.k;

		printf("[feedforward — k-down commits during settle grace (the fix)]\n");
		/* Arm a 5 s settle freeze (as an MCS-down would), then deliver a
		 * still-big frame but a collapsed committed bitrate. ff must commit
		 * k DOWN immediately despite the grace freeze and the lagging frame. */
		controller_arm_settle(&c, t, 5.0f);
		FecParams out;
		bool emit = controller_update(&c, &on, FRAME_SIZE, &r, t,
		                              0.0f, 0.0f, false, 120.0f, 2000, &out);
		CHECK(emit, "ff ON: k-step commits during settle grace (not frozen)");
		CHECK(c.current.k < k_hi,
		      "ff ON: k drops to the low operating point in grace");

		printf("[feedforward — baseline freezes k in grace (the bug)]\n");
		/* Same setup, ff OFF: grace must freeze k at the high value even
		 * though the operating point (and eventually frames) have collapsed. */
		Controller d = {0};
		HeadroomRing rd = {0};
		uint64_t td = BASE;
		feed_ff(&d, &on, &rd, &td, 60, 1200u, 16000, 120.0f);  /* same high k */
		int kd_hi = d.current.k;
		controller_arm_settle(&d, td, 5.0f);
		bool emit2 = controller_update(&d, &off, 1200u, &rd, td,
		                               0.0f, 0.0f, false, 120.0f, 2000, &out);
		CHECK(!emit2, "ff OFF: no commit during settle grace (frozen)");
		CHECK(d.current.k == kd_hi, "ff OFF: k stays pinned high in grace");

		printf("[feedforward — startup grace is honored]\n");
		Controller e = {0};
		HeadroomRing re = {0};
		FecParams oute;
		/* First frame is inside startup_grace_s (default 2 s) → no commit. */
		bool emit3 = controller_update(&e, &on, FRAME_SIZE, &re, BASE,
		                               0.0f, 0.0f, false, 120.0f, 16000, &oute);
		CHECK(!emit3, "ff ON: no commit during startup grace");
		CHECK(!e.have_current, "ff ON: have_current stays false under startup grace");

		printf("[feedforward — cooldown rate-limits rapid k-steps]\n");
		Config onc = on; onc.fec.ff_cooldown_s = 0.20f;
		Controller f = {0};
		HeadroomRing rf = {0};
		uint64_t tf = BASE;
		feed_ff(&f, &onc, &rf, &tf, 60, 1200u, 1500, 120.0f);   /* low op point, k small */
		int kf0 = f.current.k;
		FecParams o;
		bool e1 = controller_update(&f, &onc, 1200u, &rf, tf,
		                            0.0f, 0.0f, false, 120.0f, 20000, &o);
		CHECK(e1 && f.current.k > kf0, "ff cooldown: first k-step commits immediately");
		int kf1 = f.current.k;
		/* 2nd op-point change 50 ms later (< 200 ms cooldown) -> suppressed */
		bool e2 = controller_update(&f, &onc, 1200u, &rf, tf + 50*1000ULL,
		                            0.0f, 0.0f, false, 120.0f, 1500, &o);
		CHECK(!e2 && f.current.k == kf1, "ff cooldown: rapid 2nd k-step suppressed");
		/* once the cooldown elapses, the step lands */
		bool e3 = controller_update(&f, &onc, 1200u, &rf, tf + 300*1000ULL,
		                            0.0f, 0.0f, false, 120.0f, 1500, &o);
		CHECK(e3 && f.current.k < kf1, "ff cooldown: k-step resumes after cooldown");
	}

	/* ── min_k floor: low-MCS sub-MTU frames don't collapse to k=2 ──
	 * A single-packet frame would size to ppf=1 (k clamped up by min_k).
	 * With the production floor min_k=4 the block is k=4, and the curve
	 * gives n=7 (r(4)=0.40). This is what keeps MCS0 off the 2/4↔2/5
	 * budget-cliff limit cycle: at k=4 a +/-1 n step is a ~12% budget
	 * swing that stays inside bitrate_tolerance instead of forcing a
	 * bitrate rewrite every tick. */
	{
		printf("[min_k floor — sub-MTU frame holds k>=4]\n");
		Config dflt;
		config_defaults(&dflt);
		CHECK(dflt.fec.min_k == 4, "default min_k is 4");
		CHECK(dflt.fec.min_n > dflt.fec.min_k, "default min_n > min_k (parity floor)");
		dflt.fec.loss_adapt = false;   /* isolate the static curve+floor */
		Controller c = {0};
		HeadroomRing r = {0};
		uint64_t t = BASE;
		feed(&c, &dflt, &r, &t, 40, 1200u, 0.00f, 0.00f, true, 120.0f);
		CHECK(c.current.k == 4, "tiny frame floors k to 4 (not 1/2)");
		CHECK(c.current.n == 7, "curve gives n=7 at k=4 (4/7, not 2/5)");
	}

	printf(g_fail ? "\nFAILED (%d failures)\n" : "\nPASSED (0 failures)\n", g_fail);
	return g_fail ? 1 : 0;
}
