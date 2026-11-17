/* Host-side tests for ESP2's source-tank low stop (include/source_low.h).  Run with:  pio test -e native
 *
 * On a transfer or a drain, 3 valid ultrasonic readings in a row under 15 % start a 10 s countdown; 3 in a row
 * at or over 15 % cancel it; the countdown running out stops the pump whatever the (noisy) flow sensor reads.
 * Too eager and it cuts good runs short; too lax and a pump runs dry on phantom pulses -- both edges pinned here.
 */
#include <unity.h>
#include "source_low.h"

void setUp(void) {}
void tearDown(void) {}

static const float    LOW_PCT = 15.0f;
static const uint8_t  NEED = 3;
static const uint32_t HOLD = 10000;

void test_three_low_readings_start_the_countdown(void) {
  SourceLow s; slReset(s);
  slSample(s, 14.0f, 1000, LOW_PCT, NEED);
  slSample(s, 13.0f, 1300, LOW_PCT, NEED);
  TEST_ASSERT_FALSE(s.counting);
  slSample(s, 12.0f, 1600, LOW_PCT, NEED);
  TEST_ASSERT_TRUE(s.counting);
  TEST_ASSERT_FALSE(slDue(s, 1600 + HOLD - 1, HOLD));
  TEST_ASSERT_TRUE(slDue(s, 1600 + HOLD, HOLD));
  TEST_ASSERT_EQUAL_FLOAT(12.0f, s.lastPct);
}

void test_the_limit_itself_is_not_low(void) {
  SourceLow s; slReset(s);
  for (int i = 0; i < 5; i++) slSample(s, 15.0f, 100 * i, LOW_PCT, NEED);
  TEST_ASSERT_FALSE(s.counting);
}

void test_one_high_reading_does_not_cancel_or_restart(void) {
  SourceLow s; slReset(s);
  for (int i = 0; i < 3; i++) slSample(s, 10.0f, 1000 + 300 * i, LOW_PCT, NEED);   // starts at 1600
  slSample(s, 40.0f, 2000, LOW_PCT, NEED);                                            // a splash
  slSample(s, 10.0f, 2300, LOW_PCT, NEED);
  TEST_ASSERT_TRUE(s.counting);
  TEST_ASSERT_TRUE(slDue(s, 1600 + HOLD, HOLD));                                      // still from 1600
}

void test_three_good_readings_cancel(void) {
  SourceLow s; slReset(s);
  for (int i = 0; i < 3; i++) slSample(s, 10.0f, 300 * i, LOW_PCT, NEED);
  for (int i = 0; i < 3; i++) slSample(s, 30.0f, 1000 + 300 * i, LOW_PCT, NEED);
  TEST_ASSERT_FALSE(s.counting);
  TEST_ASSERT_FALSE(slDue(s, 60000, HOLD));
}

void test_a_low_run_broken_by_a_good_reading_starts_over(void) {
  SourceLow s; slReset(s);
  slSample(s, 10.0f, 0, LOW_PCT, NEED);
  slSample(s, 10.0f, 300, LOW_PCT, NEED);
  slSample(s, 20.0f, 600, LOW_PCT, NEED);
  slSample(s, 10.0f, 900, LOW_PCT, NEED);
  slSample(s, 10.0f, 1200, LOW_PCT, NEED);
  TEST_ASSERT_FALSE(s.counting);
}

void test_invalid_readings_change_nothing(void) {
  SourceLow s; slReset(s);
  slSample(s, 10.0f, 0, LOW_PCT, NEED);
  slSample(s, -1.0f, 300, LOW_PCT, NEED);      // lost echo: no vote either way
  slSample(s, 10.0f, 600, LOW_PCT, NEED);
  slSample(s, 10.0f, 900, LOW_PCT, NEED);
  TEST_ASSERT_TRUE(s.counting);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, s.lastPct);
  SourceLow t; slReset(t);
  for (int i = 0; i < 10; i++) slSample(t, -1.0f, 100 * i, LOW_PCT, NEED);
  TEST_ASSERT_FALSE(t.counting);               // blind is never "low"
}

void test_reset_clears_everything(void) {
  SourceLow s; slReset(s);
  for (int i = 0; i < 3; i++) slSample(s, 5.0f, 300 * i, LOW_PCT, NEED);
  slReset(s);
  TEST_ASSERT_FALSE(s.counting);
  TEST_ASSERT_FALSE(slDue(s, 999999, HOLD));
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, s.lastPct);
}

void test_clock_wrap_is_safe(void) {
  SourceLow s; slReset(s);
  uint32_t t0 = 0xFFFFF000u;
  for (uint32_t i = 0; i < 3; i++) slSample(s, 5.0f, t0 + 300 * i, LOW_PCT, NEED);   // starts at t0 + 600
  TEST_ASSERT_FALSE(slDue(s, t0 + 600 + HOLD - 1, HOLD));
  TEST_ASSERT_TRUE(slDue(s, t0 + 600 + HOLD, HOLD));                                  // past the wrap
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_three_low_readings_start_the_countdown);
  RUN_TEST(test_the_limit_itself_is_not_low);
  RUN_TEST(test_one_high_reading_does_not_cancel_or_restart);
  RUN_TEST(test_three_good_readings_cancel);
  RUN_TEST(test_a_low_run_broken_by_a_good_reading_starts_over);
  RUN_TEST(test_invalid_readings_change_nothing);
  RUN_TEST(test_reset_clears_everything);
  RUN_TEST(test_clock_wrap_is_safe);
  return UNITY_END();
}
