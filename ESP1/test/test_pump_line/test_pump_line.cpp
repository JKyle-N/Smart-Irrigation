// Host tests for shared/pump_line.h (2026-11-17).
#include <unity.h>
#include "../../../shared/pump_line.h"

void setUp(void) {}
void tearDown(void) {}

// The user's own readings are the default lines.
void test_readings_give_the_default_lines(void) {
  PlFit x = plFitReadings(PL_XFER_READINGS, 10);
  TEST_ASSERT_TRUE(x.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 11.74f, x.rateLpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.47f, x.startL);
  PlFit b = plFitReadings(PL_BOOST_READINGS, 10);
  TEST_ASSERT_TRUE(b.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 13.20f, b.rateLpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.65f, b.startL);
}
void test_fewer_than_five_tries_give_nothing(void) {
  float t[4] = { 5, 10, 15, 20 }, v[4] = { 1, 2, 3, 4 };
  TEST_ASSERT_FALSE(plFit(t, v, 4).ok);
}
void test_same_seconds_give_no_rate(void) {
  float t[5] = { 10, 10, 10, 10.5f, 10 }, v[5] = { 2, 2.1f, 1.9f, 2.0f, 2.05f };
  TEST_ASSERT_FALSE(plFit(t, v, 5).ok);
}
void test_exact_line_is_found(void) {
  float t[5] = { 6, 10, 20, 30, 40 }, v[5];
  for (int i = 0; i < 5; i++) v[i] = 0.5f + 12.0f / 60.0f * t[i];
  PlFit f = plFit(t, v, 5);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, f.rateLpm);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, f.startL);
}
void test_water_that_does_not_grow_is_refused(void) {
  float t[5] = { 5, 10, 15, 20, 25 }, v[5] = { 3, 2.5f, 2, 1.5f, 1 };
  TEST_ASSERT_FALSE(plFit(t, v, 5).ok);
}
void test_litres_ramp_then_line(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, plLitresAt(0.0f, 11.74f, 0.47f));
  float at6 = 0.47f + 11.74f / 60.0f * 6.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, at6 / 2.0f, plLitresAt(3.0f, 11.74f, 0.47f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.47f + 11.74f / 60.0f * 20.0f, plLitresAt(20.0f, 11.74f, 0.47f));
  TEST_ASSERT_TRUE(plLitresAt(7.0f, 13.2f, -0.65f) > 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, plLitresAt(10.0f, 0.0f, 0.47f));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_readings_give_the_default_lines);
  RUN_TEST(test_fewer_than_five_tries_give_nothing);
  RUN_TEST(test_same_seconds_give_no_rate);
  RUN_TEST(test_exact_line_is_found);
  RUN_TEST(test_water_that_does_not_grow_is_refused);
  RUN_TEST(test_litres_ramp_then_line);
  return UNITY_END();
}
