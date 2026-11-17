/* rig_constants.h -- values ESP1 and ESP2 must agree on, defined once.
 *
 * Before this file each firmware held its own copy, and nothing checked them: a batch planned by ESP1 that
 * ESP2 would not deliver is a silent mismatch. Change a value here and reflash BOTH controllers.
 *
 * The Nano is not included: its sketch also builds from the Arduino IDE, which cannot reach outside its
 * folder. COLUMN_ENABLED is not here either, because ESP1 can change its copy at runtime.
 */
#ifndef RIG_CONSTANTS_H
#define RIG_CONSTANTS_H

// Litres delivered per column per service.                                         [TBD]
const float WATER_BUDGET_L[3] = { 5.0f, 5.0f, 5.0f };

// Mixing-tank safety ceiling, litres. An outer sanity bound on any volume that crosses the wire; the
// real planning ceiling is the tank calibration's litres at MIX_HIGH_PCT (ESP1 sends it as MIXCAP --
// ~16.2 L with the default 31 / 39.4 cm frustum and the 2026-11-15 geometry).          [MEASURE]
const float MIXING_TANK_MAX_L = 50.0f;

// Mixing-tank overflow stop, % of the empty..full span: ESP2 stops a fill at it (LEVEL_HIGH), and ESP1
// works out the litres it means (MIXCAP) from the tank calibration. Was an ESP2-only const.      [TBD]
const float MIX_HIGH_PCT = 90.0f;

// Reservoir filter zone (2026-11-16, user): the reservoir has a filter at the bottom, and its ultrasonic stops at
// ~15 % -- it cannot see whether water is left below that. At or under this % the reading says nothing about the
// water left: ESP2 stops metering a fill on the reservoir and judges "empty" on the mixing tank instead, and ESP1
// treats the reservoir as low (no new run starts).                                                 [MEASURE]
const float RES_FILTER_PCT = 18.0f;

#endif // RIG_CONSTANTS_H
