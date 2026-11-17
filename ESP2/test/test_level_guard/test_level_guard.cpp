/* Host-side tests for ESP2's mixing-tank level guard (include/level_guard.h).  Run with:  pio test -e native
 *
 * The guard turns ESP1's relayed LEVEL samples into two decisions while an AC pump runs:
 *   - overflow (LG_HIGH): only while FILLING, only after `need` valid readings in a row at/over the limit;
 *   - blindness: no valid reading for blindMs (from the start + grace before the first one).
 * A wrong answer either overflows the mixing tank or stops good runs, so both edges are pinned here.
 */
#include <unity.h>
#include "level_guard.h"

void setUp(void) {}
void tearDown(void) {}

static const float   HIGH = 90.0f;
static const uint8_t NEED = 3;
static const uint32_t GRACE = 3000, BLIND = 3000;

void test_high_needs_three_in_a_row_while_filling(void) {
  LevelGuard g; lgStart(g, true, 1000);
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 91.0f, 1300, HIGH, NEED));
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 92.0f, 1600, HIGH, NEED));
  TEST_ASSERT_EQUAL(LG_HIGH, lgSample(g, 90.0f, 1900, HIGH, NEED));   // the limit itself counts
  TEST_ASSERT_EQUAL_FLOAT(90.0f, g.lastPct);
}

void test_one_low_reading_resets_the_run(void) {
  LevelGuard g; lgStart(g, true, 0);
  lgSample(g, 95.0f, 100, HIGH, NEED);
  lgSample(g, 95.0f, 200, HIGH, NEED);
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 60.0f, 300, HIGH, NEED));      // a splash, not a full tank
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 95.0f, 400, HIGH, NEED));
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 95.0f, 500, HIGH, NEED));
  TEST_ASSERT_EQUAL(LG_HIGH, lgSample(g, 95.0f, 600, HIGH, NEED));
}

void test_invalid_readings_neither_count_nor_reset(void) {
  LevelGuard g; lgStart(g, true, 0);
  lgSample(g, 95.0f, 100, HIGH, NEED);
  lgSample(g, 95.0f, 200, HIGH, NEED);
  TEST_ASSERT_EQUAL(LG_OK, lgSample(g, -1.0f, 300, HIGH, NEED));      // lost echo: no vote either way
  TEST_ASSERT_EQUAL(2, g.highRun);
  TEST_ASSERT_EQUAL(200u, g.lastValidMs);                             // and it does not refresh the blind clock
  TEST_ASSERT_EQUAL(LG_HIGH, lgSample(g, 95.0f, 400, HIGH, NEED));
}

void test_draining_never_reports_high(void) {
  LevelGuard g; lgStart(g, false, 0);                                 // booster: tank can only go down
  for (uint32_t t = 100; t < 1000; t += 100) TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 99.0f, t, HIGH, NEED));
}

void test_inactive_guard_ignores_everything(void) {
  LevelGuard g; lgStart(g, true, 0); lgStop(g);
  for (uint32_t t = 100; t < 1000; t += 100) TEST_ASSERT_EQUAL(LG_OK, lgSample(g, 99.0f, t, HIGH, NEED));
  TEST_ASSERT_FALSE(lgBlind(g, 60000, GRACE, BLIND));
}

void test_blind_before_first_sample_waits_grace_plus_blind(void) {
  LevelGuard g; lgStart(g, true, 10000);
  TEST_ASSERT_FALSE(lgBlind(g, 10000 + GRACE + BLIND - 1, GRACE, BLIND));
  TEST_ASSERT_TRUE (lgBlind(g, 10000 + GRACE + BLIND,     GRACE, BLIND));
}

void test_blind_after_samples_stop(void) {
  LevelGuard g; lgStart(g, true, 0);
  lgSample(g, 40.0f, 2000, HIGH, NEED);
  TEST_ASSERT_FALSE(lgBlind(g, 2000 + BLIND - 1, GRACE, BLIND));
  TEST_ASSERT_TRUE (lgBlind(g, 2000 + BLIND,     GRACE, BLIND));
  lgSample(g, 41.0f, 5500, HIGH, NEED);                               // readings resume -> sighted again
  TEST_ASSERT_FALSE(lgBlind(g, 6000, GRACE, BLIND));
}

void test_only_invalid_samples_still_go_blind(void) {
  // An uncalibrated tank sends -1 every 300 ms: traffic, but no reading. That must count as blind.
  LevelGuard g; lgStart(g, true, 0);
  for (uint32_t t = 300; t <= 6000; t += 300) lgSample(g, -1.0f, t, HIGH, NEED);
  TEST_ASSERT_TRUE(lgBlind(g, 6000, GRACE, BLIND));
}

void test_clock_wrap_is_safe(void) {
  LevelGuard g; lgStart(g, true, 0xFFFFF000u);                        // ~4 s before millis() wraps
  lgSample(g, 50.0f, 0xFFFFFF00u, HIGH, NEED);
  TEST_ASSERT_FALSE(lgBlind(g, 0x00000100u, GRACE, BLIND));           // 512 ms later, across the wrap
  TEST_ASSERT_TRUE (lgBlind(g, 0x00000C00u, GRACE, BLIND));           // 3328 ms later
}

void test_sample_at_time_zero_is_still_a_sample(void) {
  LevelGuard g; lgStart(g, true, 0);
  lgSample(g, 50.0f, 0, HIGH, NEED);                                  // millis() == 0 must not mean "none yet"
  TEST_ASSERT_TRUE(g.lastValidMs != 0);
  TEST_ASSERT_FALSE(lgBlind(g, GRACE, GRACE, BLIND));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_high_needs_three_in_a_row_while_filling);
  RUN_TEST(test_one_low_reading_resets_the_run);
  RUN_TEST(test_invalid_readings_neither_count_nor_reset);
  RUN_TEST(test_draining_never_reports_high);
  RUN_TEST(test_inactive_guard_ignores_everything);
  RUN_TEST(test_blind_before_first_sample_waits_grace_plus_blind);
  RUN_TEST(test_blind_after_samples_stop);
  RUN_TEST(test_only_invalid_samples_still_go_blind);
  RUN_TEST(test_clock_wrap_is_safe);
  RUN_TEST(test_sample_at_time_zero_is_still_a_sample);
  return UNITY_END();
}
