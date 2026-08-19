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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ksane_accepts_the_working_band);
  RUN_TEST(test_ksane_rejects_out_of_band);
  RUN_TEST(test_ksane_rejects_non_finite);
  RUN_TEST(test_liters_normal_conversion);
  RUN_TEST(test_zero_k_reports_no_flow_not_infinity);
  RUN_TEST(test_corrupt_k_values_all_report_no_flow);
  RUN_TEST(test_large_pulse_counts_do_not_overflow);
  return UNITY_END();
}
