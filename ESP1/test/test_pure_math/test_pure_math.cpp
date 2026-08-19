/* Host-side tests for ESP1's pure sensor maths.  Run with:  pio test -e native
 *
 * These need no ESP32 and no rig. Cases lead with defects that were actually found in review, so
 * they stay found -- each of those is marked REGRESSION with a note on what went wrong.
 *
 * Calibration endpoints used throughout are column A's measured pair (dry 656 / wet 524 ADC).
 */
#include <unity.h>
#include "pure_math.h"

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

/* ---- levelPct ------------------------------------------------------------ */

void test_level_pct_endpoints_and_midpoint(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(38.0f, 38.0f, 11.0f));   // empty
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, levelPct(11.0f, 38.0f, 11.0f));   // full
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f,  levelPct(24.5f, 38.0f, 11.0f));
}

void test_level_pct_clamps_hard_at_both_ends(void) {
  // A reading past "full" reports exactly 100, never 101 -- an over-100 tank level would read as a
  // spurious overflow downstream.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, levelPct(5.0f,  38.0f, 11.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,   levelPct(50.0f, 38.0f, 11.0f));
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
  TEST_ASSERT_EQUAL_INT(8,  pulseRowForTarget("mixer"));
  TEST_ASSERT_EQUAL_INT(9,  pulseRowForTarget("nutA"));
  TEST_ASSERT_EQUAL_INT(10, pulseRowForTarget("nutB"));
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

/* ---- clampi -------------------------------------------------------------- */

void test_clampi(void) {
  TEST_ASSERT_EQUAL_INT(5,  clampi(5, 0, 10));
  TEST_ASSERT_EQUAL_INT(0,  clampi(-3, 0, 10));
  TEST_ASSERT_EQUAL_INT(10, clampi(99, 0, 10));
  TEST_ASSERT_EQUAL_INT(0,  clampi(0, 0, 10));
  TEST_ASSERT_EQUAL_INT(10, clampi(10, 0, 10));
}

/* ---- policy invariant ---------------------------------------------------- */

void test_npk_min_moisture_stays_below_irrigation_start(void) {
  // NPK_MIN_MOIST_PCT must stay BELOW the irrigation start threshold (35 %). If it were raised to
  // or above it, fertigation could never be decided -- the soil is only ever read as dry-ish at
  // that point, so the gate would reject every run permanently.
  TEST_ASSERT_TRUE_MESSAGE(NPK_MIN_MOIST_PCT < 35,
                           "NPK_MIN_MOIST_PCT must stay below soilStartPct (35) or fertigation never runs");
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_imap_matches_arduino_map);
  RUN_TEST(test_imap_degenerate_span_does_not_divide_by_zero);
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
  RUN_TEST(test_level_pct_endpoints_and_midpoint);
  RUN_TEST(test_level_pct_clamps_hard_at_both_ends);
  RUN_TEST(test_level_pct_degenerate_geometry_returns_zero);
  RUN_TEST(test_level_pct_returns_whole_percents);
  RUN_TEST(test_pulse_row_maps_every_known_target);
  RUN_TEST(test_pulse_row_rejects_unknown_and_null);
  RUN_TEST(test_clampi);
  RUN_TEST(test_npk_min_moisture_stays_below_irrigation_start);
  return UNITY_END();
}
