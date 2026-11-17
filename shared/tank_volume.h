/* tank_volume.h -- ultrasonic distance -> litres, for both tanks (pure, host-testable).
 *
 * web v.4 "pure ultrasonic" (2026-11-16): the ResMix / MixIrr flow meters are gone. Every litre moved by the
 * transfer and booster pumps is now measured by the ultrasonic sensor of each tank, so the conversion from
 * a water level to a volume is what the whole water bookkeeping rests on. It lives here, once, with no
 * Arduino types, so ESP1 (which owns the calibration) and the host tests use the very same maths.
 *
 * SHAPE. Each tank is a truncated cone (frustum), narrow at the bottom and open/wide at the top:
 *     inner diameter ~31.0 cm at the 0 % level (EMPTY distance)            [MEASURE]
 *     inner diameter ~39.4 cm at the 100 % level (FULL distance)           [MEASURE]
 * Water height h (cm above the 0 % level) = emptyCm - distanceCm. With the radius growing linearly with h:
 *     V(h) = pi * h * (r0^2 + r0*r(h) + r(h)^2) / 3        (cm^3; /1000 -> litres)
 * With the 2026-11-15 geometry (empty 42.9 cm, full 24.0 cm -> an 18.9 cm span) that is ~18.5 L at 100 %.
 *
 * CALIBRATION POINTS. The cone is only the starting estimate. On the LCD (Settings > Calibration > "Tank Mix
 * liters" / "Tank Res liters") the operator locks the current level and types the litres actually in the tank
 * (measured with a jug, a marked bucket, ...). Up to TV_MAX_PTS points per tank. Between points the curve
 * follows the cone's SHAPE, stretched to pass exactly through them, so two or three points already correct
 * both a wrong diameter and water held below the 0 % mark:
 *     L(h) = L_i + (L_i+1 - L_i) * (V(h) - V(h_i)) / (V(h_i+1) - V(h_i))
 * Below the first point the curve runs to (h = 0, 0 L) unless the operator gave a point at h ~ 0 (e.g. "2 L
 * left when the sensor reads empty"). Above the last point it continues with the last segment's stretch.
 *
 * INSERT (2026-11-17). The mixing tank has a mixer standing on its bottom: a cylinder 6 cm across and 11.4 cm tall
 * (322.3 cm^3). Water cannot be where it is, so the tank's SHAPE is the cone minus the part of the insert under the
 * water: tvShapeL(h) = tvConeL(h) - pi * (d/2)^2 * min(h, insert height). The ultrasonic is not in its way. Everything
 * below follows the shape, so the insert counts with or without calibration points (with points, they hold exactly
 * and the curve between them bends like the real tank). insDiamCm = 0 = no insert (the reservoir).
 *
 * Do not add anything that touches hardware, time or global state to this file.
 */
#ifndef TANK_VOLUME_H
#define TANK_VOLUME_H

#include <math.h>
#include <stdint.h>

#ifndef TV_PI
#define TV_PI 3.14159265f
#endif

const uint8_t TV_MAX_PTS         = 8;      // calibration points per tank
const float   TV_PT_MERGE_CM     = 0.3f;   // a new point this close (in height) to an old one replaces it
const float   TV_ANCHOR_CM       = 0.5f;   // a user point at or below this height replaces the (0 cm, 0 L) anchor
// Heights above FULL are real (2026-11-16 user rule: water past the full mark reads past 100 %, e.g. 120 %):
// the cone keeps widening at the same taper above the mark, up to 200 % of the span (= ESP1 LEVEL_PCT_MAX).
// Calibration points may be taken up there too.
const float   TV_OVER_FULL       = 2.0f;
const float   TV_STRETCH_MIN     = 0.25f;  // extrapolation stretch limits (a wild last segment cannot run away)
const float   TV_STRETCH_MAX     = 4.0f;
const float   TV_MIN_SPAN_CM     = 2.0f;   // = ESP1's CAL_LEVEL_MIN_SPAN_CM
const float   TV_DIAM_MIN_CM     = 5.0f, TV_DIAM_MAX_CM = 300.0f;
const float   TV_L_MAX           = 2000.0f;   // a typed volume above this is a typo, not a tank

/* Speed-of-sound correction (dry air, m/s): c(T) = 331.3 + 0.606 * T[degC]. The Nano converts echo time to cm
 * with a fixed 343 m/s (~20 degC). Distances are scaled to what they would read at the REFERENCE temperature
 * the geometry was measured at, so a hot afternoon does not read as "the tank is emptier": at 40 degC vs 30 degC
 * the raw distance to the same water is ~1.7 % longer (~0.7 cm at 43 cm = ~0.6 L on this tank). */
static inline float tvSoundMps(float tC) { return 331.3f + 0.606f * tC; }
static inline float tvTempCorrectCm(float rawCm, float tC, float tRefC) {
  if (!(rawCm >= 0.0f)) return rawCm;                        // -1 (no echo) and NaN pass through unchanged
  if (!(tC > -20.0f && tC < 70.0f)) return rawCm;            // no / implausible temperature: leave it raw
  return rawCm * tvSoundMps(tRefC) / tvSoundMps(tC);
}

struct TankVolCal {
  float   emptyCm, fullCm;        // sensor-to-water distance at 0 % and at 100 %
  float   dBottomCm, dTopCm;      // inner diameter at the 0 % level and at the 100 % level
  uint8_t n;                      // calibration points in use (0..TV_MAX_PTS)
  float   hCm[TV_MAX_PTS];        // water height above the 0 % level, ascending
  float   L[TV_MAX_PTS];          // litres the operator measured at that height
  // Last, so the older 7-value initialisers still mean "no insert" (2026-11-17):
  float   insDiamCm, insHCm;      // a cylinder standing on the 0 % level (the mixer), 0 = none
};

static inline float tvSpanCm(const TankVolCal &c) { return c.emptyCm - c.fullCm; }

static inline bool tvGeomSane(const TankVolCal &c) {
  return isfinite(c.emptyCm) && isfinite(c.fullCm) && tvSpanCm(c) >= TV_MIN_SPAN_CM &&
         c.dBottomCm >= TV_DIAM_MIN_CM && c.dBottomCm <= TV_DIAM_MAX_CM &&
         c.dTopCm >= TV_DIAM_MIN_CM && c.dTopCm <= TV_DIAM_MAX_CM;
}

// The cone alone: litres held between the 0 % level and height h (cm).
static inline float tvConeL(const TankVolCal &c, float h) {
  float H = tvSpanCm(c);
  if (!(H > 0.0f) || !(h > 0.0f)) return 0.0f;
  float r0 = c.dBottomCm * 0.5f;
  float r  = r0 + (c.dTopCm - c.dBottomCm) * 0.5f * h / H;
  if (r < 0.0f) r = 0.0f;
  return TV_PI * h * (r0 * r0 + r0 * r + r * r) / 3.0f / 1000.0f;
}

// The insert's volume under water at height h (litres).
static inline float tvInsertL(const TankVolCal &c, float h) {
  if (!(c.insDiamCm > 0.0f) || !(c.insHCm > 0.0f) || !(h > 0.0f)) return 0.0f;
  float r = c.insDiamCm * 0.5f;
  return TV_PI * r * r * (h < c.insHCm ? h : c.insHCm) / 1000.0f;
}
// The water the tank holds between the 0 % level and height h: the cone minus the insert.
static inline float tvShapeL(const TankVolCal &c, float h) {
  float v = tvConeL(c, h) - tvInsertL(c, h);
  return v > 0.0f ? v : 0.0f;
}

// Points must climb: more water at a higher level. Equal or falling litres are a capture mistake.
static inline bool tvPointsSane(const TankVolCal &c) {
  if (c.n > TV_MAX_PTS) return false;
  for (uint8_t i = 0; i < c.n; i++) {
    if (!isfinite(c.hCm[i]) || !isfinite(c.L[i]) || c.L[i] < 0.0f || c.L[i] > TV_L_MAX) return false;
    if (c.hCm[i] < -TV_ANCHOR_CM || c.hCm[i] > tvSpanCm(c) * TV_OVER_FULL) return false;
    if (i > 0 && (c.hCm[i] <= c.hCm[i - 1] || c.L[i] <= c.L[i - 1])) return false;
  }
  return true;
}

/* Litres at water height h (cm above the 0 % level). Never negative, never NaN for a sane cal. */
static inline float tvLitersAtH(const TankVolCal &c, float h) {
  float H = tvSpanCm(c);
  if (!(H > 0.0f)) return 0.0f;
  if (!(h > 0.0f)) h = 0.0f;
  if (h > H * TV_OVER_FULL) h = H * TV_OVER_FULL;
  // Anchors: (0, 0) unless a user point sits at the bottom, then the user points in height order.
  float ah[TV_MAX_PTS + 1], aL[TV_MAX_PTS + 1];
  uint8_t na = 0;
  uint8_t n = (c.n <= TV_MAX_PTS) ? c.n : TV_MAX_PTS;
  if (n == 0 || c.hCm[0] > TV_ANCHOR_CM) { ah[na] = 0.0f; aL[na] = 0.0f; na++; }
  for (uint8_t i = 0; i < n; i++) { ah[na] = c.hCm[i] < 0.0f ? 0.0f : c.hCm[i]; aL[na] = c.L[i]; na++; }
  float v = tvShapeL(c, h);
  float out;
  if (na == 1) {                                   // no user points: the shape, offset by a bottom point if any
    out = aL[0] + (v - tvShapeL(c, ah[0]));
  } else if (h <= ah[0]) {
    out = aL[0];
  } else {
    uint8_t i = 0;
    while (i + 2 < na && h > ah[i + 1]) i++;       // segment [i, i+1] holds h, or the last segment if above
    float v0 = tvShapeL(c, ah[i]), v1 = tvShapeL(c, ah[i + 1]);
    float stretch;
    if (v1 - v0 > 1e-6f) stretch = (aL[i + 1] - aL[i]) / (v1 - v0);
    else                 stretch = 1.0f;
    if (h <= ah[i + 1]) {
      if (v1 - v0 > 1e-6f) out = aL[i] + (v - v0) * stretch;
      else                 out = aL[i] + (aL[i + 1] - aL[i]) * (h - ah[i]) / (ah[i + 1] - ah[i]);
    } else {                                       // above the last point: same stretch, bounded
      if (stretch < TV_STRETCH_MIN) stretch = TV_STRETCH_MIN;
      if (stretch > TV_STRETCH_MAX) stretch = TV_STRETCH_MAX;
      out = aL[i + 1] + (v - v1) * stretch;
    }
  }
  if (!(out > 0.0f)) out = 0.0f;
  return out;
}

// Height above the 0 % level for a (temperature-corrected) sensor distance.
static inline float tvHeightCm(const TankVolCal &c, float distCm) { return c.emptyCm - distCm; }

// Litres for a sensor distance; -1 for no reading (a negative / NaN distance) or an unusable geometry.
static inline float tvLiters(const TankVolCal &c, float distCm) {
  if (!(distCm >= 0.0f) || !tvGeomSane(c)) return -1.0f;
  return tvLitersAtH(c, tvHeightCm(c, distCm));
}

// Litres at a fill percentage of the empty..full span (capacity planning: "how much is 90 %?").
static inline float tvLitersAtPct(const TankVolCal &c, float pct) {
  if (!tvGeomSane(c)) return 0.0f;
  return tvLitersAtH(c, tvSpanCm(c) * pct / 100.0f);
}

/* Add (or replace) one calibration point. A point within TV_PT_MERGE_CM of an existing one replaces it (a
 * re-measure); with the table full, the nearest point is replaced. Refused -- nothing changes -- when the
 * result would not climb (tvPointsSane): e.g. 6 L typed at a level below a 7 L point. Returns true on success. */
static inline bool tvAddPoint(TankVolCal &c, float h, float L) {
  if (!isfinite(h) || !isfinite(L) || L < 0.0f || L > TV_L_MAX) return false;
  if (h < -TV_ANCHOR_CM || h > tvSpanCm(c) * TV_OVER_FULL) return false;
  if (h < 0.0f) h = 0.0f;
  TankVolCal t = c;
  if (t.n > TV_MAX_PTS) t.n = 0;
  int near = -1; float nd = 1e9f;
  for (uint8_t i = 0; i < t.n; i++) { float d = fabsf(t.hCm[i] - h); if (d < nd) { nd = d; near = i; } }
  if (near >= 0 && (nd <= TV_PT_MERGE_CM || t.n >= TV_MAX_PTS)) {
    for (uint8_t i = (uint8_t)near; i + 1 < t.n; i++) { t.hCm[i] = t.hCm[i + 1]; t.L[i] = t.L[i + 1]; }
    t.n--;
  }
  uint8_t pos = 0;
  while (pos < t.n && t.hCm[pos] < h) pos++;
  for (uint8_t i = t.n; i > pos; i--) { t.hCm[i] = t.hCm[i - 1]; t.L[i] = t.L[i - 1]; }
  t.hCm[pos] = h; t.L[pos] = L; t.n++;
  if (!tvPointsSane(t)) return false;
  c = t;
  return true;
}

static inline void tvClearPoints(TankVolCal &c) { c.n = 0; }

#endif // TANK_VOLUME_H
