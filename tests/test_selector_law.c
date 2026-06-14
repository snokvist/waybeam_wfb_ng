/* test_selector_law.c — host unit test for the MCS control law's transport-
 * backpressure coupling (anti death-spiral, link_controller.c).
 *
 * Drives selector_update() directly with synthetic Score / ProbeState / now /
 * in_pressure so each decision branch is exercised deterministically (no
 * sockets, no real clock). Built by `make test` in vehicle/ with
 * -DLC_TEST_NO_MAIN so the daemon's main() is excluded and the whole unit is
 * #included here.
 *
 * Covers:
 *   regression  — with no backpressure the law is unchanged (probe-fail demote,
 *                 probe-clean promote, reactive-PER demote, RSSI-floor demote).
 *   rule #1     — under in_pressure the PER/probe-fail demotes are suppressed
 *                 (the "loss" is local airtime starvation, not RF).
 *   RF-still    — RSSI floor/fade demotes STILL fire under in_pressure.
 *   rule #2     — in_pressure held >= pressure_escape_s with clean RF promotes.
 *   gated       — the escape does NOT fire when RF is near the floor.
 *   reset       — pressure_since_us clears the moment in_pressure drops.
 */
#ifndef LC_TEST_NO_MAIN   /* normally set by the Makefile -D; guard here too */
#define LC_TEST_NO_MAIN
#endif
#include "../vehicle/link_controller.c"

#include <stdio.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
	else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

#define BASE      ((uint64_t)1000 * 1000 * 1000)   /* 1000 s in us */
#define SEC_US    ((uint64_t)1000 * 1000)

/* Fresh selector parked at a mid rung, last change well in the past so all
 * cooldowns / dwells are satisfied. */
static void mk_sel(Selector *s, int mcs)
{
	selector_init(s);
	s->current_mcs    = mcs;
	s->last_change_us = BASE - 10 * SEC_US;
	s->last_datagram_us = BASE;
}

static Score mk_score(float rssi, float slope, float lost_ratio)
{
	Score sc;
	memset(&sc, 0, sizeof sc);
	sc.raw_rssi            = rssi;
	sc.smoothed_rssi       = rssi;
	sc.rssi_slope_db_s     = slope;
	sc.smoothed_lost_ratio = lost_ratio;
	sc.adapter_count       = 2;
	sc.ant_count           = 2;
	return sc;
}

/* Probe with a single fresh V+2 rung reading (or no traffic if mcs < 0). */
static void mk_probe(ProbeState *p, int v2_mcs, int per_milli)
{
	probe_state_init(p);
	p->gate_us = 0;
	if (v2_mcs >= 0 && v2_mcs < PROBE_MCS_MAX) {
		p->rung[v2_mcs].valid     = true;
		p->rung[v2_mcs].per_milli = per_milli;
		p->rung[v2_mcs].last_us   = BASE;   /* fresh, >= gate_us */
		p->rung[v2_mcs].accounted = 20;
		p->last_any_us            = BASE;
	}
}

int main(void)
{
	Config cfg;
	config_defaults(&cfg);
	/* Defaults of record: mcs_min 0, mcs_max 5, demote_per_milli 30,
	 * probe_fail_milli 200, probe_clean_milli 20, rssi_floor -85,
	 * floor_hyst 6 (clean RF needs rssi > -79), pressure_demote_block on,
	 * pressure_escape_s 2.0. */
	const int START = 3;
	Selector s;
	Score sc;
	ProbeState pr;
	int out;
	SelectDecision d;

	printf("[regression — no backpressure]\n");
	/* probe-fail (V+2=5 at 700‰) on clean RF -> demote */
	mk_sel(&s, START); sc = mk_score(-40, 0, 0.0f); mk_probe(&pr, START + 2, 700);
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE, &out);
	CHECK(d == SD_DOWN && out == START - 1, "probe-fail demotes when not in_pressure");

	/* probe-clean (V+2=5 at 0‰) on clean RF -> promote */
	mk_sel(&s, START); sc = mk_score(-40, 0, 0.0f); mk_probe(&pr, START + 2, 0);
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE, &out);
	CHECK(d == SD_UP && out == START + 1, "probe-clean promotes when not in_pressure");

	/* reactive video PER 100‰ (>30) -> demote */
	mk_sel(&s, START); sc = mk_score(-40, 0, 0.10f); mk_probe(&pr, -1, 0);
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE, &out);
	CHECK(d == SD_DOWN && out == START - 1, "reactive-PER demotes when not in_pressure");

	/* RSSI floor (-90 <= -85) -> demote */
	mk_sel(&s, START); sc = mk_score(-90, 0, 0.0f); mk_probe(&pr, -1, 0);
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE, &out);
	CHECK(d == SD_DOWN && out == START - 1, "RSSI-floor demotes when not in_pressure");

	printf("[rule #1 — in_pressure suppresses PER/probe demotes]\n");
	/* probe-fail 700‰ + clean RF + in_pressure, but held=0 (escape not yet
	 * armed) -> NO demote, NO escape -> hold. */
	mk_sel(&s, START); sc = mk_score(-27, 0, 0.0f); mk_probe(&pr, START + 2, 700);
	s.pressure_since_us = BASE;   /* held = 0 < pressure_escape_s */
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_NONE, "probe-fail does NOT demote under in_pressure");

	/* reactive PER 100‰ + in_pressure (held=0) -> NO demote */
	mk_sel(&s, START); sc = mk_score(-27, 0, 0.10f); mk_probe(&pr, -1, 0);
	s.pressure_since_us = BASE;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_NONE, "reactive-PER does NOT demote under in_pressure");

	printf("[RF guards stay live under in_pressure]\n");
	/* RSSI floor -90 + in_pressure -> STILL demote (RF is transport-independent) */
	mk_sel(&s, START); sc = mk_score(-90, 0, 0.0f); mk_probe(&pr, -1, 0);
	s.pressure_since_us = BASE;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_DOWN && out == START - 1, "RSSI-floor demotes even under in_pressure");

	printf("[rule #2 — backpressure escape]\n");
	/* in_pressure held 3 s (>= 2.0) + clean RF + contaminated probe -> promote */
	mk_sel(&s, START); sc = mk_score(-27, 0, 0.0f); mk_probe(&pr, START + 2, 700);
	s.pressure_since_us = BASE - 3 * SEC_US;   /* held = 3 s */
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_UP && out == START + 1, "escape promotes after pressure held with clean RF");
	CHECK(s.pressure_escape_count == 1, "escape increments observability counter");

	/* held only 1 s (< 2.0) -> no escape yet (and demotes suppressed) -> hold */
	mk_sel(&s, START); sc = mk_score(-27, 0, 0.0f); mk_probe(&pr, START + 2, 700);
	s.pressure_since_us = BASE - 1 * SEC_US;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_NONE, "escape does NOT fire before pressure_escape_s elapses");

	printf("[escape gated by RF — near floor]\n");
	/* in_pressure held 3 s but RSSI -82 (<= floor+hyst -79) -> not clean -> no escape */
	mk_sel(&s, START); sc = mk_score(-82, 0, 0.0f); mk_probe(&pr, START + 2, 700);
	s.pressure_since_us = BASE - 3 * SEC_US;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_NONE, "escape does NOT fire when RF is near the floor");

	/* escape must not exceed mcs_max */
	mk_sel(&s, cfg.mcs.mcs_max); sc = mk_score(-27, 0, 0.0f);
	mk_probe(&pr, cfg.mcs.mcs_max + 2, 700);
	s.pressure_since_us = BASE - 3 * SEC_US;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(d == SD_NONE, "escape does NOT promote past mcs_max");

	printf("[pressure persistence reset]\n");
	/* in_pressure true sets the timer... */
	mk_sel(&s, START); sc = mk_score(-27, 0, 0.0f); mk_probe(&pr, START + 2, 100);
	(void)selector_update(&s, &cfg, &sc, &pr, true, BASE, &out);
	CHECK(s.pressure_since_us != 0, "pressure timer arms on in_pressure");
	/* ...then clears the moment it drops, and the law resumes (probe-fail demotes). */
	mk_probe(&pr, START + 2, 700);
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE + SEC_US, &out);
	CHECK(s.pressure_since_us == 0, "pressure timer clears when in_pressure drops");
	CHECK(d == SD_DOWN, "probe-fail demote resumes once pressure clears");

	printf("[hard flap-freeze — 2-rung trap]\n");
	/* Reproduce the mcs_max==mcs_min+1 trap: every promote back into the top
	 * rung re-fails on its own retune burst. Defaults: flap_freeze_count 3,
	 * window 6 s, hold 10 s. Drive 3 fast re-demotes FROM rung 1 (simulating
	 * the promote-back between them by resetting current_mcs) and confirm the
	 * freeze latches on the 3rd. */
	cfg.mcs.mcs_min = 0;
	cfg.mcs.mcs_max = 1;
	mk_sel(&s, 1);
	(void)selector_commit(&s, &cfg, 0, BASE,                SD_DOWN, &out);
	CHECK(s.flap_freeze_mcs < 0, "1st re-demote does not yet freeze");
	s.current_mcs = 1;
	(void)selector_commit(&s, &cfg, 0, BASE + 1 * SEC_US,   SD_DOWN, &out);
	CHECK(s.flap_freeze_mcs < 0, "2nd re-demote does not yet freeze");
	s.current_mcs = 1;
	(void)selector_commit(&s, &cfg, 0, BASE + 2 * SEC_US,   SD_DOWN, &out);
	CHECK(s.flap_freeze_mcs == 1 && s.flap_demote_streak == 3,
	      "3rd fast re-demote latches the freeze on rung 1");

	/* Frozen: a clean, FRESH V+2 probe that WOULD promote 0->1 is refused; the
	 * lower rung is held. Park last_change far back so dwell can't be the
	 * blocker, and stamp the probe at the call time so staleness can't be it
	 * either — the freeze is the only thing standing in the way. */
	s.current_mcs = 0; s.last_change_us = BASE - 10 * SEC_US;
	sc = mk_score(-40, 0, 0.0f); mk_probe(&pr, 2, 0);
	pr.rung[2].last_us = BASE + 2 * SEC_US; pr.last_any_us = BASE + 2 * SEC_US;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE + 2 * SEC_US, &out);
	CHECK(d == SD_NONE, "clean-probe promote is BLOCKED while frozen (holds lower rung)");

	/* After flap_freeze_s the freeze lifts and the (fresh) probe may climb. */
	s.current_mcs = 0; s.last_change_us = BASE - 10 * SEC_US;
	sc = mk_score(-40, 0, 0.0f); mk_probe(&pr, 2, 0);
	pr.rung[2].last_us = BASE + 13 * SEC_US; pr.last_any_us = BASE + 13 * SEC_US;
	out = -1; d = selector_update(&s, &cfg, &sc, &pr, false, BASE + 13 * SEC_US, &out);
	CHECK(d == SD_UP && out == 1, "freeze lifts after flap_freeze_s -> promote resumes");

	/* A real walk-away degrade steps down through DIFFERENT rungs (5->4->3...),
	 * even rapidly — the same-rung requirement keeps the streak at 1, so a
	 * genuine cascade never trips the freeze. */
	cfg.mcs.mcs_max = 5;
	mk_sel(&s, 5);
	(void)selector_commit(&s, &cfg, 4, BASE,                SD_DOWN, &out);
	(void)selector_commit(&s, &cfg, 3, BASE + 1 * SEC_US,   SD_DOWN, &out);
	(void)selector_commit(&s, &cfg, 2, BASE + 2 * SEC_US,   SD_DOWN, &out);
	CHECK(s.flap_freeze_mcs < 0 && s.flap_demote_streak == 1,
	      "multi-rung walk-away cascade does NOT freeze (different rungs)");

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	       g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
