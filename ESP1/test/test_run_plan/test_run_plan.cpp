// Host tests for include/run_plan.h: a column's Operation (Automatic / Manual) and its day plan (2026-11-18).
#include <unity.h>
#include "run_plan.h"

void setUp(void) {}
void tearDown(void) {}

void test_day_numbers_round_trip(void) {
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(19700101L));                 // 1970 is outside the 2000-2099 window
  TEST_ASSERT_EQUAL_INT32(10957, planDaysFromCivil(2000, 1, 1));         // known value
  const long ymds[] = { 20000101L, 20000229L, 20241231L, 20250101L, 20261118L, 20261130L, 20261201L,
                        20261231L, 20270101L, 20280229L, 20990101L, 20991231L };
  for (unsigned i = 0; i < sizeof(ymds) / sizeof(ymds[0]); i++) {
    long d = planDayFromYmd(ymds[i]);
    TEST_ASSERT_TRUE(d > 0);
    TEST_ASSERT_EQUAL_INT32(ymds[i], planYmdFromDay(d));
  }
  // consecutive across a month end, a year end and a leap day
  TEST_ASSERT_EQUAL_INT32(1, planDayFromYmd(20261201L) - planDayFromYmd(20261130L));
  TEST_ASSERT_EQUAL_INT32(1, planDayFromYmd(20270101L) - planDayFromYmd(20261231L));
  TEST_ASSERT_EQUAL_INT32(2, planDayFromYmd(20280301L) - planDayFromYmd(20280228L));
}

void test_invalid_dates_are_refused(void) {
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(20261131L));   // 31 Nov
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(20270229L));   // not a leap year
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(21000229L));   // 2100 is not a leap year, and out of range anyway
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(20261300L));
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(-1L));
  TEST_ASSERT_EQUAL_INT32(-1, planDayFromYmd(0L));
  TEST_ASSERT_TRUE(planDayFromYmd(20240229L) > 0);          // a real leap day
}

void test_text_validation(void) {
  TEST_ASSERT_TRUE(planTextValid(""));
  TEST_ASSERT_TRUE(planTextValid("--M-----A"));
  TEST_ASSERT_FALSE(planTextValid("--m"));                   // lower case is not a mark
  TEST_ASSERT_FALSE(planTextValid("M A"));
  TEST_ASSERT_FALSE(planTextValid(nullptr));
  char full[RUN_PLAN_DAYS + 2];
  memset(full, 'M', sizeof(full)); full[RUN_PLAN_DAYS] = '\0';
  TEST_ASSERT_TRUE(planTextValid(full));                     // exactly the horizon
  full[RUN_PLAN_DAYS] = 'M'; full[RUN_PLAN_DAYS + 1] = '\0';
  TEST_ASSERT_FALSE(planTextValid(full));                    // one day too many
}

// The user's example: Nov 20 Manual, Nov 27 Automatic, Nov 28 Manual, the rest follow the default.
void test_user_example(void) {
  RunPlan p; planClear(p);
  const long today = planDayFromYmd(20261118L);
  //               18 19 20 21 22 23 24 25 26 27 28
  int marked = planReplace(p, today, today, "--M------AM");
  TEST_ASSERT_EQUAL_INT(3, marked);
  TEST_ASSERT_TRUE (planManualOn(p, false, planDayFromYmd(20261120L)));
  TEST_ASSERT_FALSE(planManualOn(p, false, planDayFromYmd(20261121L)));   // default Automatic
  TEST_ASSERT_TRUE (planManualOn(p, true,  planDayFromYmd(20261121L)));   // default Manual
  TEST_ASSERT_FALSE(planManualOn(p, true,  planDayFromYmd(20261127L)));   // marked Automatic beats a Manual default
  TEST_ASSERT_TRUE (planManualOn(p, false, planDayFromYmd(20261128L)));
}

void test_days_outside_the_plan_use_the_default(void) {
  RunPlan p; planClear(p);
  TEST_ASSERT_EQUAL_CHAR(PLAN_DEFAULT, planMarkFor(p, planDayFromYmd(20261118L)));   // empty plan
  const long today = planDayFromYmd(20261118L);
  planReplace(p, today, today, "MMMM");
  TEST_ASSERT_FALSE(planManualOn(p, false, today - 1));                    // yesterday: past, so the default
  TEST_ASSERT_FALSE(planManualOn(p, false, today + RUN_PLAN_DAYS));        // beyond the horizon
  TEST_ASSERT_TRUE (planManualOn(p, true,  today + RUN_PLAN_DAYS));
}

// The website anchors its text at its own idea of today; ESP1's clock may be a day either side.
void test_text_from_a_different_day_is_shifted(void) {
  RunPlan p; planClear(p);
  const long today = planDayFromYmd(20261118L);
  // sent as starting yesterday: "A" (yesterday) is dropped, "M" lands on today
  int marked = planReplace(p, today, today - 1, "AM-A");
  TEST_ASSERT_EQUAL_INT(2, marked);
  TEST_ASSERT_EQUAL_CHAR(PLAN_MANUAL, planMarkFor(p, today));
  TEST_ASSERT_EQUAL_CHAR(PLAN_DEFAULT, planMarkFor(p, today + 1));
  TEST_ASSERT_EQUAL_CHAR(PLAN_AUTO, planMarkFor(p, today + 2));
  // sent as starting tomorrow: today stays default
  planReplace(p, today, today + 1, "M");
  TEST_ASSERT_EQUAL_CHAR(PLAN_DEFAULT, planMarkFor(p, today));
  TEST_ASSERT_EQUAL_CHAR(PLAN_MANUAL, planMarkFor(p, today + 1));
}

void test_horizon_is_enforced(void) {
  RunPlan p; planClear(p);
  const long today = planDayFromYmd(20261118L);
  char text[RUN_PLAN_DAYS + 1];
  memset(text, 'M', RUN_PLAN_DAYS); text[RUN_PLAN_DAYS] = '\0';
  // starting 5 days from now: only the days up to the horizon fit
  int marked = planReplace(p, today, today + 5, text);
  TEST_ASSERT_EQUAL_INT(RUN_PLAN_DAYS - 5, marked);
  TEST_ASSERT_EQUAL_CHAR(PLAN_MANUAL, planMarkFor(p, today + RUN_PLAN_DAYS - 1));
}

void test_empty_text_clears_the_plan(void) {
  RunPlan p; planClear(p);
  const long today = planDayFromYmd(20261118L);
  planReplace(p, today, today, "MMA");
  TEST_ASSERT_EQUAL_INT(3, planMarkedFrom(p, today));
  TEST_ASSERT_EQUAL_INT(0, planReplace(p, today, today, ""));
  TEST_ASSERT_EQUAL_INT(0, planMarkedFrom(p, today));
}

void test_text_slides_with_the_days(void) {
  RunPlan p; planClear(p);
  const long d0 = planDayFromYmd(20261118L);
  planReplace(p, d0, d0, "M-A");
  char out[RUN_PLAN_DAYS + 1];
  planText(p, d0, out);
  TEST_ASSERT_EQUAL_INT(RUN_PLAN_DAYS, (int)strlen(out));
  TEST_ASSERT_EQUAL_CHAR('M', out[0]); TEST_ASSERT_EQUAL_CHAR('-', out[1]); TEST_ASSERT_EQUAL_CHAR('A', out[2]);
  // two days later the Manual day is in the past: the text now starts at the Automatic day
  planText(p, d0 + 2, out);
  TEST_ASSERT_EQUAL_CHAR('A', out[0]);
  TEST_ASSERT_EQUAL_CHAR('-', out[RUN_PLAN_DAYS - 1]);
  TEST_ASSERT_EQUAL_INT(1, planMarkedFrom(p, d0 + 2));
}

void test_corrupt_marks_read_as_default(void) {
  RunPlan p; planClear(p);
  const long today = planDayFromYmd(20261118L);
  p.startDay = today;
  p.marks[0] = 'x'; p.marks[1] = 0; p.marks[2] = 'M';
  TEST_ASSERT_EQUAL_CHAR(PLAN_DEFAULT, planMarkFor(p, today));
  TEST_ASSERT_EQUAL_CHAR(PLAN_DEFAULT, planMarkFor(p, today + 1));
  TEST_ASSERT_EQUAL_CHAR(PLAN_MANUAL, planMarkFor(p, today + 2));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_day_numbers_round_trip);
  RUN_TEST(test_invalid_dates_are_refused);
  RUN_TEST(test_text_validation);
  RUN_TEST(test_user_example);
  RUN_TEST(test_days_outside_the_plan_use_the_default);
  RUN_TEST(test_text_from_a_different_day_is_shifted);
  RUN_TEST(test_horizon_is_enforced);
  RUN_TEST(test_empty_text_clears_the_plan);
  RUN_TEST(test_text_slides_with_the_days);
  RUN_TEST(test_corrupt_marks_read_as_default);
  return UNITY_END();
}
