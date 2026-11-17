/* pump_line.h -- a pump's litres against its run time, timed from the inverter SWITCH-ON (pure, host-testable).
 *
 * 2026-11-17 (user): the transfer and booster pumps were measured with a container on a scale, the time counted from
 * the moment the inverter is switched on (its start-up included). A straight line fits them:
 *     litres(t) = startL + rate x t          (rate in L/min, t in seconds from the switch-on)
 * startL is what the start adds or takes away (negative: the inverter's start-up gives no water at first). Below
 * PL_MIN_RUN_S (the shortest run measured) the line means nothing, so the litres ramp from 0 at the switch-on up to
 * the line at PL_MIN_RUN_S.
 *
 * The LCD screens "Transfer pump est" / "Booster pump est" (ESP1, Settings > Calibration) re-measure a pump: run it for
 * a chosen number of seconds, weigh the water, type the grams -- at least PL_MIN_TRIES tries -- and plFit() gives its new
 * line. Until a pump is re-measured, the user's readings of 17 Nov below ARE its line (plFitReadings at boot).
 *
 * Do not add anything that touches hardware, time or global state to this file.
 */
#ifndef PUMP_LINE_H
#define PUMP_LINE_H

#include <math.h>
#include <stdint.h>

const float   PL_G_PER_L      = 1028.0f;   // the water here: 1 L weighs 1028 g (user, 17 Nov)
const float   PL_MIN_RUN_S    = 6.0f;      // the shortest run measured
const uint8_t PL_MIN_TRIES    = 5;         // fewer tries give no line (user: at least 5)
const float   PL_MIN_SPREAD_S = 1.5f;      // the tries' run times must differ by this much for a rate
const float   PL_START_L_MAX  = 3.0f;      // a start of more than +-3 L is a bad try, not a pump

// The user's readings, 17 Nov 2026: { container g, container + water g, seconds from the inverter switch-on }.
static const float PL_XFER_READINGS[10][3] = {          // transfer pump, reservoir -> mixing tank
  { 73, 1980, 6.15f }, { 76, 1995, 6.17f }, { 75, 1949, 6.00f }, { 76, 1807, 6.84f }, { 77, 1798, 6.91f },
  { 76, 1509, 6.15f }, { 75, 1863, 7.12f }, { 75, 2426, 8.68f }, { 75, 2214, 8.10f }, { 75, 2153, 7.89f } };
static const float PL_BOOST_READINGS[10][3] = {         // booster, straight out of the column valve (no drip pipe)
  { 74, 1413, 8.02f }, { 76, 1262, 7.25f }, { 76, 1040, 7.76f }, { 76,  876, 6.93f }, { 76,  950, 7.19f },   // column A
  { 76, 1255, 8.22f }, { 76, 1076, 7.38f }, { 76, 1283, 8.36f }, { 76, 1206, 8.16f }, { 75, 1309, 8.61f } }; // column B

struct PlFit { bool ok; float rateLpm, startL; uint8_t n; };

// Least squares litres = startL + rate x t over n tries. ok = false with fewer than PL_MIN_TRIES, run times too
// alike to give a rate (PL_MIN_SPREAD_S), water that does not grow with time, or a start beyond PL_START_L_MAX.
static inline PlFit plFit(const float *tS, const float *vL, uint8_t n) {
  PlFit f = { false, 0.0f, 0.0f, n };
  if (n < PL_MIN_TRIES) return f;
  float mt = 0.0f, mv = 0.0f, lo = tS[0], hi = tS[0];
  for (uint8_t i = 0; i < n; i++) {
    if (!isfinite(tS[i]) || !isfinite(vL[i])) return f;
    mt += tS[i];
    mv += vL[i];
    if (tS[i] < lo) lo = tS[i];
    if (tS[i] > hi) hi = tS[i];
  }
  if (hi - lo < PL_MIN_SPREAD_S) return f;
  mt /= n; mv /= n;
  float sxx = 0.0f, sxy = 0.0f;
  for (uint8_t i = 0; i < n; i++) { sxx += (tS[i] - mt) * (tS[i] - mt); sxy += (tS[i] - mt) * (vL[i] - mv); }
  if (!(sxx > 0.0f)) return f;
  float b = sxy / sxx;
  if (!(b > 0.0f)) return f;
  f.rateLpm = b * 60.0f;
  f.startL = mv - b * mt;
  f.ok = fabsf(f.startL) <= PL_START_L_MAX;
  return f;
}
static inline PlFit plFitReadings(const float r[][3], uint8_t n) {
  float t[16], v[16];
  if (n > 16) n = 16;
  for (uint8_t i = 0; i < n; i++) { t[i] = r[i][2]; v[i] = (r[i][1] - r[i][0]) / PL_G_PER_L; }
  return plFit(t, v, n);
}
// Litres tS seconds after the switch-on. Never negative.
static inline float plLitresAt(float tS, float rateLpm, float startL) {
  if (!(tS > 0.0f) || !(rateLpm > 0.0f)) return 0.0f;
  float q = rateLpm / 60.0f;
  float atMin = startL + q * PL_MIN_RUN_S;
  if (atMin < 0.0f) atMin = 0.0f;
  if (tS < PL_MIN_RUN_S) return atMin * tS / PL_MIN_RUN_S;
  float v = startL + q * tS;
  return v > 0.0f ? v : 0.0f;
}

#endif // PUMP_LINE_H
