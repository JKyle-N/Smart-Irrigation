/* Host-side tests for ESP1's pure sensor maths.  Run with:  pio test -e native
 *
 * These need no ESP32 and no rig. Cases lead with defects that were actually found in review, so
 * they stay found -- each of those is marked REGRESSION with a note on what went wrong.
 *
 * Calibration endpoints used throughout are column A's measured pair (dry 656 / wet 524 ADC).
 */
#include <unity.h>
#include "pure_math.h"
#include <stdio.h>          // snprintf (servo command checksum test)

static const int AIR_A = 656, WATER_A = 524;   // column A, measured on the rig

void setUp(void) {}
void tearDown(void) {}

/* ---- imap: Arduino map() semantics, including the truncation ------------- */

void test_imap_matches_arduino_map(void) {
  TEST_ASSERT_EQUAL_INT(0,   imap(0, 0, 10, 0, 100));
  TEST_ASSERT_EQUAL_INT(100, imap(10, 0, 10, 0, 100));
  TEST_ASSERT_EQUAL_INT(50,  imap(5, 0, 10, 0, 100));
  // Arduino map() truncates toward zero rather than rounding; the firmware has always relied on
  // that, so the extracted version must too.
  TEST_ASSERT_EQUAL_INT(33, imap(1, 0, 3, 0, 100));
}

void test_imap_degenerate_span_does_not_divide_by_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, imap(5, 10, 10, 0, 100));
}

/* ---- RTC sanity ---------------------------------------------------------- */

// REGRESSION: these exact bad dates are in the rig's logs, and nothing stopped them reaching the scheduler.
void test_rtc_rejects_the_bad_dates_seen_on_the_rig(void) {
  TEST_ASSERT_FALSE(rtcPlausible(2000, 1, 1, 0, 27, 32));     // DS3231 lost power
  TEST_ASSERT_FALSE(rtcPlausible(2000, 0, 0, 45, 161, 0));    // garbled read
  TEST_ASSERT_FALSE(rtcPlausible(2080, 13, 10, 5, 5, 5));
  TEST_ASSERT_FALSE(rtcPlausible(2026, 46, 65, 5, 5, 5));
}

void test_rtc_accepts_real_dates_and_checks_each_field(void) {
  TEST_ASSERT_TRUE(rtcPlausible(2026, 11, 1, 23, 59, 59));
  TEST_ASSERT_TRUE(rtcPlausible(2028, 2, 29, 0, 0, 0));       // leap day
  TEST_ASSERT_FALSE(rtcPlausible(2027, 2, 29, 0, 0, 0));
  TEST_ASSERT_FALSE(rtcPlausible(2026, 11, 31, 0, 0, 0));
  TEST_ASSERT_FALSE(rtcPlausible(2026, 11, 1, 24, 0, 0));
  TEST_ASSERT_FALSE(rtcPlausible(2026, 11, 1, 0, 60, 0));
  TEST_ASSERT_FALSE(rtcPlausible(2026, 11, 1, 0, 0, 60));
  TEST_ASSERT_FALSE(rtcPlausible(2100, 1, 1, 0, 0, 0));
}

void test_rtc_jump_works_in_both_directions(void) {
  TEST_ASSERT_FALSE(rtcJumped(1000, 1000, 120));
  TEST_ASSERT_FALSE(rtcJumped(1120, 1000, 120));
  TEST_ASSERT_TRUE(rtcJumped(1121, 1000, 120));
  TEST_ASSERT_FALSE(rtcJumped(880, 1000, 120));
  TEST_ASSERT_TRUE(rtcJumped(879, 1000, 120));
}

/* ---- soilCalOk ------------------------------------------------------------ */

// REGRESSION: two equal capture points were saved, which read as 0 % forever and watered every service.
void test_soil_cal_needs_dry_above_wet_by_a_margin(void) {
  TEST_ASSERT_TRUE(soilCalOk(656, 524, 20));                  // column A's measured pair
  TEST_ASSERT_TRUE(soilCalOk(544, 524, 20));                  // exactly the margin
  TEST_ASSERT_FALSE(soilCalOk(543, 524, 20));
  TEST_ASSERT_FALSE(soilCalOk(524, 524, 20));                 // equal
  TEST_ASSERT_FALSE(soilCalOk(524, 656, 20));                 // reversed
}

/* ---- soilPctCal ---------------------------------------------------------- */

void test_soil_pct_endpoints(void) {
  TEST_ASSERT_EQUAL_INT(0,   soilPctCal(AIR_A,   AIR_A, WATER_A));   // bone dry
  TEST_ASSERT_EQUAL_INT(100, soilPctCal(WATER_A, AIR_A, WATER_A));   // saturated
  TEST_ASSERT_EQUAL_INT(50,  soilPctCal(590,     AIR_A, WATER_A));   // midpoint of 656..524
}

void test_soil_pct_clamps_outside_the_span(void) {
  // Drier than the dry endpoint, and wetter than the wet endpoint. Neither may leave 0..100 --
  // an unclamped value here would feed a nonsense moisture into the irrigation decision.
  TEST_ASSERT_EQUAL_INT(0,   soilPctCal(700, AIR_A, WATER_A));
  TEST_ASSERT_EQUAL_INT(100, soilPctCal(400, AIR_A, WATER_A));
}

void test_soil_pct_survives_inverted_endpoints(void) {
  // If someone calibrates air/water the wrong way round the reading is meaningless, but it must
  // still be bounded rather than wild.
  int p = soilPctCal(590, WATER_A, AIR_A);
  TEST_ASSERT_TRUE(p >= 0 && p <= 100);
}

void test_soil_pct_equal_endpoints_is_zero_not_a_crash(void) {
  TEST_ASSERT_EQUAL_INT(0, soilPctCal(600, 600, 600));
}

/* ---- soilCombineCal: the NPK blending policy ----------------------------- */

void test_combine_capacitive_only_when_no_npk(void) {
  SoilBlend why;
  TEST_ASSERT_EQUAL_INT(50, soilCombineCal(590, 590, -1, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
}

void test_combine_blends_npk_when_it_agrees(void) {
  SoilBlend why;
  // cap = 50, npk = 56, within the 15-point tolerance -> (2*50 + 56)/3 = 52.
  // The NPK gets one vote against the pair's two, so its 6-point error moves the column by 2.
  TEST_ASSERT_EQUAL_INT(52, soilCombineCal(590, 590, 56, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_BLENDED, why);
}

void test_combine_excludes_npk_when_it_diverges(void) {
  SoilBlend why;
  // cap = 50, npk = 80: 30 points apart. The whole reason this gate exists -- an NPK probe in dry
  // soil reports confident nonsense, and averaging it in would drag the column with it.
  TEST_ASSERT_EQUAL_INT(50, soilCombineCal(590, 590, 80, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_NPK_DIVERGED, why);
}

void test_combine_agreement_boundary_is_inclusive(void) {
  SoilBlend why;
  // Exactly NPK_MOIST_AGREE_PCT apart still blends; one point further does not. Pinning the
  // boundary means a later tweak to the constant cannot silently flip the comparison.
  soilCombineCal(590, 590, 50 + NPK_MOIST_AGREE_PCT, AIR_A, WATER_A, &why);
  TEST_ASSERT_EQUAL_INT(SB_BLENDED, why);
  soilCombineCal(590, 590, 50 + NPK_MOIST_AGREE_PCT + 1, AIR_A, WATER_A, &why);
  TEST_ASSERT_EQUAL_INT(SB_NPK_DIVERGED, why);
}

void test_combine_divergence_is_symmetric(void) {
  SoilBlend why;
  // Reads far DRIER than the capacitive pair, not just far wetter. An abs() dropped from this
  // comparison would pass every test above and still let a low NPK reading through.
  soilCombineCal(590, 590, 50 - NPK_MOIST_AGREE_PCT - 1, AIR_A, WATER_A, &why);
  TEST_ASSERT_EQUAL_INT(SB_NPK_DIVERGED, why);
}

void test_combine_drops_a_railed_probe(void) {
  SoilBlend why;
  // One probe disconnected (reads a rail). The good probe must carry the column alone --
  // averaging the dead one in would peg the column dry and irrigate forever.
  TEST_ASSERT_EQUAL_INT(50, soilCombineCal(SOIL_ADC_RAIL_LO, 590, -1, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
  TEST_ASSERT_EQUAL_INT(50, soilCombineCal(590, SOIL_ADC_RAIL_HI, -1, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
}

void test_combine_rail_boundaries_are_exclusive(void) {
  SoilBlend why;
  // A reading exactly ON a rail is bad; one step inside is good. Both probes just inside the rails
  // must still combine normally.
  int in = soilCombineCal(SOIL_ADC_RAIL_LO + 1, SOIL_ADC_RAIL_HI - 1, -1, AIR_A, WATER_A, &why);
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
  TEST_ASSERT_TRUE(in >= 0 && in <= 100);
}

void test_combine_falls_back_to_npk_when_both_probes_are_dead(void) {
  SoilBlend why;
  // Keeps the plants watered on the one sensor still reporting. The caller still raises
  // SOIL_MISSING, so this does not hide the fault -- it only avoids blinding the column.
  TEST_ASSERT_EQUAL_INT(42, soilCombineCal(0, 2000, 42, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_NPK_FALLBACK, why);
}

void test_combine_reports_none_when_nothing_is_usable(void) {
  SoilBlend why;
  TEST_ASSERT_EQUAL_INT(-1, soilCombineCal(0, 2000, -1, AIR_A, WATER_A, &why));
  TEST_ASSERT_EQUAL_INT(SB_NONE, why);
}

void test_combine_tolerates_a_null_why_pointer(void) {
  TEST_ASSERT_EQUAL_INT(50, soilCombineCal(590, 590, -1, AIR_A, WATER_A, NULL));
}

/* ---- soilCombineProbes: per-probe calibration (2026-11-16) ---------------- */

void test_probes_each_use_their_own_endpoints(void) {
  SoilBlend why;
  // A1 748 dry / 392 wet, A2 661 / 518 (the rig's measured probes). Both at their own midpoint -> 50 %,
  // although their raw readings are 570 and 589.
  TEST_ASSERT_EQUAL_INT(50, soilCombineProbes(570, 589, -1, 748, 392, 661, 518, &why));
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
  // One saturated, one bone dry on their own endpoints -> 50 %.
  TEST_ASSERT_EQUAL_INT(50, soilCombineProbes(392, 661, -1, 748, 392, 661, 518, &why));
}

void test_probes_with_shared_endpoints_match_the_column_cal(void) {
  for (int v = 500; v <= 680; v += 7) {
    int a = soilCombineProbes(v, v + 10, -1, AIR_A, WATER_A, AIR_A, WATER_A, NULL);
    int b = soilCombineCal(v, v + 10, -1, AIR_A, WATER_A, NULL);
    TEST_ASSERT_INT_WITHIN(1, b, a);
  }
}

void test_probes_railed_one_uses_the_other_on_its_own_cal(void) {
  SoilBlend why;
  TEST_ASSERT_EQUAL_INT(50, soilCombineProbes(SOIL_ADC_RAIL_LO, 589, -1, 748, 392, 661, 518, &why));
  TEST_ASSERT_EQUAL_INT(SB_CAP_ONLY, why);
  TEST_ASSERT_EQUAL_INT(50, soilCombineProbes(570, SOIL_ADC_RAIL_HI, -1, 748, 392, 661, 518, &why));
}

void test_probes_both_dead_fall_back_or_none(void) {
  SoilBlend why;
  TEST_ASSERT_EQUAL_INT(42, soilCombineProbes(0, 2000, 42, 748, 392, 661, 518, &why));
  TEST_ASSERT_EQUAL_INT(SB_NPK_FALLBACK, why);
  TEST_ASSERT_EQUAL_INT(-1, soilCombineProbes(0, 2000, -1, 748, 392, 661, 518, &why));
  TEST_ASSERT_EQUAL_INT(SB_NONE, why);
}

void test_probes_npk_blend_unchanged(void) {
  SoilBlend why;
  TEST_ASSERT_EQUAL_INT(52, soilCombineProbes(570, 589, 56, 748, 392, 661, 518, &why));   // (2*50+56)/3
  TEST_ASSERT_EQUAL_INT(SB_BLENDED, why);
}

void test_soil_cal_steady_window(void) {
  TEST_ASSERT_FALSE(soilCalSteady(3, 5, 600, 601, 4));   // too few samples yet
  TEST_ASSERT_TRUE (soilCalSteady(5, 5, 600, 604, 4));   // spread 4: steady (inclusive)
  TEST_ASSERT_FALSE(soilCalSteady(8, 5, 600, 605, 4));   // still moving
}

/* ---- levelPct ------------------------------------------------------------ */

void test_level_pct_endpoints_and_midpoint(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(38.0f, 38.0f, 11.0f));   // empty
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, levelPct(11.0f, 38.0f, 11.0f));   // full
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f,  levelPct(24.5f, 38.0f, 11.0f));
}

void test_level_pct_floor_at_zero_and_past_full(void) {
  // web v.4 (2026-11-16): water closer than "full" reads PAST 100 (the tank is over its mark); an echo
  // past "empty" is 0, never negative; a nonsense reading stops at LEVEL_PCT_MAX.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 122.0f, levelPct(5.0f,  38.0f, 11.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(50.0f, 38.0f, 11.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, LEVEL_PCT_MAX, levelPct(-100.0f, 38.0f, 11.0f));
}

void test_level_pct_degenerate_geometry_returns_zero(void) {
  // REGRESSION: equal endpoints would divide by zero and yield inf/NaN, which propagates into the
  // fill logic as a level that compares false against every threshold.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, levelPct(20.0f, 30.0f, 30.0f));
}

void test_level_pct_returns_whole_percents(void) {
  float p = levelPct(24.6f, 38.0f, 11.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, p, roundf(p));   // no fractional part survives
}

/* ---- pulseRowForTarget --------------------------------------------------- */

void test_pulse_row_maps_every_known_target(void) {
  TEST_ASSERT_EQUAL_INT(17, pulseRowForTarget("colA"));
  TEST_ASSERT_EQUAL_INT(18, pulseRowForTarget("colB"));
  TEST_ASSERT_EQUAL_INT(19, pulseRowForTarget("colC"));
  TEST_ASSERT_EQUAL_INT(16, pulseRowForTarget("transfer"));   // fill COMBO, not the bare pump
  TEST_ASSERT_EQUAL_INT(20, pulseRowForTarget("servoL"));     // pump selector -> transfer side (servo only)
  TEST_ASSERT_EQUAL_INT(21, pulseRowForTarget("servoR"));     // pump selector -> booster side (servo only)
  TEST_ASSERT_EQUAL_INT(22, pulseRowForTarget("mixer"));      // combo row: the 12 V DC mixer relay
  TEST_ASSERT_EQUAL_INT(1,  pulseRowForTarget("nutA"));   // P1 since 2026-11-16 (A/B swapped)
  TEST_ASSERT_EQUAL_INT(7,  pulseRowForTarget("nutB"));   // P7 since 2026-11-16
  TEST_ASSERT_EQUAL_INT(11, pulseRowForTarget("nutC"));
  TEST_ASSERT_EQUAL_INT(12, pulseRowForTarget("nutD"));
  TEST_ASSERT_EQUAL_INT(13, pulseRowForTarget("phUp"));
  TEST_ASSERT_EQUAL_INT(14, pulseRowForTarget("phDn"));
}

void test_pulse_row_rejects_unknown_and_null(void) {
  // -1 is what stops an unrecognised dashboard target energising row 0 by accident.
  TEST_ASSERT_EQUAL_INT(-1, pulseRowForTarget("nope"));
  TEST_ASSERT_EQUAL_INT(-1, pulseRowForTarget(""));
  TEST_ASSERT_EQUAL_INT(-1, pulseRowForTarget("COLA"));   // case sensitive on purpose
  TEST_ASSERT_EQUAL_INT(-1, pulseRowForTarget(NULL));
}

/* ---- battery-switch strike / latch policy ---------------------------------- */

static const uint32_t DAY = 86400;
static const uint32_t T0  = 1790000000UL;   // an arbitrary 2026 timestamp

void test_bsw_two_strikes_in_a_day_latch(void) {
  uint8_t n = bswNextStrikes(0, 0, T0, DAY);
  TEST_ASSERT_EQUAL_UINT8(1, n);                              // first strike: tolerated
  n = bswNextStrikes(n, T0, T0 + 3600, DAY);
  TEST_ASSERT_EQUAL_UINT8(2, n);                              // second within 24 h: latch threshold
}

void test_bsw_strike_after_the_window_starts_over(void) {
  TEST_ASSERT_EQUAL_UINT8(1, bswNextStrikes(1, T0, T0 + DAY, DAY));        // exactly 24 h later
  TEST_ASSERT_EQUAL_UINT8(1, bswNextStrikes(1, T0, T0 + 3 * DAY, DAY));
}

void test_bsw_uncheckable_time_counts_towards_the_latch(void) {
  // A loop must not escape the latch because the RTC is dead or was set backwards.
  TEST_ASSERT_EQUAL_UINT8(2, bswNextStrikes(1, T0, 0, DAY));               // RTC dead now
  TEST_ASSERT_EQUAL_UINT8(2, bswNextStrikes(1, 0, T0, DAY));               // last strike time unknown
  TEST_ASSERT_EQUAL_UINT8(2, bswNextStrikes(1, T0, T0 - 5000, DAY));       // clock went backwards
  TEST_ASSERT_EQUAL_UINT8(255, bswNextStrikes(255, T0, T0 + 10, DAY));     // saturates, never wraps to 0
}

void test_bsw_auto_clear_only_after_a_full_known_window(void) {
  TEST_ASSERT_FALSE(bswAutoClearDue(T0, T0 + DAY - 1, DAY));
  TEST_ASSERT_TRUE(bswAutoClearDue(T0, T0 + DAY, DAY));
  TEST_ASSERT_FALSE(bswAutoClearDue(0, T0 + 2 * DAY, DAY));               // unknown strike time: manual only
  TEST_ASSERT_FALSE(bswAutoClearDue(T0, 0, DAY));                          // RTC dead: manual only
  TEST_ASSERT_FALSE(bswAutoClearDue(T0, T0 - 10, DAY));                    // clock went backwards
}

/* ---- INA226 scaling --------------------------------------------------------- */

void test_ina_cal_for_the_r010_module(void) {
  TEST_ASSERT_EQUAL_UINT16(2048, inaCalFor(0.010f, 0.00025f));
}

void test_ina_cal_for_the_50a_75mv_shunt_bar(void) {
  TEST_ASSERT_EQUAL_UINT16(1707, inaCalFor(0.075f / 50.0f, 0.002f));   // 1706.67 rounds up
}

void test_ina_cal_refuses_values_the_register_cannot_hold(void) {
  TEST_ASSERT_EQUAL_UINT16(0, inaCalFor(0.0f, 0.002f));
  TEST_ASSERT_EQUAL_UINT16(0, inaCalFor(0.0015f, 0.0f));
  TEST_ASSERT_EQUAL_UINT16(0, inaCalFor(-0.01f, 0.002f));
  TEST_ASSERT_EQUAL_UINT16(0, inaCalFor(0.00001f, 0.00001f));          // far above 32767
}

void test_ina_amps_reads_the_register_as_signed(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, inaAmps(25000, 0.002f, false, 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -20.0f, inaAmps((uint16_t)(int16_t)-10000, 0.002f, false, 0.0f));  // charging
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, inaAmps((uint16_t)(int16_t)-10000, 0.002f, true, 0.0f));    // leads swapped
}

void test_ina_amps_removes_the_offset_after_the_sign(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.986f, inaAmps(8000, 0.00025f, false, 0.014f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -2.014f, inaAmps(8000, 0.00025f, true, 0.014f));
}

void test_ina_bus_volts(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.6f, inaBusVolts(10080, 1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.726f, inaBusVolts(10080, 1.01f));
}

/* ---- backup state machine -------------------------------------------------- */

static BackupIn bkIn(void) {           // a calm rig: idle, pack fine, main neither low nor recovered
  BackupIn in = {};
  in.idle = true; in.secUsable = true;
  return in;
}

void test_backup_stays_idle_until_main_is_low_for_the_whole_trigger_time(void) {
  BackupIn in = bkIn();
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_IDLE, in));
  in.mainLowHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_ALERT, backupNext(BK_IDLE, in));
  in.idle = false;                                                        // a run is on: its sag doesn't count
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_IDLE, in));
}

void test_backup_alert_goes_out_before_the_boards_move(void) {
  BackupIn in = bkIn(); in.mainLowHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_ALERT, backupNext(BK_ALERT, in));
  in.alertDone = true;
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_ALERT, in));
  TEST_ASSERT_EQUAL_INT(BK_SWITCHING, backupNext(BK_WAIT, in));
}

void test_backup_never_switches_onto_an_unusable_pack_or_a_busy_rig(void) {
  BackupIn in = bkIn(); in.mainLowHeld = true;
  in.secUsable = false;
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_WAIT, in));
  in.secUsable = true; in.idle = false;
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_WAIT, in));
}

void test_backup_failed_switch_goes_back_and_waits(void) {
  BackupIn in = bkIn(); in.mainLowHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_SWITCHING, backupNext(BK_SWITCHING, in));
  in.timedOut = true;
  TEST_ASSERT_EQUAL_INT(BK_RETURNING, backupNext(BK_SWITCHING, in));
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_RETURNING, in));
}

void test_backup_active_returns_when_main_recovers(void) {
  BackupIn in = bkIn(); in.onSecondary = true;
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, backupNext(BK_SWITCHING, in));
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, backupNext(BK_ACTIVE, in));
  in.mainOkHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_RETURNING, backupNext(BK_ACTIVE, in));
  TEST_ASSERT_EQUAL_INT(BK_RETURNING, backupNext(BK_RETURNING, in));      // ESP2 hasn't confirmed yet
  in.onSecondary = false;
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_RETURNING, in));
}

void test_backup_protects_the_pack_even_if_main_is_still_critical(void) {
  BackupIn in = bkIn(); in.onSecondary = true; in.mainLowHeld = true; in.secExhausted = true;
  TEST_ASSERT_EQUAL_INT(BK_RETURNING, backupNext(BK_ACTIVE, in));
  in.onSecondary = false; in.secUsable = false;
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_RETURNING, in));
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_WAIT, in));                // and doesn't go straight back on it
}

void test_backup_losing_the_secondary_re_requests_it(void) {
  BackupIn in = bkIn(); in.mainLowHeld = true;                            // a fault or E-stop dropped P0
  TEST_ASSERT_EQUAL_INT(BK_WAIT, backupNext(BK_ACTIVE, in));
  TEST_ASSERT_EQUAL_INT(BK_SWITCHING, backupNext(BK_WAIT, in));
}

void test_backup_return_timeout_cannot_strand_the_state(void) {
  BackupIn in = bkIn(); in.onSecondary = true; in.timedOut = true; in.mainOkHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_RETURNING, in));
}

void test_backup_main_recovering_before_the_switch_cancels_it(void) {
  BackupIn in = bkIn(); in.mainOkHeld = true;
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_ALERT, in));
  TEST_ASSERT_EQUAL_INT(BK_IDLE, backupNext(BK_WAIT, in));
  TEST_ASSERT_EQUAL_INT(BK_RETURNING, backupNext(BK_SWITCHING, in));
}

/* ---- testRowUsesInverter (secondary-battery gate) ------------------------- */

void test_inverter_rows_are_exactly_the_ac_rows(void) {
  // The bare inverter relay and every Fill/Push combo raise the inverter -> secondary battery.
  TEST_ASSERT_TRUE(testRowUsesInverter(2));
  for (int r = 16; r <= 19; r++) TEST_ASSERT_TRUE(testRowUsesInverter(r));
  // The battery select (P0), spares P1/P5/P7, column valves (P3/P4/P6), the DC
  // mixer relay, the two servo-only rows (20/21: the selector moves with the inverter OFF), the mixer combo
  // (22 -- the mixer is 12 V DC), DC pumps, P17: no inverter.
  int none[] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 21, 22, 23, -1 };
  for (unsigned k = 0; k < sizeof(none) / sizeof(none[0]); k++) TEST_ASSERT_FALSE(testRowUsesInverter(none[k]));
}

void test_pulse_targets_that_need_the_inverter(void) {
  // A dashboard pulse is blocked on a low secondary only when its row raises the inverter.
  TEST_ASSERT_TRUE(testRowUsesInverter(pulseRowForTarget("transfer")));
  TEST_ASSERT_TRUE(testRowUsesInverter(pulseRowForTarget("colA")));
  TEST_ASSERT_FALSE(testRowUsesInverter(pulseRowForTarget("nutA")));
  TEST_ASSERT_FALSE(testRowUsesInverter(pulseRowForTarget("phUp")));
  TEST_ASSERT_FALSE(testRowUsesInverter(pulseRowForTarget("mixer")));   // 12 V DC mixer
  TEST_ASSERT_FALSE(testRowUsesInverter(pulseRowForTarget("servoL")));  // servo test: inverter stays off
  TEST_ASSERT_FALSE(testRowUsesInverter(pulseRowForTarget("servoR")));
}

/* ---- acVoltWindowOk (AC supply window sent to ESP2) ------------------------ */

void test_ac_volt_window_accepts_the_defaults_and_the_edges(void) {
  TEST_ASSERT_TRUE(acVoltWindowOk(AC_V_DEFAULT_LOW, AC_V_DEFAULT_HIGH));
  TEST_ASSERT_TRUE(acVoltWindowOk(80, 85));            // lowest window the PZEM can measure
  TEST_ASSERT_TRUE(acVoltWindowOk(255, 260));          // highest
  TEST_ASSERT_TRUE(acVoltWindowOk(215, 220));          // exactly the minimum gap
}

void test_ac_volt_window_refuses_out_of_range_narrow_and_absent(void) {
  TEST_ASSERT_FALSE(acVoltWindowOk(79, 240));          // below the PZEM's range
  TEST_ASSERT_FALSE(acVoltWindowOk(215, 261));         // above it
  TEST_ASSERT_FALSE(acVoltWindowOk(215, 219));         // gap of 4 V
  TEST_ASSERT_FALSE(acVoltWindowOk(240, 215));         // low and high swapped
  TEST_ASSERT_FALSE(acVoltWindowOk(220, 220));         // no window at all
  TEST_ASSERT_FALSE(acVoltWindowOk(-1, -1));           // the "field absent" sentinel of a web command
  TEST_ASSERT_FALSE(acVoltWindowOk(0, 0));
}

/* ---- acAmpWindowOk (AC pump current limits sent to ESP2, tenths of an amp) ------- */

void test_ac_amp_window_accepts_the_defaults_and_the_edges(void) {
  TEST_ASSERT_TRUE(acAmpWindowOk(AC_A_DEFAULT_MIN, AC_A_DEFAULT_MAX));   // 0.5 / 3.0 A
  TEST_ASSERT_TRUE(acAmpWindowOk(1, 3));               // 0.1 / 0.3 A: lowest pair
  TEST_ASSERT_TRUE(acAmpWindowOk(98, 100));            // 9.8 / 10.0 A: highest pair
  TEST_ASSERT_TRUE(acAmpWindowOk(5, 7));               // exactly the 0.2 A gap
}

void test_ac_amp_window_refuses_out_of_range_narrow_and_absent(void) {
  TEST_ASSERT_FALSE(acAmpWindowOk(0, 30));             // min 0 would switch the dry-run check off
  TEST_ASSERT_FALSE(acAmpWindowOk(5, 101));            // above the 10 A PZEM's range
  TEST_ASSERT_FALSE(acAmpWindowOk(5, 6));              // gap of 0.1 A
  TEST_ASSERT_FALSE(acAmpWindowOk(30, 5));             // min and max swapped
  TEST_ASSERT_FALSE(acAmpWindowOk(-1, -1));
}

/* ---- calibration values read back from NVS --------------------------------- */

void test_cal_in_range_refuses_nan(void) {
  TEST_ASSERT_TRUE(calInRange(50.0f, 1.0f, 5000.0f));
  TEST_ASSERT_FALSE(calInRange(0.0f, 1.0f, 5000.0f));
  TEST_ASSERT_FALSE(calInRange(NAN, 1.0f, 5000.0f));
}

void test_ina_trim_accepts_real_results_for_both_shunts(void) {
  TEST_ASSERT_TRUE(inaTrimOk(0.010f, 0.010f, 0.00025f, 0.014f, 1.000f));       // R010, the secondary's real offset
  TEST_ASSERT_TRUE(inaTrimOk(0.00152f, 0.0015f, 0.002f, -0.3f, 1.004f));       // the bar, trimmed
  TEST_ASSERT_TRUE(inaTrimOk(0.0013f, 0.0015f, 0.002f, 0.0f, 1.0f));           // 13 % low: R010 left on, still a trim
}

void test_ina_trim_refuses_each_kind_of_bad_value(void) {
  TEST_ASSERT_FALSE(inaTrimOk(0.0f, 0.010f, 0.00025f, 0.0f, 1.0f));            // no shunt
  TEST_ASSERT_FALSE(inaTrimOk(0.021f, 0.010f, 0.00025f, 0.0f, 1.0f));          // over x2: the wrong module's value
  TEST_ASSERT_FALSE(inaTrimOk(0.004f, 0.010f, 0.00025f, 0.0f, 1.0f));          // under x0.5
  TEST_ASSERT_FALSE(inaTrimOk(0.010f, 0.010f, 0.00025f, 0.2f, 1.0f));          // over 2 % of 8.19 A: real current
  TEST_ASSERT_FALSE(inaTrimOk(0.010f, 0.010f, 0.00025f, -0.2f, 1.0f));
  TEST_ASSERT_FALSE(inaTrimOk(0.010f, 0.010f, 0.00025f, 0.0f, 1.2f));          // bus factor off by 20 %
  TEST_ASSERT_FALSE(inaTrimOk(NAN, 0.010f, 0.00025f, 0.0f, 1.0f));
  TEST_ASSERT_FALSE(inaTrimOk(0.010f, 0.010f, 0.00025f, NAN, 1.0f));
  TEST_ASSERT_FALSE(inaTrimOk(0.010f, 0.010f, 0.00025f, 0.0f, NAN));
}

/* ---- battFlagHeld ----------------------------------------------------------- */

void test_batt_flag_ignores_a_short_sag(void) {
  uint32_t since = 0;
  TEST_ASSERT_FALSE(battFlagHeld(true, 1000, &since, 60000));
  TEST_ASSERT_FALSE(battFlagHeld(true, 60999, &since, 60000));    // one ms short
  TEST_ASSERT_FALSE(battFlagHeld(false, 61000, &since, 60000));   // recovered: the clock restarts
  TEST_ASSERT_EQUAL_UINT32(0, since);
  TEST_ASSERT_FALSE(battFlagHeld(true, 62000, &since, 60000));
  TEST_ASSERT_FALSE(battFlagHeld(true, 121999, &since, 60000));
}

void test_batt_flag_sets_after_the_hold_and_clears_at_once(void) {
  uint32_t since = 0;
  TEST_ASSERT_FALSE(battFlagHeld(true, 5000, &since, 60000));
  TEST_ASSERT_TRUE(battFlagHeld(true, 65000, &since, 60000));
  TEST_ASSERT_TRUE(battFlagHeld(true, 70000, &since, 60000));
  TEST_ASSERT_FALSE(battFlagHeld(false, 70001, &since, 60000));
}

void test_batt_flag_survives_millis_zero_and_rollover(void) {
  uint32_t since = 0;
  TEST_ASSERT_FALSE(battFlagHeld(true, 0, &since, 60000));        // starting at 0 must still start the clock
  TEST_ASSERT_EQUAL_UINT32(1, since);
  since = 0xFFFFFFF0UL;                                           // started just before millis() wrapped
  TEST_ASSERT_FALSE(battFlagHeld(true, 100, &since, 60000));
  TEST_ASSERT_TRUE(battFlagHeld(true, 60000, &since, 60000));
}

/* ---- forceBatchOk ----------------------------------------------------------- */

void test_force_batch_needs_the_floor_only_when_dosing(void) {
  TEST_ASSERT_TRUE(forceBatchOk(2.0f, 20.0f, 5.0f, 0, 0, 0));     // water only: no floor
  TEST_ASSERT_FALSE(forceBatchOk(2.0f, 20.0f, 5.0f, 25, 0, 0));   // the LCD editor's default volume, with a dose
  TEST_ASSERT_FALSE(forceBatchOk(6.0f, 20.0f, 5.0f, 0, 25, 0));   // 4.8 L batch
  TEST_ASSERT_TRUE(forceBatchOk(6.25f, 20.0f, 5.0f, 0, 0, 25));   // exactly the 5 L floor
}

/* ---- clampi -------------------------------------------------------------- */

void test_clampi(void) {
  TEST_ASSERT_EQUAL_INT(5,  clampi(5, 0, 10));
  TEST_ASSERT_EQUAL_INT(0,  clampi(-3, 0, 10));
  TEST_ASSERT_EQUAL_INT(10, clampi(99, 0, 10));
  TEST_ASSERT_EQUAL_INT(0,  clampi(0, 0, 10));
  TEST_ASSERT_EQUAL_INT(10, clampi(10, 0, 10));
}

/* ---- daysInMonth / isLeapYear --------------------------------------------- */

void test_days_in_month_ordinary_year(void) {
  TEST_ASSERT_EQUAL_INT(31, daysInMonth(2025, 1));
  TEST_ASSERT_EQUAL_INT(28, daysInMonth(2025, 2));   // not a leap year
  TEST_ASSERT_EQUAL_INT(30, daysInMonth(2025, 4));
  TEST_ASSERT_EQUAL_INT(31, daysInMonth(2025, 12));
}

void test_days_in_month_leap_year_rules(void) {
  // REGRESSION target: SET_CLOCK previously validated day-of-month as a flat 1..31 on every path
  // (LCD editor + Firebase command), so "Feb 30" or "Feb 29" in a non-leap year could be set.
  TEST_ASSERT_EQUAL_INT(29, daysInMonth(2024, 2));   // divisible by 4 -> leap
  TEST_ASSERT_EQUAL_INT(28, daysInMonth(2100, 2));   // divisible by 100 but not 400 -> not leap
  TEST_ASSERT_EQUAL_INT(29, daysInMonth(2000, 2));   // divisible by 400 -> leap
}

void test_is_leap_year(void) {
  TEST_ASSERT_TRUE(isLeapYear(2024));
  TEST_ASSERT_FALSE(isLeapYear(2025));
  TEST_ASSERT_FALSE(isLeapYear(2100));
  TEST_ASSERT_TRUE(isLeapYear(2000));
}

/* ---- policy invariant ---------------------------------------------------- */

void test_npk_min_moisture_stays_below_irrigation_start(void) {
  // NPK_MIN_MOIST_PCT must stay BELOW the irrigation start threshold (35 %). If it were raised to
  // or above it, fertigation could never be decided -- the soil is only ever read as dry-ish at
  // that point, so the gate would reject every run permanently.
  TEST_ASSERT_TRUE_MESSAGE(NPK_MIN_MOIST_PCT < 35,
                           "NPK_MIN_MOIST_PCT must stay below soilStartPct (35) or fertigation never runs");
}

void test_reservoir_low_uses_the_reservoir_reading_only(void) {
  TEST_ASSERT_TRUE(reservoirLow(true, 10.0f, 15.0f));       // low
  TEST_ASSERT_TRUE(reservoirLow(true, 0.0f, 15.0f));        // empty is a real reading
  TEST_ASSERT_FALSE(reservoirLow(true, 15.0f, 15.0f));      // exactly at the limit is allowed
  TEST_ASSERT_FALSE(reservoirLow(true, 80.0f, 15.0f));
  TEST_ASSERT_FALSE(reservoirLow(true, -1.0f, 15.0f));      // no reading is unknown, not low
  TEST_ASSERT_FALSE(reservoirLow(false, 0.0f, 15.0f));      // before the first TANK packet
}

// 18 Nov 2026: the reservoir pause can be raised from the filter zone (18 %) to 50 %, never lowered under it.
void test_reservoir_pause_only_goes_up_from_the_filter_zone(void) {
  TEST_ASSERT_TRUE(resPauseOk(18, 18));                      // the filter zone itself (the old fixed value)
  TEST_ASSERT_TRUE(resPauseOk(30, 18));
  TEST_ASSERT_TRUE(resPauseOk(RES_PAUSE_MAX_PCT, 18));
  TEST_ASSERT_FALSE(resPauseOk(17, 18));                     // under the filter: the pumps could run dry
  TEST_ASSERT_FALSE(resPauseOk(0, 18));
  TEST_ASSERT_FALSE(resPauseOk(RES_PAUSE_MAX_PCT + 1, 18));
  TEST_ASSERT_FALSE(reservoirLow(true, 30.0f, 30.0f));       // at a raised pause: still allowed
  TEST_ASSERT_TRUE(reservoirLow(true, 29.0f, 30.0f));        // under it: new runs wait
}

// 18 Nov 2026: a column's own Soil start / Too wet, -1 = the default; Too wet stays at least 5 above soil start.
void test_column_thresholds_follow_the_default_or_their_own(void) {
  TEST_ASSERT_EQUAL_INT(35, effPct(-1, 35));                 // no own value: the default
  TEST_ASSERT_EQUAL_INT(40, effPct(40, 35));
  TEST_ASSERT_EQUAL_INT(0, effPct(0, 35));                   // 0 is a real (if odd) own value, not "absent"
  TEST_ASSERT_TRUE(wetAboveStart(35, 90));
  TEST_ASSERT_TRUE(wetAboveStart(35, 40));                   // exactly the gap
  TEST_ASSERT_FALSE(wetAboveStart(35, 39));
  TEST_ASSERT_FALSE(wetAboveStart(35, 35));
  TEST_ASSERT_FALSE(wetAboveStart(35, 101));
  TEST_ASSERT_EQUAL_INT(90, wetEffPct(90, 35));
  TEST_ASSERT_EQUAL_INT(65, wetEffPct(60, 60));              // a default start raised past it: pushed up, not crossed
  TEST_ASSERT_EQUAL_INT(100, wetEffPct(90, 98));             // never over 100
  TEST_ASSERT_TRUE(SOIL_WET_DEFAULT_PCT >= 35 + THRESH_MIN_GAP_PCT);   // the defaults agree with each other
}

void test_tank_check_needs_a_live_run_and_no_switch_reset(void) {
  TEST_ASSERT_TRUE(runLossNeedsTankCheck(true, false));
  TEST_ASSERT_FALSE(runLossNeedsTankCheck(true, true));     // a switch reset re-runs on its own (locked behaviour)
  TEST_ASSERT_FALSE(runLossNeedsTankCheck(false, false));   // nothing was running
  TEST_ASSERT_FALSE(runLossNeedsTankCheck(false, true));
}

void test_crc8_matches_vectors_worked_out_separately(void) {
  TEST_ASSERT_EQUAL_HEX8(0xF4, crc8Text("123456789", 9));    // the standard check value for this CRC
  TEST_ASSERT_EQUAL_HEX8(0xC0, crc8Text("ENV,28.1,65.0", 13));
  TEST_ASSERT_EQUAL_HEX8(0xA0, crc8Text("TANK,41.2,18.7,0.00", 19));
  TEST_ASSERT_EQUAL_HEX8(0xCC, crc8Text("STATUS,NANO,OK", 14));
  TEST_ASSERT_EQUAL_HEX8(0x00, crc8Text("", 0));
}

static bool frameOk(const char *s, size_t *body) { return frameCheckCrc(s, strlen(s), body); }

void test_frame_check_accepts_a_good_frame_and_gives_the_body(void) {
  size_t body = 0;
  TEST_ASSERT_TRUE(frameOk("ENV,28.1,65.0,*C0", &body));
  TEST_ASSERT_EQUAL_UINT(13, body);
  TEST_ASSERT_TRUE(frameOk("STATUS,NANO,OK,*CC", &body));
  TEST_ASSERT_EQUAL_UINT(14, body);
}

void test_frame_check_rejects_damage_and_old_frames(void) {
  size_t body = 99;
  TEST_ASSERT_FALSE(frameOk("ENV,28.1,65.1,*C0", &body));    // one flipped digit, still a plausible number
  TEST_ASSERT_FALSE(frameOk("ENV,28.1,65.0,*C7", &body));    // damaged checksum
  TEST_ASSERT_FALSE(frameOk("ENV,28.1,65.0,*c0", &body));    // always uppercase on the wire
  TEST_ASSERT_FALSE(frameOk("ENV,28.1,65.0,*G0", &body));    // not hex
  TEST_ASSERT_FALSE(frameOk("ENV,28.1,65.0", &body));        // old firmware: no checksum at all
  TEST_ASSERT_FALSE(frameOk(",*00", &body));                 // nothing to check
  TEST_ASSERT_FALSE(frameOk("", &body));
  TEST_ASSERT_FALSE(frameCheckCrc(NULL, 0, &body));
  TEST_ASSERT_EQUAL_UINT(99, body);                          // a rejected frame leaves it alone
}

/* ---- outgoing Nano command checksum + pump-selector servo ------------------- */

void test_crc_field_round_trips_through_the_frame_check(void) {
  // What ESP1 appends to a SERVO command must be exactly what the Nano (same CRC, same layout) accepts.
  const char *cmds[] = { "SERVO,LEFT,7", "SERVO,RIGHT,65535", "FAST", "ACTIVE" };
  for (unsigned k = 0; k < sizeof(cmds) / sizeof(cmds[0]); k++) {
    char f[5]; crcFieldFor(cmds[k], strlen(cmds[k]), f);
    char line[40]; snprintf(line, sizeof(line), "%s%s", cmds[k], f);
    size_t body = 0;
    TEST_ASSERT_TRUE(frameOk(line, &body));
    TEST_ASSERT_EQUAL_UINT(strlen(cmds[k]), body);
  }
  char f[5]; crcFieldFor("ENV,28.1,65.0", 13, f);
  TEST_ASSERT_EQUAL_STRING(",*C0", f);                         // the worked vector above, uppercase
}

void test_servo_side_for_each_pump(void) {
  TEST_ASSERT_EQUAL_STRING("LEFT",  servoSideForPump("TRANSFER"));   // sketch_nov15a: LEFT = transfer
  TEST_ASSERT_EQUAL_STRING("RIGHT", servoSideForPump("BOOSTER"));
  TEST_ASSERT_NULL(servoSideForPump("MIXER"));                       // DC, never on the selector
  TEST_ASSERT_NULL(servoSideForPump("transfer"));                    // the wire token is uppercase
  TEST_ASSERT_NULL(servoSideForPump(NULL));
}

/* ---- the measured tank geometry (2026-11-15: both tanks empty 42.9 cm, full 24.0 cm) ------------- */

void test_level_pct_with_the_measured_tank_geometry(void) {
  const float E = 42.9f, F = 24.0f;                                 // ESP1 calRes*/calMix* defaults
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(42.9f, E, F));   // empty
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, levelPct(24.0f, E, F));   // full
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f,  levelPct(33.45f, E, F));  // half-way
  // Where the mixing-tank stops trip: ESP2 stops a fill at 90 %, ESP1's backstop is 97 %.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 89.0f,  levelPct(26.0f, E, F));   // 26.0 cm: not yet
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f,  levelPct(25.8f, E, F));   // ~25.9 cm: the 90 % stop
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 97.0f,  levelPct(24.57f, E, F));  // ~24.6 cm: the 97 % backstop
  // Past full keeps counting (user example: 120 % = 3.78 cm above the full mark); a deep echo is 0.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, levelPct(20.22f, E, F));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 121.0f, levelPct(20.0f, E, F));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(48.0f, E, F));
}

// LINK PACE (18 Nov 2026 e): busy while a window is open, and from preMin before it opens; not after it, nor
// for tomorrow's window late at night.
static void test_window_open_or_soon() {
  TEST_ASSERT_TRUE(windowOpenOrSoon(360, 360, 480, 10));    // 06:00, window 06:00-08:00: open (inclusive start)
  TEST_ASSERT_TRUE(windowOpenOrSoon(480, 360, 480, 10));    // 08:00: still open (inclusive end)
  TEST_ASSERT_FALSE(windowOpenOrSoon(481, 360, 480, 10));   // 08:01: over
  TEST_ASSERT_TRUE(windowOpenOrSoon(350, 360, 480, 10));    // 05:50: opens in 10 min
  TEST_ASSERT_FALSE(windowOpenOrSoon(349, 360, 480, 10));   // 05:49: 11 min away
  TEST_ASSERT_FALSE(windowOpenOrSoon(1435, 0, 60, 10));     // 23:55, tomorrow's 00:00 window: not counted
  TEST_ASSERT_FALSE(windowOpenOrSoon(600, 360, 480, 0));    // preMin 0: only while open
  TEST_ASSERT_TRUE(windowOpenOrSoon(360, 360, 480, 0));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_imap_matches_arduino_map);
  RUN_TEST(test_imap_degenerate_span_does_not_divide_by_zero);
  RUN_TEST(test_rtc_rejects_the_bad_dates_seen_on_the_rig);
  RUN_TEST(test_rtc_accepts_real_dates_and_checks_each_field);
  RUN_TEST(test_rtc_jump_works_in_both_directions);
  RUN_TEST(test_soil_cal_needs_dry_above_wet_by_a_margin);
  RUN_TEST(test_soil_pct_endpoints);
  RUN_TEST(test_soil_pct_clamps_outside_the_span);
  RUN_TEST(test_soil_pct_survives_inverted_endpoints);
  RUN_TEST(test_soil_pct_equal_endpoints_is_zero_not_a_crash);
  RUN_TEST(test_combine_capacitive_only_when_no_npk);
  RUN_TEST(test_combine_blends_npk_when_it_agrees);
  RUN_TEST(test_combine_excludes_npk_when_it_diverges);
  RUN_TEST(test_combine_agreement_boundary_is_inclusive);
  RUN_TEST(test_combine_divergence_is_symmetric);
  RUN_TEST(test_combine_drops_a_railed_probe);
  RUN_TEST(test_combine_rail_boundaries_are_exclusive);
  RUN_TEST(test_combine_falls_back_to_npk_when_both_probes_are_dead);
  RUN_TEST(test_combine_reports_none_when_nothing_is_usable);
  RUN_TEST(test_combine_tolerates_a_null_why_pointer);
  RUN_TEST(test_probes_each_use_their_own_endpoints);
  RUN_TEST(test_probes_with_shared_endpoints_match_the_column_cal);
  RUN_TEST(test_probes_railed_one_uses_the_other_on_its_own_cal);
  RUN_TEST(test_probes_both_dead_fall_back_or_none);
  RUN_TEST(test_probes_npk_blend_unchanged);
  RUN_TEST(test_soil_cal_steady_window);
  RUN_TEST(test_level_pct_endpoints_and_midpoint);
  RUN_TEST(test_level_pct_floor_at_zero_and_past_full);
  RUN_TEST(test_level_pct_degenerate_geometry_returns_zero);
  RUN_TEST(test_level_pct_returns_whole_percents);
  RUN_TEST(test_pulse_row_maps_every_known_target);
  RUN_TEST(test_pulse_row_rejects_unknown_and_null);
  RUN_TEST(test_bsw_two_strikes_in_a_day_latch);
  RUN_TEST(test_bsw_strike_after_the_window_starts_over);
  RUN_TEST(test_bsw_uncheckable_time_counts_towards_the_latch);
  RUN_TEST(test_bsw_auto_clear_only_after_a_full_known_window);
  RUN_TEST(test_ina_cal_for_the_r010_module);
  RUN_TEST(test_ina_cal_for_the_50a_75mv_shunt_bar);
  RUN_TEST(test_ina_cal_refuses_values_the_register_cannot_hold);
  RUN_TEST(test_ina_amps_reads_the_register_as_signed);
  RUN_TEST(test_ina_amps_removes_the_offset_after_the_sign);
  RUN_TEST(test_ina_bus_volts);
  RUN_TEST(test_backup_stays_idle_until_main_is_low_for_the_whole_trigger_time);
  RUN_TEST(test_backup_alert_goes_out_before_the_boards_move);
  RUN_TEST(test_backup_never_switches_onto_an_unusable_pack_or_a_busy_rig);
  RUN_TEST(test_backup_failed_switch_goes_back_and_waits);
  RUN_TEST(test_backup_active_returns_when_main_recovers);
  RUN_TEST(test_backup_protects_the_pack_even_if_main_is_still_critical);
  RUN_TEST(test_backup_losing_the_secondary_re_requests_it);
  RUN_TEST(test_backup_return_timeout_cannot_strand_the_state);
  RUN_TEST(test_backup_main_recovering_before_the_switch_cancels_it);
  RUN_TEST(test_inverter_rows_are_exactly_the_ac_rows);
  RUN_TEST(test_pulse_targets_that_need_the_inverter);
  RUN_TEST(test_ac_volt_window_accepts_the_defaults_and_the_edges);
  RUN_TEST(test_ac_volt_window_refuses_out_of_range_narrow_and_absent);
  RUN_TEST(test_ac_amp_window_accepts_the_defaults_and_the_edges);
  RUN_TEST(test_ac_amp_window_refuses_out_of_range_narrow_and_absent);
  RUN_TEST(test_cal_in_range_refuses_nan);
  RUN_TEST(test_ina_trim_accepts_real_results_for_both_shunts);
  RUN_TEST(test_ina_trim_refuses_each_kind_of_bad_value);
  RUN_TEST(test_batt_flag_ignores_a_short_sag);
  RUN_TEST(test_batt_flag_sets_after_the_hold_and_clears_at_once);
  RUN_TEST(test_batt_flag_survives_millis_zero_and_rollover);
  RUN_TEST(test_force_batch_needs_the_floor_only_when_dosing);
  RUN_TEST(test_clampi);
  RUN_TEST(test_days_in_month_ordinary_year);
  RUN_TEST(test_days_in_month_leap_year_rules);
  RUN_TEST(test_is_leap_year);
  RUN_TEST(test_npk_min_moisture_stays_below_irrigation_start);
  RUN_TEST(test_reservoir_low_uses_the_reservoir_reading_only);
  RUN_TEST(test_reservoir_pause_only_goes_up_from_the_filter_zone);
  RUN_TEST(test_column_thresholds_follow_the_default_or_their_own);
  RUN_TEST(test_tank_check_needs_a_live_run_and_no_switch_reset);
  RUN_TEST(test_crc8_matches_vectors_worked_out_separately);
  RUN_TEST(test_frame_check_accepts_a_good_frame_and_gives_the_body);
  RUN_TEST(test_frame_check_rejects_damage_and_old_frames);
  RUN_TEST(test_crc_field_round_trips_through_the_frame_check);
  RUN_TEST(test_servo_side_for_each_pump);
  RUN_TEST(test_level_pct_with_the_measured_tank_geometry);
  RUN_TEST(test_window_open_or_soon);
  return UNITY_END();
}
