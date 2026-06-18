/* bench_fec_loss_adapt.c — FPV-flight loss-scenario bench for Adaptive-n.
 *
 * Drives the REAL controller_update() / fec_compute() (link_controller.c,
 * #included with -DLC_TEST_NO_MAIN) through scripted FPV loss timelines and
 * reports adaptive-n (fec.loss_adapt=ON) vs. the static curve (OFF) on two
 * axes: frame delivery and airtime cost. Host-only — no RF, no sockets.
 *
 * Model (deterministic, seeded; identical channel + frame sizes fed to both
 * modes so the only variable is the controller):
 *   - Channel: Gilbert-Elliott two-state burst loss per packet. A per-frame
 *     "stress" timeline modulates burst entry rate + in-burst loss, scripting
 *     each scenario. Bursty (not i.i.d.) loss is what actually defeats block
 *     FEC, so this is the honest stressor.
 *   - Frames: GOP-60, large I-frames + smaller P-frames, fed to the controller
 *     so k tracks frame geometry exactly as in flight.
 *   - FEC outcome per frame: the committed block is k data + (n-k) parity over
 *     n packet slots; the frame is delivered iff lost <= n-k (RS decodable).
 *   - Feedback: the GS-observed residual-loss and FEC-recovered ratios are
 *     EWMA-smoothed (scorer alpha) and fed back DELAYED by the uplink RTT,
 *     exactly the closed loop the daemon runs — so loop lag / oscillation
 *     shows up here if it exists.
 *
 * Airtime rail (fec.airtime_max_pps) is left OFF here to isolate the loss
 * law; the rail itself is covered by tests/test_fec_loss_adapt.c.
 *
 * Build/run:  make -C vehicle bench
 * See docs/design/adaptive-n-rs-peek.md.
 */
#ifndef LC_TEST_NO_MAIN
#define LC_TEST_NO_MAIN
#endif
#include "../vehicle/link_controller.c"

#include <stdio.h>
#include <math.h>

/* ── deterministic PRNG (splitmix64) ─────────────────────────────────── */
static uint64_t g_rng;
static uint64_t sm64(void)
{
	uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}
static double urand(void) { return (double)(sm64() >> 11) * (1.0 / 9007199254740992.0); }

/* ── timeline ─────────────────────────────────────────────────────────── */
#define FPS        60
#define DUR_S      12
#define NFRAMES    (FPS * DUR_S)
#define MAXP       80                 /* >= max_n (72); per-frame packet slots */
#define FB_DELAY   6                  /* ~100 ms uplink RTT at 60 fps          */
#define DT_US      (1000000ULL / FPS)
#define BASE_US    ((uint64_t)1000 * 1000 * 1000)

enum { SC_CLEAN, SC_OBSTACLE, SC_RANGE, SC_TURNS, SC_FOLIAGE, SC_N };
static const char *SC_NAME[SC_N] = {
	"clean-cruise", "obstacle-flyby", "range-edge-fade",
	"sharp-turn-nulls", "foliage-sustained",
};
static const char *SC_DESC[SC_N] = {
	"~0% loss, steady",
	"0.2s 90%-loss bursts every 1.5s (multipath behind obstacles)",
	"loss ramps 0->peak->0 (fly to range edge and back)",
	"0.08s near-total dropouts every 1.0s (antenna nulls on hard turns)",
	"sustained ~moderate loss plateau (flying through foliage)",
};

/* Per-frame stress in [0,1]. */
static float stress_at(int scn, int f)
{
	float t = (float)f / FPS;
	switch (scn) {
	case SC_CLEAN:    return 0.02f;
	case SC_OBSTACLE: return (fmodf(t, 1.5f) < 0.2f) ? 0.90f : 0.05f;
	case SC_RANGE: {
		float h = (float)f / NFRAMES;            /* 0..1 */
		float tri = (h < 0.5f) ? h * 2.0f : (1.0f - h) * 2.0f;
		return 0.80f * tri;
	}
	case SC_TURNS:    return (fmodf(t, 1.0f) < 0.08f) ? 1.00f : 0.00f;
	case SC_FOLIAGE:  return 0.40f * (t < 1.0f ? t : 1.0f);
	}
	return 0.0f;
}

/* Pre-generated flight: identical for both modes (fairness). */
static uint32_t g_size[NFRAMES];
static uint8_t  g_lost[NFRAMES][MAXP];   /* 1 = packet lost on the channel */

static void generate_flight(int scn, uint64_t seed)
{
	g_rng = seed;
	int bad = 0;
	for (int f = 0; f < NFRAMES; f++) {
		/* GOP-60: I-frame 40-52 KB, P-frame 4-13 KB. */
		g_size[f] = (f % 60 == 0)
		    ? (uint32_t)(40000 + urand() * 12000)
		    : (uint32_t)(4000  + urand() * 9000);

		float s    = stress_at(scn, f);
		double pGB  = s * 0.05;            /* burst entry per packet           */
		double pBG  = 0.25;                /* mean burst ~4 packets            */
		double pG   = 0.001;               /* good-state residual loss         */
		double pB   = 0.50 + 0.40 * s;     /* in-burst loss 50-90%             */
		for (int p = 0; p < MAXP; p++) {
			if (!bad) { if (urand() < pGB) bad = 1; }
			else      { if (urand() < pBG) bad = 0; }
			g_lost[f][p] = (urand() < (bad ? pB : pG)) ? 1 : 0;
		}
	}
}

typedef struct {
	long   frames, delivered;
	long   sumN, sumK;
	double sumRed;
	int    peakN;
	double worst_resid;     /* peak smoothed residual loss the loop rode */
} Metrics;

static Config bench_cfg(int loss_adapt)
{
	Config cfg;
	config_defaults(&cfg);
	cfg.fec.loss_adapt      = loss_adapt;
	cfg.fec.startup_grace_s = 0.2f;   /* commit fast so the 12 s window counts */
	cfg.fec.airtime_max_pps = 0;      /* rail off here (covered by unit test)  */
	return cfg;
}

/* Run one (scenario, mode); flight arrays must already be generated.
 * If csv != NULL, dump per-frame trace (adaptive runs only). */
static Metrics run(int loss_adapt, FILE *csv)
{
	Config cfg = bench_cfg(loss_adapt);
	Controller c = {0};
	HeadroomRing ring = {0};
	FecParams out;
	Metrics m = {0};

	float hist_loss[NFRAMES], hist_recov[NFRAMES];
	float lossfb = 0.0f, recovfb = 0.0f;
	uint64_t now = BASE_US;

	for (int f = 0; f < NFRAMES; f++) {
		float in_loss  = (f >= FB_DELAY) ? hist_loss[f - FB_DELAY]  : 0.0f;
		float in_recov = (f >= FB_DELAY) ? hist_recov[f - FB_DELAY] : 0.0f;

		controller_update(&c, &cfg, g_size[f], &ring, now,
		                   in_loss, in_recov, true, (float)FPS, &out);

		double resid = 0.0, recov = 0.0;
		int k = c.current.k, n = c.current.n;
		if (c.have_current && n > 0 && k > 0) {
			int lost = 0, data_lost = 0;
			for (int p = 0; p < n; p++)      lost      += g_lost[f][p];
			for (int p = 0; p < k; p++)      data_lost += g_lost[f][p];
			int ok = (lost <= n - k);

			m.frames++;
			if (ok) m.delivered++;
			m.sumN += n; m.sumK += k;
			m.sumRed += 1.0 - (double)k / (double)n;
			if (n > m.peakN) m.peakN = n;

			resid = ok ? 0.0 : (double)data_lost / (double)k;
			recov = ok ? (double)data_lost / (double)k : 0.0;

			if (csv)
				fprintf(csv, "%d,%.3f,%.3f,%d,%d,%.3f,%.3f,%d\n",
				    f, (float)f / FPS, stress_at(SC_RANGE, f), k, n,
				    c.current.redundancy, c.current.r_loss_bias, ok);
		}

		lossfb  = 0.5f * (float)resid + 0.5f * lossfb;     /* scorer alpha 0.5 */
		recovfb = 0.5f * (float)recov + 0.5f * recovfb;
		hist_loss[f]  = lossfb;
		hist_recov[f] = recovfb;
		if (lossfb > m.worst_resid) m.worst_resid = lossfb;
		now += DT_US;
	}
	return m;
}

static void report(int scn, Metrics st, Metrics ad)
{
	double st_del = 100.0 * st.delivered / st.frames;
	double ad_del = 100.0 * ad.delivered / ad.frames;
	double st_pps = (double)st.sumN / DUR_S;
	double ad_pps = (double)ad.sumN / DUR_S;
	double air_ovh = 100.0 * ((double)ad.sumN - st.sumN) / st.sumN;
	long   st_drop = st.frames - st.delivered;
	long   ad_drop = ad.frames - ad.delivered;

	printf("\n== %-18s %s\n", SC_NAME[scn], SC_DESC[scn]);
	printf("   %-9s  %7s %7s %8s %7s %8s\n",
	       "mode", "deliv%", "drops", "mean_n", "peak_n", "pps");
	printf("   %-9s  %7.2f %7ld %8.1f %7d %8.0f\n",
	       "static",   st_del, st_drop, (double)st.sumN / st.frames, st.peakN, st_pps);
	printf("   %-9s  %7.2f %7ld %8.1f %7d %8.0f\n",
	       "adaptive", ad_del, ad_drop, (double)ad.sumN / ad.frames, ad.peakN, ad_pps);
	printf("   -> delivery %+.2f pp  (%ld -> %ld drops),  airtime %+.1f%%\n",
	       ad_del - st_del, st_drop, ad_drop, air_ovh);
}

int main(void)
{
	printf("Adaptive-n FPV loss bench  (%d fps, %d s/scenario, GE burst channel, "
	       "%d-frame feedback delay)\n", FPS, DUR_S, FB_DELAY);
	printf("static = fec.loss_adapt off (curve only) | adaptive = on "
	       "(gain=3.0 recov=0.5 ceil=0.60 decay=2.0s)\n");

	Metrics agg_st = {0}, agg_ad = {0};
	for (int scn = 0; scn < SC_N; scn++) {
		/* identical channel+frames per scenario for both modes */
		generate_flight(scn, 0xF1A17 + scn);
		Metrics st = run(0, NULL);
		generate_flight(scn, 0xF1A17 + scn);
		FILE *csv = NULL;
		if (scn == SC_RANGE) {
			csv = fopen("build/bench_range_fade.csv", "w");
			if (csv) fprintf(csv, "frame,t_s,stress,k,n,redundancy,r_loss_bias,delivered\n");
		}
		Metrics ad = run(1, csv);
		if (csv) { fclose(csv); }
		report(scn, st, ad);

		agg_st.frames += st.frames; agg_st.delivered += st.delivered; agg_st.sumN += st.sumN;
		agg_ad.frames += ad.frames; agg_ad.delivered += ad.delivered; agg_ad.sumN += ad.sumN;
	}

	double st_del = 100.0 * agg_st.delivered / agg_st.frames;
	double ad_del = 100.0 * agg_ad.delivered / agg_ad.frames;
	double air_ovh = 100.0 * ((double)agg_ad.sumN - agg_st.sumN) / agg_st.sumN;
	printf("\n== AGGREGATE (all scenarios)\n");
	printf("   static   delivery %.2f%%\n", st_del);
	printf("   adaptive delivery %.2f%%   (%+.2f pp,  airtime %+.1f%%)\n",
	       ad_del, ad_del - st_del, air_ovh);
	printf("\n   wrote build/bench_range_fade.csv (adaptive per-frame trace)\n");
	return 0;
}
