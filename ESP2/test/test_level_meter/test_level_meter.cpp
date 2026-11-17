/* Host tests for include/level_meter.h (web v.4 "pure ultrasonic").
 *     pio test -e native -f test_level_meter
 * Needs a host C++ compiler (see platformio.ini). */
#include <unity.h>
#include "level_meter.h"

void setUp(void) {}
void tearDown(void) {}

static void feed(LevelFilt &f, float L, uint32_t &t, int n, float jump = 1.5f) {
  for (int i = 0; i < n; i++) { t += 300; lfPush(f, L, t, jump); }
}

/* ---- filter ------------------------------------------------------------------------------------ */
void test_needs_three_samples(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 2);
  TEST_ASSERT_FALSE(lfValid(f));
  feed(f, 5.0f, t, 1);
  TEST_ASSERT_TRUE(lfValid(f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
}
void test_no_reading_is_ignored_and_goes_stale(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 5);
  uint32_t last = f.lastMs;
  TEST_ASSERT_FALSE(lfPush(f, -1.0f, t + 300, 1.5f));
  TEST_ASSERT_EQUAL_UINT32(last, f.lastMs);
  TEST_ASSERT_TRUE(lfFresh(f, last + 2000, 3000));
  TEST_ASSERT_FALSE(lfFresh(f, last + 3000, 3000));
}
void test_single_spike_never_reaches_the_median(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 5);
  t += 300; TEST_ASSERT_FALSE(lfPush(f, 12.0f, t, 1.5f));          // splash under the transducer
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
  feed(f, 5.0f, t, 1);
  t += 300; TEST_ASSERT_FALSE(lfPush(f, 0.0f, t, 1.5f));           // a multipath "empty"
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
  TEST_ASSERT_EQUAL_UINT32(2, f.rejects);
}
void test_small_outlier_inside_the_gate_is_medianed_out(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 4);
  t += 300; TEST_ASSERT_TRUE(lfPush(f, 6.2f, t, 1.5f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
}
void test_real_step_is_believed_after_three_agreeing(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 5);
  t += 300; lfPush(f, 9.0f, t, 1.5f);
  t += 300; lfPush(f, 9.1f, t, 1.5f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
  t += 300; TEST_ASSERT_TRUE(lfPush(f, 9.05f, t, 1.5f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 9.05f, f.med);
}
void test_scattered_outliers_never_make_a_step(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  feed(f, 5.0f, t, 5);
  t += 300; lfPush(f, 9.0f, t, 1.5f);
  t += 300; lfPush(f, 1.0f, t, 1.5f);                               // disagrees with 9.0: run restarts
  t += 300; lfPush(f, 12.0f, t, 1.5f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, f.med);
}
void test_follows_a_steady_fill(void) {
  LevelFilt f; lfReset(f); uint32_t t = 0;
  float L = 2.0f;
  for (int i = 0; i < 40; i++) { L += 0.04f; t += 300; lfPush(f, L, t, 1.5f); }   // ~8 L/min
  TEST_ASSERT_FLOAT_WITHIN(0.15f, L, f.med);                       // the median lags ~2 samples
}

/* ---- stage ------------------------------------------------------------------------------------- */
void test_stage_progress_and_no_flow(void) {
  LevelStage s; lsBegin(s, +1, 3.0f, 0);
  lsUpdate(s, 3.1f, 1000, 0.25f, 0.4f);
  TEST_ASSERT_FALSE(s.moved);
  TEST_ASSERT_TRUE(lsNoFlow(s, 20000, 20000));
  lsUpdate(s, 3.5f, 2000, 0.25f, 0.4f);
  TEST_ASSERT_TRUE(s.moved);
  TEST_ASSERT_FALSE(lsNoFlow(s, 30000, 20000));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, lsMoved(s, 3.5f));
}
void test_drain_moves_negative_level_positive_progress(void) {
  LevelStage s; lsBegin(s, -1, 10.0f, 0);
  lsUpdate(s, 9.0f, 1000, 0.25f, 0.4f);
  TEST_ASSERT_TRUE(s.moved);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, lsMoved(s, 9.0f));
}
void test_stall_after_progress(void) {
  LevelStage s; lsBegin(s, +1, 0.0f, 0);
  uint32_t t = 0; float L = 0.0f;
  for (int i = 0; i < 20; i++) { t += 1000; L += 0.13f; lsUpdate(s, L, t, 0.25f, 0.4f); }
  TEST_ASSERT_FALSE(lsStalled(s, t, 12000));
  for (int i = 0; i < 13; i++) { t += 1000; lsUpdate(s, L + ((i & 1) ? 0.1f : -0.1f), t, 0.25f, 0.4f); }  // ripple only
  TEST_ASSERT_TRUE(lsStalled(s, t, 12000));
}
void test_pause_shift_keeps_stall_clock(void) {
  LevelStage s; lsBegin(s, +1, 0.0f, 0);
  lsUpdate(s, 1.0f, 1000, 0.25f, 0.4f);
  lsShift(s, 15000);                                                // a 15 s re-seat
  TEST_ASSERT_FALSE(lsStalled(s, 17000, 12000));
}

/* ---- stuck selector ---------------------------------------------------------------------------- */
void test_wrong_way_transfer_selected_tank_falls(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f, levelWrongWayL(+1, -0.8f, 0.0f, true));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, levelWrongWayL(+1, +0.8f, -0.8f, true));   // the right way
}
void test_wrong_way_booster_selected_tank_rises_or_reservoir_falls(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.7f, levelWrongWayL(-1, +0.7f, 0.0f, true));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.9f, levelWrongWayL(-1, 0.1f, -0.9f, true));    // reservoir is the witness
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1f, levelWrongWayL(-1, 0.1f, -0.9f, false));   // ... only when readable (the tank's own 0.1 L rise still counts)
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, levelWrongWayL(-1, -1.0f, 0.0f, true));    // the right way
}
void test_wrong_way_must_last_and_grow(void) {
  TEST_ASSERT_FALSE(levelWrongConfirmed(0.9f, 0.6f, 1500, 2000, 0.15f));   // too short
  TEST_ASSERT_FALSE(levelWrongConfirmed(0.65f, 0.6f, 2500, 2000, 0.15f));  // not growing (a slosh)
  TEST_ASSERT_TRUE(levelWrongConfirmed(0.9f, 0.6f, 2500, 2000, 0.15f));
}

/* ---- reservoir cross-check --------------------------------------------------------------------- */
void test_levels_disagree(void) {
  TEST_ASSERT_FALSE(levelsDisagree(5.0f, 4.6f, 1.0f, 0.35f));
  TEST_ASSERT_FALSE(levelsDisagree(2.0f, 1.2f, 1.0f, 0.35f));     // within the 1 L floor
  TEST_ASSERT_TRUE(levelsDisagree(5.0f, 0.5f, 1.0f, 0.35f));      // the reservoir barely moved
}

/* ---- fill estimate: reservoir basis, mixing-tank witness ------------------------------------------ */
void test_xfer_agreeing_tanks_are_weighted(void) {
  XferEst e = xferEstimate(4.0f, true, 4.4f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_EQUAL_UINT8(XF_BOTH, e.src);
  TEST_ASSERT_FALSE(e.disagree);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4.1f, e.L);                       // 0.75*4.0 + 0.25*4.4
}
void test_xfer_mix_ripple_moves_the_estimate_little(void) {
  // the mixing tank's waves swing its reading +-0.6 L; the estimate moves a quarter of that
  XferEst hi = xferEstimate(3.0f, true, 3.6f, true, 0.75f, 1.0f, 0.35f);
  XferEst lo = xferEstimate(3.0f, true, 2.4f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.3f, hi.L - lo.L);
}
void test_xfer_disagreement_stays_on_the_reservoir_but_is_flagged(void) {
  XferEst e = xferEstimate(2.0f, true, 6.0f, true, 0.75f, 1.0f, 0.35f);   // band = max(1, 0.35*6) = 2.1
  TEST_ASSERT_TRUE(e.disagree);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f + 0.25f * 2.1f, e.L);       // pulled by at most a quarter of the band
}
void test_xfer_one_source_alone(void) {
  XferEst r = xferEstimate(3.0f, true, 0.0f, false, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_EQUAL_UINT8(XF_RES, r.src); TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, r.L);
  XferEst m = xferEstimate(0.0f, false, 2.5f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_EQUAL_UINT8(XF_MIX, m.src); TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f, m.L);
  XferEst n = xferEstimate(0.0f, false, 0.0f, false, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_EQUAL_UINT8(XF_NONE, n.src);
}
void test_xfer_done_on_estimate_or_on_a_clear_mixing_tank(void) {
  XferEst e = xferEstimate(4.9f, true, 5.0f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_TRUE(xferDone(e, 5.0f, true, 5.0f, 0.15f, 1.0f));    // 4.925 >= 5 - 0.15
  e = xferEstimate(3.0f, true, 3.2f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_FALSE(xferDone(e, 3.2f, true, 5.0f, 0.15f, 1.0f));
  // the reservoir under-reads (only 2 L) while the mixing tank clearly holds the 4.5 L and more: stop anyway
  e = xferEstimate(2.0f, true, 5.6f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_TRUE(xferDone(e, 5.6f, true, 4.5f, 0.15f, 1.0f));
  // ... but a mixing tank only a ripple above the target cannot end it alone
  e = xferEstimate(3.0f, true, 5.3f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_FALSE(xferDone(e, 5.3f, true, 5.0f, 0.15f, 1.0f));
  // ... and the mixing tank alone (reservoir unreadable) is judged by the estimate itself
  e = xferEstimate(0.0f, false, 4.9f, true, 0.75f, 1.0f, 0.35f);
  TEST_ASSERT_TRUE(xferDone(e, 4.9f, true, 5.0f, 0.15f, 1.0f));
}

/* ---- which pump really runs: the 10 s verdict (2026-11-16 user rule) ------------------------------------ */
void test_verdict_transfer_ok_when_mix_rises_or_res_drops(void) {
  TEST_ASSERT_EQUAL_UINT8(PV_OK, pumpVerdict(+1, 1.2f, -1.1f, true, true, 0.4f, 0.6f));
  TEST_ASSERT_EQUAL_UINT8(PV_OK, pumpVerdict(+1, 0.1f, -0.9f, true, true, 0.4f, 0.6f));   // water left the reservoir
}
void test_verdict_transfer_but_booster_runs(void) {
  TEST_ASSERT_EQUAL_UINT8(PV_WRONG,  pumpVerdict(+1, -0.8f, 0.0f, true, true, 0.4f, 0.6f));   // the mixing tank falls
  TEST_ASSERT_EQUAL_UINT8(PV_NOMOVE, pumpVerdict(+1, 0.1f, -0.1f, true, true, 0.4f, 0.6f));   // nothing moves, reservoir full
}
void test_verdict_transfer_unsure_without_a_readable_full_reservoir(void) {
  TEST_ASSERT_EQUAL_UINT8(PV_UNSURE, pumpVerdict(+1, 0.1f, 0.0f, true, false, 0.4f, 0.6f));   // reservoir in its filter zone
  TEST_ASSERT_EQUAL_UINT8(PV_UNSURE, pumpVerdict(+1, 0.1f, 0.0f, false, false, 0.4f, 0.6f));  // reservoir unreadable
}
void test_verdict_booster(void) {
  TEST_ASSERT_EQUAL_UINT8(PV_OK,     pumpVerdict(-1, -1.0f, 0.0f, true, true, 0.4f, 0.6f));
  TEST_ASSERT_EQUAL_UINT8(PV_WRONG,  pumpVerdict(-1, 0.1f, -0.9f, true, true, 0.4f, 0.6f));   // the reservoir loses water
  TEST_ASSERT_EQUAL_UINT8(PV_WRONG,  pumpVerdict(-1, 0.8f, 0.0f, false, false, 0.4f, 0.6f));  // the mixing tank rises
  TEST_ASSERT_EQUAL_UINT8(PV_OK,     pumpVerdict(-1, -0.9f, -0.9f, true, true, 0.4f, 0.6f));  // draining: not the transfer
  TEST_ASSERT_EQUAL_UINT8(PV_UNSURE, pumpVerdict(-1, 0.0f, 0.0f, true, true, 0.4f, 0.6f));    // nothing: no-flow rule decides
}

/* ---- reservoir empty, judged on the mixing tank (2026-11-16 user rule) ---------------------------------- */
void test_empty_window_rises_then_stops(void) {
  EmptyWin w; ewReset(w);
  TEST_ASSERT_FALSE(ewTick(w, false, 5.0f, 0, 10000, 0.3f));        // not in the filter zone: nothing
  TEST_ASSERT_FALSE(ewTick(w, true, 5.0f, 1000, 10000, 0.3f));      // opens
  TEST_ASSERT_FALSE(ewTick(w, true, 5.1f, 9000, 10000, 0.3f));      // inside the 10 s
  TEST_ASSERT_FALSE(ewTick(w, true, 6.2f, 11000, 10000, 0.3f));     // +1.2 L: still coming, next window
  TEST_ASSERT_FALSE(ewTick(w, true, 6.3f, 20000, 10000, 0.3f));
  TEST_ASSERT_TRUE (ewTick(w, true, 6.35f, 21000, 10000, 0.3f));    // +0.15 L in 10 s: the reservoir is empty
}
void test_empty_window_closes_outside_the_zone(void) {
  EmptyWin w; ewReset(w);
  ewTick(w, true, 5.0f, 0, 10000, 0.3f);
  TEST_ASSERT_FALSE(ewTick(w, false, 5.0f, 5000, 10000, 0.3f));     // back above the zone: closed
  TEST_ASSERT_FALSE(ewTick(w, true, 5.0f, 12000, 10000, 0.3f));     // reopens from here, not judged yet
}

/* ---- calibrated pump rate x run time (2026-11-17) ------------------------------------------------------ */
void test_rate_agreeing_pulls_by_the_weight(void) {
  RateMix m = rateBlend(10.0f, 10.4f, 0.4f, 1.0f, 0.25f);          // tol = 2.6 L, d = 0.4 L
  TEST_ASSERT_FALSE(m.disagree);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f + 0.4f * 0.4f * (1.0f - 0.4f / 2.6f), m.L);
  RateMix same = rateBlend(5.0f, 5.0f, 0.4f, 1.0f, 0.25f);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 5.0f, same.L);
}
void test_rate_far_off_is_ignored_and_flagged(void) {
  RateMix m = rateBlend(4.0f, 9.0f, 0.4f, 1.0f, 0.25f);            // tol = 2.25 L, d = 5 L
  TEST_ASSERT_TRUE(m.disagree);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 4.0f, m.L);
}
void test_rate_pull_never_jumps_and_is_bounded(void) {
  // the pull w*d*(1-|d|/tol) peaks at w*tol/4 (|d| = tol/2) and is 0 at the band edge
  float prev = 0.0f, peak = 0.0f;
  for (int i = 0; i <= 100; i++) {
    float d = 1.0f * i / 100.0f;                                       // frac 0: tol = minL = 1 L
    float pull = rateBlend(10.0f, 10.0f + d, 0.4f, 1.0f, 0.0f).L - 10.0f;
    TEST_ASSERT_TRUE(fabsf(pull - prev) < 0.02f);                      // continuous: no step
    if (pull > peak) peak = pull;
    prev = pull;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.4f * 1.0f / 4.0f, peak);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, prev);                       // at the edge: no pull
}
void test_rate_bad_inputs_leave_the_levels(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.0f, rateBlend(3.0f, -1.0f, 0.4f, 1.0f, 0.25f).L);   // no rate
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.0f, rateBlend(3.0f, NAN, 0.4f, 1.0f, 0.25f).L);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.0f, rateBlend(3.0f, 3.5f, 0.0f, 1.0f, 0.25f).L);     // weight 0
}
void test_measured_rate_and_weak_pump(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, measuredLpm(4.0f, 30000, 20000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, measuredLpm(1.0f, 10000, 20000));   // too short to say
  TEST_ASSERT_TRUE (pumpWeak(1.4f, 3.0f, 0.5f));
  TEST_ASSERT_FALSE(pumpWeak(1.6f, 3.0f, 0.5f));
  TEST_ASSERT_FALSE(pumpWeak(0.0f, 0.0f, 0.5f));                        // no calibrated rate: never weak
}

// The web v.3 log of 17 Nov 08:01: 5 L "delivered" within a second of the booster coming on.
void test_pace_rejects_an_instant_finish(void) {
  TEST_ASSERT_FALSE(paceOk(5.0f, 1000, 6.0f, 3.0f, 0.6f));      // 5 L in 1 s at 6 L/min: impossible
  TEST_ASSERT_TRUE(paceOk(5.0f, 50000, 6.0f, 3.0f, 0.6f));      // 5 L in 50 s: fine
  TEST_ASSERT_TRUE(paceOk(5.0f, 16700, 6.0f, 3.0f, 0.6f));      // 3x the rate: still believable
  TEST_ASSERT_TRUE(paceOk(0.5f, 100, 6.0f, 3.0f, 0.6f));        // inside the level noise: not judged
  TEST_ASSERT_TRUE(paceOk(5.0f, 1000, 0.0f, 3.0f, 0.6f));       // no rate: not judged
  TEST_ASSERT_TRUE(paceOk(NAN, 1000, 6.0f, 3.0f, 0.6f));
}
void test_burst_limit(void) {
  // K 450, 5000 mL/min ceiling, 200 ms: 450*5*0.2/60*1.5 + 3 = 14.25 -> 14
  TEST_ASSERT_EQUAL_UINT32(14, burstMaxPulses(450.0f, 5.0f, 200));
  // a measured 50 mL/min pump x3: 450*0.15*0.2/60*1.5+3 = 3.3 -> 3; the real flow (0.075 pulse) never trips it
  TEST_ASSERT_EQUAL_UINT32(3, burstMaxPulses(450.0f, 0.15f, 200));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, burstMaxPulses(0.0f, 5.0f, 200));
}

// The user's line (17 Nov): V = 0.47 + 0.1957 t from the switch-on (11.74 L/min), at least 6 s.
void test_xfer_run_time_from_the_bucket_line(void) {
  const float q = 11.74f / 60.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 23.1f, xferRunS(5.0f, q, 0.47f, 6.0f));    // 5 L -> ~23 s
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 99.8f, xferRunS(20.0f, q, 0.47f, 6.0f));   // 20 L -> ~100 s
  TEST_ASSERT_EQUAL_FLOAT(6.0f, xferRunS(0.5f, q, 0.47f, 6.0f));            // never under the measured range
  TEST_ASSERT_EQUAL_FLOAT(6.0f, xferRunS(5.0f, 0.0f, 0.47f, 6.0f));         // no rate: the shortest run
}
void test_xfer_litres_ramp_then_line(void) {
  const float q = 11.74f / 60.0f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, xferLitresAt(0.0f, q, 0.47f, 6.0f));
  float at6 = 0.47f + q * 6.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, at6 / 2.0f, xferLitresAt(3.0f, q, 0.47f, 6.0f));   // the ramp
  TEST_ASSERT_FLOAT_WITHIN(0.001f, at6, xferLitresAt(6.0f, q, 0.47f, 6.0f));          // meets the line
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, xferLitresAt(xferRunS(5.0f, q, 0.47f, 6.0f), q, 0.47f, 6.0f));
}
void test_settle_watch(void) {
  SteadyWatch w; swReset(w);
  TEST_ASSERT_FALSE(swSample(w, 10.0f, 0, 0.3f, 10000));
  TEST_ASSERT_FALSE(swSample(w, 10.2f, 5000, 0.3f, 10000));
  TEST_ASSERT_TRUE(swSample(w, 10.1f, 10000, 0.3f, 10000));     // held within 0.3 L for 10 s
  swReset(w);
  swSample(w, 10.0f, 0, 0.3f, 10000);
  TEST_ASSERT_FALSE(swSample(w, 10.5f, 6000, 0.3f, 10000));     // moved: the window starts again
  TEST_ASSERT_FALSE(swSample(w, 10.5f, 15000, 0.3f, 10000));
  TEST_ASSERT_TRUE(swSample(w, 10.4f, 16000, 0.3f, 10000));
}
void test_topup_rule(void) {
  TEST_ASSERT_FALSE(fillNeedsTopup(10.0f, 7.0f, 3.0f, 0, 2));   // exactly 3 L short: carry on (user rule)
  TEST_ASSERT_TRUE(fillNeedsTopup(10.0f, 6.9f, 3.0f, 0, 2));    // more than 3 L short: run again
  TEST_ASSERT_FALSE(fillNeedsTopup(10.0f, 2.0f, 3.0f, 2, 2));   // top-ups used up
  TEST_ASSERT_FALSE(fillNeedsTopup(10.0f, 11.0f, 3.0f, 0, 2));  // over: never
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_needs_three_samples);
  RUN_TEST(test_no_reading_is_ignored_and_goes_stale);
  RUN_TEST(test_single_spike_never_reaches_the_median);
  RUN_TEST(test_small_outlier_inside_the_gate_is_medianed_out);
  RUN_TEST(test_real_step_is_believed_after_three_agreeing);
  RUN_TEST(test_scattered_outliers_never_make_a_step);
  RUN_TEST(test_follows_a_steady_fill);
  RUN_TEST(test_stage_progress_and_no_flow);
  RUN_TEST(test_drain_moves_negative_level_positive_progress);
  RUN_TEST(test_stall_after_progress);
  RUN_TEST(test_pause_shift_keeps_stall_clock);
  RUN_TEST(test_wrong_way_transfer_selected_tank_falls);
  RUN_TEST(test_wrong_way_booster_selected_tank_rises_or_reservoir_falls);
  RUN_TEST(test_wrong_way_must_last_and_grow);
  RUN_TEST(test_levels_disagree);
  RUN_TEST(test_xfer_agreeing_tanks_are_weighted);
  RUN_TEST(test_xfer_mix_ripple_moves_the_estimate_little);
  RUN_TEST(test_xfer_disagreement_stays_on_the_reservoir_but_is_flagged);
  RUN_TEST(test_xfer_one_source_alone);
  RUN_TEST(test_xfer_done_on_estimate_or_on_a_clear_mixing_tank);
  RUN_TEST(test_verdict_transfer_ok_when_mix_rises_or_res_drops);
  RUN_TEST(test_verdict_transfer_but_booster_runs);
  RUN_TEST(test_verdict_transfer_unsure_without_a_readable_full_reservoir);
  RUN_TEST(test_verdict_booster);
  RUN_TEST(test_empty_window_rises_then_stops);
  RUN_TEST(test_empty_window_closes_outside_the_zone);
  RUN_TEST(test_rate_agreeing_pulls_by_the_weight);
  RUN_TEST(test_rate_far_off_is_ignored_and_flagged);
  RUN_TEST(test_rate_pull_never_jumps_and_is_bounded);
  RUN_TEST(test_rate_bad_inputs_leave_the_levels);
  RUN_TEST(test_measured_rate_and_weak_pump);
  RUN_TEST(test_pace_rejects_an_instant_finish);
  RUN_TEST(test_burst_limit);
  RUN_TEST(test_xfer_run_time_from_the_bucket_line);
  RUN_TEST(test_xfer_litres_ramp_then_line);
  RUN_TEST(test_settle_watch);
  RUN_TEST(test_topup_rule);
  return UNITY_END();
}
