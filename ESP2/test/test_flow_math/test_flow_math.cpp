/* Host-side tests for ESP2's flow maths.  Run with:  pio test -e native
 *
 * Small suite, but it covers the single worst failure mode in the rig: a corrupted flow K-factor.
 * K is a DIVISOR, and every metered stage decides when to stop its pump from the result, so a bad K
 * is not a measurement error -- it is an actuation fault.
 */
#include <unity.h>
#include <math.h>
#include "flow_math.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- kSane: the range guard --------------------------------------------- */

void test_ksane_accepts_the_working_band(void) {
  TEST_ASSERT_TRUE(kSane(450.0f));            // the bench value
  TEST_ASSERT_TRUE(kSane(K_FLOW_MIN));        // boundaries are inclusive
  TEST_ASSERT_TRUE(kSane(K_FLOW_MAX));
}

void test_ksane_rejects_out_of_band(void) {
  TEST_ASSERT_FALSE(kSane(K_FLOW_MIN - 0.5f));
  TEST_ASSERT_FALSE(kSane(K_FLOW_MAX + 1.0f));
  TEST_ASSERT_FALSE(kSane(0.0f));
  TEST_ASSERT_FALSE(kSane(-450.0f));
}

void test_ksane_rejects_non_finite(void) {
  // A bit-flip on the unchecksummed ESP1 link can land here as NaN or inf.
  TEST_ASSERT_FALSE(kSane(NAN));
  TEST_ASSERT_FALSE(kSane(INFINITY));
  TEST_ASSERT_FALSE(kSane(-INFINITY));
}

/* ---- pulsesToLiters ------------------------------------------------------ */

void test_liters_normal_conversion(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, pulsesToLiters(450, 450.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, pulsesToLiters(900, 450.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(0,   450.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, pulsesToLiters(225, 450.0f));
}

void test_zero_k_reports_no_flow_not_infinity(void) {
  // REGRESSION, and the important one. Dividing by K = 0 yields inf, which reads as "target
  // reached" on the very first pulse: the stage completes instantly, the run reports DONE, and no
  // water ever moved. Returning 0 instead makes it look like NO FLOW, which the stage already
  // handles safely (FLOW_TIMEOUT_MS -> FLOW_FAIL -> hold for the operator).
  float l = pulsesToLiters(900, 0.0f);
  TEST_ASSERT_TRUE_MESSAGE(isfinite(l), "a zero K must never produce inf");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, l);
}

void test_corrupt_k_values_all_report_no_flow(void) {
  // Every way K can arrive broken must land on the same safe answer.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(900, NAN));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(900, INFINITY));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(900, -450.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(900, K_FLOW_MAX + 1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pulsesToLiters(900, 0.001f));
}

void test_large_pulse_counts_do_not_overflow(void) {
  // The pulse counter is an unsigned long fed by an ISR and only cleared per stage; a stuck sensor
  // can run it high. The conversion must stay finite and monotonic.
  float a = pulsesToLiters(1000000UL, 450.0f);
  float b = pulsesToLiters(2000000UL, 450.0f);
  TEST_ASSERT_TRUE(isfinite(a) && isfinite(b));
  TEST_ASSERT_TRUE(b > a);
}

/* ---- Stage cap, timed credit, stall ---------------------------------------- */

// REGRESSION: a legal 20 L FORCE delivery needs 200 s at 6 L/min; the flat 180 s cap called it FLOW_FAIL.
void test_stage_cap_grows_with_the_batch(void) {
  TEST_ASSERT_EQUAL_UINT32(300000UL, stageCapMs(20.0f, 6.0f, 180000UL, 1.5f));
}

void test_stage_cap_keeps_the_base_for_small_batches(void) {
  TEST_ASSERT_EQUAL_UINT32(180000UL, stageCapMs(5.0f, 6.0f, 180000UL, 1.5f));
  TEST_ASSERT_EQUAL_UINT32(180000UL, stageCapMs(0.0f, 6.0f, 180000UL, 1.5f));
  TEST_ASSERT_EQUAL_UINT32(180000UL, stageCapMs(5.0f, 0.0f, 180000UL, 1.5f));
}

// REGRESSION: an interrupted flow-blind stage used to credit its whole target.
void test_timed_credit_is_proportional_when_interrupted(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, timedLiters(5.0f, 6.0f, 10000UL));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, timedLiters(5.0f, 6.0f, 0UL));
}

void test_timed_credit_stops_at_the_target(void) {
  // A completed stage runs TIMED_MARGIN long, so it still credits exactly the target.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, timedLiters(5.0f, 6.0f, 65000UL));
}

// REGRESSION: the 120 s timed cap on a 20 L delivery moves 12 L; the rest is still in the tank.
void test_timed_credit_reports_a_capped_stage_honestly(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, timedLiters(20.0f, 6.0f, 120000UL));
}

// REGRESSION: flow that started then stopped ran the pump dry until the hard cap.
void test_stall_needs_flow_first_and_an_ac_pump(void) {
  TEST_ASSERT_TRUE(flowStalled(true, true, 10001UL, 10000UL));
  TEST_ASSERT_FALSE(flowStalled(true, true, 9999UL, 10000UL));
  TEST_ASSERT_FALSE(flowStalled(false, true, 60000UL, 10000UL));   // never flowed: FLOW_TIMEOUT's job
  TEST_ASSERT_FALSE(flowStalled(true, false, 60000UL, 10000UL));   // dosing pumps pulse slowly
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ksane_accepts_the_working_band);
  RUN_TEST(test_ksane_rejects_out_of_band);
  RUN_TEST(test_ksane_rejects_non_finite);
  RUN_TEST(test_liters_normal_conversion);
  RUN_TEST(test_zero_k_reports_no_flow_not_infinity);
  RUN_TEST(test_corrupt_k_values_all_report_no_flow);
  RUN_TEST(test_large_pulse_counts_do_not_overflow);
  RUN_TEST(test_stage_cap_grows_with_the_batch);
  RUN_TEST(test_stage_cap_keeps_the_base_for_small_batches);
  RUN_TEST(test_timed_credit_is_proportional_when_interrupted);
  RUN_TEST(test_timed_credit_stops_at_the_target);
  RUN_TEST(test_timed_credit_reports_a_capped_stage_honestly);
  RUN_TEST(test_stall_needs_flow_first_and_an_ac_pump);
  return UNITY_END();
}
