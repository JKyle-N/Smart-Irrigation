/* Host tests for shared/tank_volume.h (web v.4 "pure ultrasonic"): ultrasonic cm -> litres.
 *     pio test -e native -f test_tank_volume
 * Needs a host C++ compiler (see platformio.ini). */
#include <unity.h>
#include "../../../shared/tank_volume.h"

static TankVolCal cone() {
  TankVolCal c = { 42.9f, 24.0f, 31.0f, 39.4f, 0, {0}, {0} };   // 2026-11-15 geometry, 31 / 39.4 cm frustum
  return c;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- the cone ---------------------------------------------------------------------------------- */
void test_cone_empty_is_zero(void) {
  TankVolCal c = cone();
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, tvLiters(c, 42.9f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, tvLiters(c, 50.0f));          // below empty: never negative
}
void test_cone_full_matches_frustum_formula(void) {
  TankVolCal c = cone();
  // pi * 18.9 * (15.5^2 + 15.5*19.7 + 19.7^2) / 3 / 1000 = 18.48 L
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 18.48f, tvLiters(c, 24.0f));
}
void test_cone_is_wider_at_the_top(void) {
  TankVolCal c = cone();
  float lowCm  = tvLitersAtH(c, 2.0f) - tvLitersAtH(c, 1.0f);     // litres per cm near the bottom
  float highCm = tvLitersAtH(c, 18.0f) - tvLitersAtH(c, 17.0f);   // ... near the top
  TEST_ASSERT_TRUE(highCm > lowCm * 1.4f);                         // area ratio (39.4/31)^2 ~ 1.6
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.79f, lowCm);                   // ~pi * 15.8^2 at h = 1..2 cm
}
void test_cone_clamps_far_above_full(void) {
  TankVolCal c = cone();
  TEST_ASSERT_EQUAL_FLOAT(tvLitersAtH(c, 18.9f * TV_OVER_FULL), tvLiters(c, 0.0f));
}
void test_no_reading_and_bad_geometry_give_minus_one(void) {
  TankVolCal c = cone();
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, tvLiters(c, -1.0f));
  c.fullCm = 42.0f;                                                 // span < 2 cm
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, tvLiters(c, 30.0f));
}
void test_past_full_keeps_counting(void) {
  TankVolCal c = cone();
  float full = tvLitersAtPct(c, 100.0f), p120 = tvLitersAtPct(c, 120.0f);
  TEST_ASSERT_TRUE(p120 > full + 3.0f);                             // 3.78 cm more, at ~1.2 L/cm
  TEST_ASSERT_FLOAT_WITHIN(0.01f, p120, tvLiters(c, 42.9f - 18.9f * 1.2f));   // 20.22 cm from the sensor
  TEST_ASSERT_TRUE(tvAddPoint(c, 18.9f * 1.2f, p120 + 0.5f));      // a point above full is accepted
}
void test_litres_at_pct(void) {
  TankVolCal c = cone();
  TEST_ASSERT_FLOAT_WITHIN(0.01f, tvLitersAtH(c, 18.9f * 0.9f), tvLitersAtPct(c, 90.0f));
  TEST_ASSERT_TRUE(tvLitersAtPct(c, 90.0f) > 16.0f && tvLitersAtPct(c, 90.0f) < 17.0f);
}

/* ---- calibration points ------------------------------------------------------------------------ */
void test_one_point_scales_the_cone(void) {
  TankVolCal c = cone();
  float coneAt10 = tvLitersAtH(c, 10.0f);
  TEST_ASSERT_TRUE(tvAddPoint(c, 10.0f, coneAt10 * 1.2f));          // the tank holds 20 % more than drawn
  TEST_ASSERT_FLOAT_WITHIN(0.01f, coneAt10 * 1.2f, tvLitersAtH(c, 10.0f));    // passes through the point
  TEST_ASSERT_FLOAT_WITHIN(0.02f, tvLitersAtH(cone(), 5.0f) * 1.2f, tvLitersAtH(c, 5.0f));   // stretched below
  TEST_ASSERT_FLOAT_WITHIN(0.05f, tvLitersAtH(cone(), 18.9f) * 1.2f, tvLitersAtH(c, 18.9f)); // ... and above
}
void test_points_are_passed_through_exactly(void) {
  TankVolCal c = cone();
  TEST_ASSERT_TRUE(tvAddPoint(c, 5.0f, 4.0f));
  TEST_ASSERT_TRUE(tvAddPoint(c, 12.0f, 11.0f));
  TEST_ASSERT_TRUE(tvAddPoint(c, 18.0f, 18.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f,  tvLitersAtH(c, 5.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 11.0f, tvLitersAtH(c, 12.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.5f, tvLitersAtH(c, 18.0f));
  float mid = tvLitersAtH(c, 8.5f);
  TEST_ASSERT_TRUE(mid > 4.0f && mid < 11.0f);                      // between its neighbours
}
void test_point_at_the_bottom_is_dead_water(void) {
  TankVolCal c = cone();
  TEST_ASSERT_TRUE(tvAddPoint(c, 0.0f, 2.0f));                      // "2 L left when the sensor reads empty"
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, tvLiters(c, 42.9f));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 18.48f + 2.0f, tvLiters(c, 24.0f));   // the cone on top of it
}
void test_curve_never_falls(void) {
  TankVolCal c = cone();
  tvAddPoint(c, 4.0f, 3.5f); tvAddPoint(c, 10.0f, 8.0f); tvAddPoint(c, 16.0f, 15.0f);
  float last = -1.0f;
  for (float h = 0.0f; h <= 20.0f; h += 0.25f) {
    float v = tvLitersAtH(c, h);
    TEST_ASSERT_TRUE(v >= last);
    last = v;
  }
}
void test_non_climbing_point_is_refused(void) {
  TankVolCal c = cone();
  TEST_ASSERT_TRUE(tvAddPoint(c, 10.0f, 8.0f));
  TEST_ASSERT_FALSE(tvAddPoint(c, 14.0f, 7.0f));                    // higher level, fewer litres: a mistake
  TEST_ASSERT_FALSE(tvAddPoint(c, 6.0f, 9.0f));                     // lower level, more litres
  TEST_ASSERT_EQUAL_UINT8(1, c.n);                                  // nothing changed
}
void test_close_point_replaces(void) {
  TankVolCal c = cone();
  TEST_ASSERT_TRUE(tvAddPoint(c, 10.0f, 8.0f));
  TEST_ASSERT_TRUE(tvAddPoint(c, 10.2f, 8.3f));                     // re-measure within 0.3 cm
  TEST_ASSERT_EQUAL_UINT8(1, c.n);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.3f, c.L[0]);
}
void test_full_table_replaces_nearest(void) {
  TankVolCal c = cone();
  for (int i = 0; i < TV_MAX_PTS; i++) TEST_ASSERT_TRUE(tvAddPoint(c, 2.0f + 2.0f * i, 1.5f + 2.0f * i));
  TEST_ASSERT_EQUAL_UINT8(TV_MAX_PTS, c.n);
  TEST_ASSERT_TRUE(tvAddPoint(c, 7.0f, 6.6f));                      // nearest is h=6 (or 8): replaced
  TEST_ASSERT_EQUAL_UINT8(TV_MAX_PTS, c.n);
  TEST_ASSERT_TRUE(tvPointsSane(c));
}
void test_out_of_range_point_refused(void) {
  TankVolCal c = cone();
  TEST_ASSERT_FALSE(tvAddPoint(c, -3.0f, 1.0f));
  TEST_ASSERT_FALSE(tvAddPoint(c, 40.0f, 20.0f));                   // > 200 % of the span
  TEST_ASSERT_FALSE(tvAddPoint(c, 5.0f, -1.0f));
  TEST_ASSERT_FALSE(tvAddPoint(c, 5.0f, TV_L_MAX + 1.0f));
}
void test_clear_points(void) {
  TankVolCal c = cone();
  tvAddPoint(c, 10.0f, 9.0f);
  tvClearPoints(c);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, tvLitersAtH(cone(), 10.0f), tvLitersAtH(c, 10.0f));
}

/* ---- temperature ------------------------------------------------------------------------------- */
void test_temperature_correction(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 40.0f, tvTempCorrectCm(40.0f, 30.0f, 30.0f));   // at the reference: unchanged
  float hot = tvTempCorrectCm(40.0f, 40.0f, 30.0f);                 // sound is faster: raw over-reads
  TEST_ASSERT_TRUE(hot < 40.0f && hot > 39.0f);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, tvTempCorrectCm(-1.0f, 40.0f, 30.0f));   // no echo stays no echo
  TEST_ASSERT_EQUAL_FLOAT(40.0f, tvTempCorrectCm(40.0f, 200.0f, 30.0f));  // absurd temperature: raw
}

// The mixer (2026-11-17): 6 cm x 11.4 cm standing on the bottom = 0.3223 L, 0.0283 L per cm of water over it.
static TankVolCal mixTank() {
  TankVolCal c = cone();
  c.insDiamCm = 6.0f; c.insHCm = 11.4f;
  return c;
}
void test_mixer_takes_its_volume_out(void) {
  TankVolCal plain = cone(), mix = mixTank();
  const float perCm = 3.14159265f * 9.0f / 1000.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f * perCm, tvLitersAtH(plain, 5.0f) - tvLitersAtH(mix, 5.0f));   // part under
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3223f, tvLitersAtH(plain, 15.0f) - tvLitersAtH(mix, 15.0f));     // all of it
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3223f, tvLitersAtPct(plain, 100.0f) - tvLitersAtPct(mix, 100.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, tvLitersAtH(mix, 0.0f));
  float prev = -1.0f;                                    // still climbs everywhere
  for (int i = 0; i <= 200; i++) { float L = tvLitersAtH(mix, i * 0.1f); TEST_ASSERT_TRUE(L > prev || i == 0); prev = L; }
}
void test_mixer_with_points_still_passes_through_them(void) {
  TankVolCal c = mixTank();
  TEST_ASSERT_TRUE(tvAddPoint(c, 6.0f, 5.0f));
  TEST_ASSERT_TRUE(tvAddPoint(c, 15.0f, 13.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, tvLitersAtH(c, 6.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.0f, tvLitersAtH(c, 15.0f));
}

// The tape-measured tank (2026-11-18): 34.60 cm across at the floor, +0.1219 cm per cm, sensor head 47.5 cm above the
// floor. ESP1 puts the 42.9 / 24.0 cm marks on it (4.6 and 23.5 cm of water): 35.16 / 37.46 cm across there, and
// the mixer's 11.4 cm stands 6.8 cm above the 0 % mark. These are the litres the guides' tables quote.
static float tapeD(float hFromFloor) { return 34.60f + 0.1219f * hFromFloor; }
void test_measured_tank_shape(void) {
  TankVolCal mix = { 42.9f, 24.0f, tapeD(4.6f), tapeD(23.5f), 0, {0}, {0}, 6.0f, 11.4f - 4.6f };
  TankVolCal res = { 42.9f, 24.0f, tapeD(4.6f), tapeD(23.5f), 0, {0}, {0} };
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.89f, tvLitersAtPct(mix, 5.0f));     // the delivery low stop
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 9.29f, tvLitersAtPct(mix, 50.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 17.32f, tvLitersAtPct(mix, 90.0f));   // MIXCAP
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 19.39f, tvLitersAtPct(mix, 100.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 19.58f, tvLitersAtPct(res, 100.0f));
  // The whole body, floor to the 35.8 cm line (47.5 -> 11.7 cm from the sensor), no mixer: 38.08 L.
  TankVolCal body = { 47.5f, 11.7f, tapeD(0.0f), tapeD(35.8f), 0, {0}, {0} };
  TEST_ASSERT_FLOAT_WITHIN(0.03f, 38.08f, tvLitersAtPct(body, 100.0f));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_measured_tank_shape);
  RUN_TEST(test_cone_empty_is_zero);
  RUN_TEST(test_cone_full_matches_frustum_formula);
  RUN_TEST(test_cone_is_wider_at_the_top);
  RUN_TEST(test_cone_clamps_far_above_full);
  RUN_TEST(test_no_reading_and_bad_geometry_give_minus_one);
  RUN_TEST(test_past_full_keeps_counting);
  RUN_TEST(test_litres_at_pct);
  RUN_TEST(test_one_point_scales_the_cone);
  RUN_TEST(test_points_are_passed_through_exactly);
  RUN_TEST(test_point_at_the_bottom_is_dead_water);
  RUN_TEST(test_curve_never_falls);
  RUN_TEST(test_non_climbing_point_is_refused);
  RUN_TEST(test_close_point_replaces);
  RUN_TEST(test_full_table_replaces_nearest);
  RUN_TEST(test_out_of_range_point_refused);
  RUN_TEST(test_clear_points);
  RUN_TEST(test_temperature_correction);
  RUN_TEST(test_mixer_takes_its_volume_out);
  RUN_TEST(test_mixer_with_points_still_passes_through_them);
  return UNITY_END();
}
