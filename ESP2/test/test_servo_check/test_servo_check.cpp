/* Host-side tests for ESP2's stuck pump-selector check (include/servo_check.h).  Run with:  pio test -e native
 *
 * While the inverter powers one pump, the OTHER pump's flow sensor is watched. Flow there means the servo
 * left the wrong pump connected (stuck / slipped lever). Rule (user, 2026-11-15): inverter off, two full
 * servo swings, inverter on, check again -- retry twice, then hold. These pin the decision edges.
 */
#include <unity.h>
#include "servo_check.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t MIN = 20;
enum { T = 1, B = 2 };   // PUMP_TRANSFER / PUMP_BOOSTER as in main.cpp

void test_cross_flow_needs_the_minimum(void) {
  TEST_ASSERT_FALSE(crossFlowMismatch(19, 0, MIN));    // a few noise pulses never re-seat the servo
  TEST_ASSERT_TRUE (crossFlowMismatch(20, 0, MIN));    // the floor itself counts
  TEST_ASSERT_TRUE (crossFlowMismatch(500, 0, MIN));
}

void test_expected_flow_wins_a_tie_or_more(void) {
  // Both sensors moving (a siphon, a shared line): only call it the wrong pump when the other side clearly
  // outruns the one that should be flowing.
  TEST_ASSERT_FALSE(crossFlowMismatch(40, 40, MIN));
  TEST_ASSERT_FALSE(crossFlowMismatch(40, 300, MIN));
  TEST_ASSERT_TRUE (crossFlowMismatch(41, 40, MIN));
}

void test_unmetered_expected_side_counts_as_zero(void) {
  // Exercise / prime run without a meter on the expected pin: main.cpp passes 0.
  TEST_ASSERT_TRUE(crossFlowMismatch(25, 0, MIN));
}

void test_reseat_plan_is_two_full_swings_ending_on_target(void) {
  int m[4] = { 0, 0, 0, 0 };
  TEST_ASSERT_EQUAL_UINT8(4, servoReseatPlan(T, B, 2, m, 4));
  TEST_ASSERT_EQUAL_INT(B, m[0]); TEST_ASSERT_EQUAL_INT(T, m[1]);
  TEST_ASSERT_EQUAL_INT(B, m[2]); TEST_ASSERT_EQUAL_INT(T, m[3]);   // last move = the pump we want
  TEST_ASSERT_EQUAL_UINT8(4, servoReseatPlan(B, T, 2, m, 4));
  TEST_ASSERT_EQUAL_INT(T, m[0]); TEST_ASSERT_EQUAL_INT(B, m[3]);
}

void test_reseat_plan_never_overruns_the_buffer(void) {
  int m[4] = { 0, 0, 0, 0 };
  TEST_ASSERT_EQUAL_UINT8(4, servoReseatPlan(T, B, 5, m, 4));        // asks for 10 moves, room for 4
  TEST_ASSERT_EQUAL_UINT8(2, servoReseatPlan(T, B, 2, m, 3));        // never half a swing
  TEST_ASSERT_EQUAL_INT(T, m[1]);
}

void test_retry_retry_then_hold(void) {
  TEST_ASSERT_TRUE (reseatAllowed(1, 2));   // first detection: re-seat
  TEST_ASSERT_TRUE (reseatAllowed(2, 2));   // second: re-seat once more
  TEST_ASSERT_FALSE(reseatAllowed(3, 2));   // third: hold (SERVO_FAIL|STUCK)
  TEST_ASSERT_FALSE(reseatAllowed(0, 2));   // no detection, nothing to allow
}

// 2026-11-16: the inverter-on transient must not be read as the wrong pump running.
void test_transient_burst_is_not_confirmed(void) {
  // 30 edges in the first instant, then nothing: the mismatch holds, but the other sensor stopped counting.
  TEST_ASSERT_FALSE(crossFlowConfirmed(30, 30, 1500, 1000, 10));
  TEST_ASSERT_FALSE(crossFlowConfirmed(30, 25, 1500, 1000, 10));   // a few stray edges: still not water
}

void test_sustained_cross_flow_is_confirmed(void) {
  // The wrong pump: ~75 pulses/s at K 450 and 10 L/min -- grows by far more than the floor in 1 s.
  TEST_ASSERT_TRUE (crossFlowConfirmed(120, 45, 1000, 1000, 10));
  TEST_ASSERT_TRUE (crossFlowConfirmed(30, 20, 1000, 1000, 10));   // the growth floor itself counts
}

void test_confirmation_needs_the_full_window(void) {
  TEST_ASSERT_FALSE(crossFlowConfirmed(500, 20, 999, 1000, 10));   // plenty of pulses, not long enough
  TEST_ASSERT_TRUE (crossFlowConfirmed(500, 20, 1000, 1000, 10));
}

void test_counter_going_backwards_is_not_growth(void) {
  // A watch restarted under us (crossPulses re-zeroed): never let unsigned wrap read as a huge growth.
  TEST_ASSERT_FALSE(crossFlowConfirmed(5, 40, 2000, 1000, 10));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_transient_burst_is_not_confirmed);
  RUN_TEST(test_sustained_cross_flow_is_confirmed);
  RUN_TEST(test_confirmation_needs_the_full_window);
  RUN_TEST(test_counter_going_backwards_is_not_growth);
  RUN_TEST(test_cross_flow_needs_the_minimum);
  RUN_TEST(test_expected_flow_wins_a_tie_or_more);
  RUN_TEST(test_unmetered_expected_side_counts_as_zero);
  RUN_TEST(test_reseat_plan_is_two_full_swings_ending_on_target);
  RUN_TEST(test_reseat_plan_never_overruns_the_buffer);
  RUN_TEST(test_retry_retry_then_hold);
  return UNITY_END();
}
