/* run_plan.h -- a column's Operation (Automatic / Manual) and its day plan (2026-11-18, website request).
 *
 * Automatic: the column waters on its own watering times, exactly as before.
 * Manual:    ESP1 starts no scheduled run for the column. Its readings, forced runs ("Water a column now" on the
 *            website or the LCD) and everything else carry on; the operator irrigates and fertigates it by hand.
 *
 * Each column has a default Operation, plus a day plan covering today and the next RUN_PLAN_DAYS - 1 days. A marked
 * day overrides the default for that date only: 'M' = Manual that day, 'A' = Automatic that day, '-' = the default.
 *
 * Pure functions only -- no Arduino headers, no clock, no globals -- so `pio test -e native` covers all of it
 * (test/test_run_plan). Dates are yyyymmdd longs, the same form as ESP1's currentDayStamp; a "day" is a day number,
 * days since 1970-01-01, so plain subtraction counts days across month and year ends.
 */
#ifndef RUN_PLAN_H
#define RUN_PLAN_H

#include <stdint.h>
#include <string.h>

const int  RUN_PLAN_DAYS = 62;          // today + 61 days: about two months ahead
const char PLAN_DEFAULT  = '-';
const char PLAN_MANUAL   = 'M';
const char PLAN_AUTO     = 'A';

struct RunPlan {
  long startDay;                        // day number of marks[0]; -1 = an empty plan
  char marks[RUN_PLAN_DAYS];            // one of PLAN_DEFAULT / PLAN_MANUAL / PLAN_AUTO per day
};

/* ---- dates ------------------------------------------------------------------------------------------------------ */

// Days since 1970-01-01 for a Gregorian date (H. Hinnant's days_from_civil).
static inline long planDaysFromCivil(long y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;                                        // [0, 399]
  const long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;     // [0, 365]
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
  return era * 146097 + doe - 719468;
}

// A real calendar date between 2000-01-01 and 2099-12-31, as yyyymmdd.
static inline bool planYmdValid(long ymd) {
  const long y = ymd / 10000;
  const int  m = (int)((ymd / 100) % 100), d = (int)(ymd % 100);
  if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1) return false;
  static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return d <= DIM[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

// yyyymmdd -> day number; -1 for anything that is not a real date.
static inline long planDayFromYmd(long ymd) {
  if (!planYmdValid(ymd)) return -1;
  return planDaysFromCivil(ymd / 10000, (int)((ymd / 100) % 100), (int)(ymd % 100));
}

// day number -> yyyymmdd (H. Hinnant's civil_from_days).
static inline long planYmdFromDay(long z) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const long doe = z - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp  = (5 * doy + 2) / 153;
  const long d   = doy - (153 * mp + 2) / 5 + 1;
  const long m   = mp < 10 ? mp + 3 : mp - 9;
  const long y   = yoe + era * 400 + (m <= 2);
  return y * 10000 + m * 100 + d;
}

/* ---- the plan --------------------------------------------------------------------------------------------------- */

static inline void planClear(RunPlan &p) {
  p.startDay = -1;
  memset(p.marks, PLAN_DEFAULT, sizeof(p.marks));
}

// The mark for one day; PLAN_DEFAULT for any day the plan does not cover (past days included).
static inline char planMarkFor(const RunPlan &p, long day) {
  if (p.startDay < 0 || day < p.startDay || day >= p.startDay + RUN_PLAN_DAYS) return PLAN_DEFAULT;
  const char m = p.marks[day - p.startDay];
  return (m == PLAN_MANUAL || m == PLAN_AUTO) ? m : PLAN_DEFAULT;      // anything stored oddly reads as default
}

// Whether the column is in Manual on `day`: that day's mark, else the column's default Operation.
static inline bool planManualOn(const RunPlan &p, bool defaultManual, long day) {
  const char m = planMarkFor(p, day);
  if (m == PLAN_MANUAL) return true;
  if (m == PLAN_AUTO)   return false;
  return defaultManual;
}

// A plan as the website sends it: only '-', 'M' and 'A', at most RUN_PLAN_DAYS of them ("" clears the plan).
static inline bool planTextValid(const char *s) {
  if (!s) return false;
  const size_t n = strlen(s);
  if (n > (size_t)RUN_PLAN_DAYS) return false;
  for (size_t i = 0; i < n; i++)
    if (s[i] != PLAN_DEFAULT && s[i] != PLAN_MANUAL && s[i] != PLAN_AUTO) return false;
  return true;
}

// Replaces the whole plan with `text`, whose first character is the day `fromDay`. The website may be a day off
// ESP1's clock, so characters for days before `today` or past the horizon are dropped rather than refused.
// Returns how many days end up marked (not default).
static inline int planReplace(RunPlan &p, long today, long fromDay, const char *text) {
  planClear(p);
  p.startDay = today;
  int marked = 0;
  for (size_t i = 0; text && text[i]; i++) {
    const long day = fromDay + (long)i;
    if (day < today || day >= today + RUN_PLAN_DAYS) continue;
    const char c = text[i];
    if (c != PLAN_MANUAL && c != PLAN_AUTO) continue;
    p.marks[day - today] = c;
    marked++;
  }
  return marked;
}

// Days still marked from `today` on (past marks no longer count).
static inline int planMarkedFrom(const RunPlan &p, long today) {
  int n = 0;
  for (int i = 0; i < RUN_PLAN_DAYS; i++) if (planMarkFor(p, today + i) != PLAN_DEFAULT) n++;
  return n;
}

// The plan as text starting at `today`, RUN_PLAN_DAYS characters plus the terminating NUL -- what the website reads.
static inline void planText(const RunPlan &p, long today, char out[RUN_PLAN_DAYS + 1]) {
  for (int i = 0; i < RUN_PLAN_DAYS; i++) out[i] = planMarkFor(p, today + i);
  out[RUN_PLAN_DAYS] = '\0';
}

#endif  // RUN_PLAN_H
