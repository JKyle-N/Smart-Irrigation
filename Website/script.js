/*
 * Dashboard contract
 * ------------------
 * irrigation/live     ESP1 -> dashboard (latest verified snapshot)
 * irrigation/config   which crop/stage each zone grows, plus each column's own "Firmware save
 *                     settings" library (savedSettings/*). ESP1 does not read this node at all --
 *                     values reach the rig only when the operator fills them into the column
 *                     settings (via a saved setting, "Fill targets from crop profile", or by hand)
 *                     and sends them via SET_COLUMN.
 * irrigation/manual   dashboard -> ESP1 { seq, want }: the Manual/Test hold. ESP1 answers in
 *                     irrigation/live under diagnostics.webManual. Needs its own RTDB rule --
 *                     see Website/firebase-rules.json.
 * irrigation/testHold dashboard -> ESP1 { seq, bit, want }: press-and-hold tests. ESP1 polls it every
 *                     1-3 s, drops the relay 4 s after the last refresh, and caps a hold at 15 s.
 * irrigation/commands dashboard -> ESP1 (ESP1 validates every supported command). ESP1 replies through
 *                     status + detail: queued -> received -> accepted/completed/failed/rejected. The
 *                     detail field holds 47 characters, so longer replies arrive cut off (see
 *                     explainCommandDetail()). "withdrawn" is written only by this page, and only for
 *                     a row still "queued", because ESP1 runs a queued row however old it is.
 *
 * The contract below follows the master's web v.3 firmware (firebaseUploadLive, firebasePollCommands,
 * firebaseCommandTick in ESP1/src/main.cpp). The firmware publishes no version number, so isV3Firmware()
 * tells the generations apart for the few places they differ.
 *
 * Commands ESP1 actually implements, and the exact payload each one expects. Kept complete on
 * purpose -- this drifted out of sync with the real command set once before (only 3 of 14 were
 * listed here for a long stretch).
 *   RUN_PUMP_TEST      { pump: "transfer" | "booster" | "mixer" }   preventive exercise, for the length
 *                        set by SET_EXERCISE (1-10 s). ESP2 raises the inverter first.
 *   FORCE_RUN          { columns: "A"|"B"|"C"|"AB", liters: <=20, doseMl: {A,B,C} (each <=500),
 *                        delaySeconds: 0-300 }   no pH field exists -- see note below. Any dose needs at
 *                        least 6.25 L, and mixing time is fixed by the firmware.
 *   EMERGENCY_STOP     {}   no confirmation dialog, never held back by the rig lock or the cooldown. Since 6 Nov
 *                        2026 it, DISABLE_ACTUATIONS and CANCEL_FORCE are not sent while ESP1 isn't reporting (see
 *                        stopOfflineReason()), and the rules refuse them when ESP1's last report is over 3 minutes old
 *   RECOVER            { action: "hold"|"release"|"irrigate"|"normal" }   only while a fault is held
 *   ESTOP_RECOVER      {}   only while stopped (EMERGENCY_STOP) and no fault is held
 *   ENABLE_ACTUATIONS  {}   the one escape hatch checked first, gated on nothing
 *   DISABLE_ACTUATIONS {}   monitoring keeps running
 *   CANCEL_FORCE       {}   only while a forced run is armed/counting down
 *   ACK_FAULT          {}   dashboard-only snooze; no effect on the rig
 *   SET_COLUMN         { col: "A"|"B"|"C", mode?, enabled?, schedMode?, winStart?, winEnd?, windows?,
 *                        targetN?, targetP?, targetK?, targetPH?, preset? }   every field but col
 *                        is optional; an absent field is left unchanged, never treated as zero. windows
 *                        ([[start, end], ...], 1 to 4 pairs, ESP1 from 5 Nov 2026) replaces winStart/winEnd.
 *                        ESP1 still accepts preset (one of its own CROP_PRESETS names, resolved
 *                        server-side) via the LCD/SMS, but this page no longer offers it -- see
 *                        "Firmware save settings" below, which replaced it with the operator's own
 *                        saved N/P/K/pH+mode+schedule combinations instead of ESP1's fixed 4-name
 *                        table.
 *   SET_EXERCISE       { exerciseEnabled?, exerciseSeconds? }
 *   REBOOT             { target: "nano"|"esp2"|"esp1" }
 *   TEST_PULSE         { target, seconds: 1-15 }   Manual/Test only, dead-man timed on ESP2. Whole
 *                        seconds only: ESP1 reads a decimal as 0, then runs its default 5 s. target servoL /
 *                        servoR only moves the pump selector (the Pump selector switch sends 15 s); ESP1 from
 *                        16 Nov 2026 ends it as soon as the move is settled and answers "SERVO OK: selector on
 *                        the TRANSFER side" (completed) or "SERVO FAIL: <why>" / "SERVO NOT CONFIRMED: ..."
 *                        (failed), and publishes the side the Nano last confirmed as system.servoSide
 *                        ("TRANSFER"|"BOOSTER", absent = never) with servoSideAgeMs (absent = before ESP1's
 *                        last restart).
 *   DIAG_SWEEP         { seconds? }
 *   SET_CLOCK          { clkY, clkMo, clkD, clkH, clkMi }   all required together; sets ESP1's RTC
 *   SET_THRESH         { thStart, thGap }   both required; same clamps as the LCD. Older firmware
 *                        also requires thStop, and web v.3 ignores it.
 *   SET_AC_VOLT        { vMin, vMax }   both required, whole volts, 80-260 with vMin at least 5 under vMax:
 *                        the AC supply window ESP2's PZEM check stops an inverter pump outside of. Privileged
 *                        tier (operators, Sub-operators, 1-hour access; 1-hour access only since 16 Nov 2026),
 *                        in the rules too. ESP1 from 14 Nov 2026
 *                        publishes the window as system.acVoltMin/acVoltMax, plus system.acVoltLast and
 *                        acVoltLastAgeMs once a pump has run; older firmware rejects it as "not a remotely
 *                        safe control". Refused while someone has Settings > AC Voltage open on the LCD.
 *   SET_AC_AMP         { aMin10, aMax10 }   both required, whole TENTHS of an amp (5 = 0.5 A), 1-100 with aMin10
 *                        at least 2 under aMax10: the no-current and overcurrent limits ESP2's PZEM check stops an
 *                        inverter pump outside of. Same accounts as SET_AC_VOLT, in the rules too. ESP1 from 16 Nov
 *                        2026 publishes them as system.acAmpMin10/acAmpMax10, plus system.acAmpLast (amps) and
 *                        acAmpLastAgeMs once a pump has run; older firmware rejects it as "not a remotely safe
 *                        control". Refused while someone has Settings > AC Current open on the LCD.
 *   RESTORE_DEFAULTS   {}   idle-only; runs the same reset function the LCD's Restore Defaults does
 *   CLEAR_BATT_SWITCH  {}   clears a latched battery-switch fault; privileged tier only. Dormant while
 *                        the firmware builds with SECONDARY_BATT_ENABLED 0.
 *   LCD_LOCK           { action: "lock"|"unlock" }   the rig lock: ESP1's own Lock Screen flag, published as
 *                        diagnostics.system.lcdLocked. Privileged tier only, and the row carries byName (the
 *                        sender's own name, checked by the rules). Added to web v.3 on 5 Nov 2026; older firmware
 *                        rejects it as "not a remotely safe control". ESP1 refuses to lock while someone is in its
 *                        LCD menu. While the flag is on, the rules refuse every new row except EMERGENCY_STOP,
 *                        DISABLE_ACTUATIONS, CANCEL_FORCE, ACK_FAULT and LCD_LOCK, and refuse starting a manual
 *                        hold or a hold-to-run press.
 * ESP1 also accepts two legacy names this page does not send: START_PUMP and TOGGLE_MIXER, both of which
 * just run the preventive exercise.
 * Anything else is rejected by ESP1 as "not a remotely safe control", so this page does not offer
 * it. In particular there is no standalone valve command (valves are sequenced inside a work order)
 * and no pH Up/Down dosing target anywhere -- pH is validation-only in the current design; the pH
 * pumps are reachable only via TEST_PULSE's manual, operator-confirmed pulse.
 */

const cropDatabase = {
  pechay: { seedling: { n: 70, p: 35, k: 110, ph: 6.0, ec: 1.0, moisture: 65 }, vegetative: { n: 140, p: 50, k: 210, ph: 6.5, ec: 1.5, moisture: 75 } },
  kangkong: { seedling: { n: 60, p: 30, k: 100, ph: 5.5, ec: 0.8, moisture: 75 }, vegetative: { n: 150, p: 45, k: 220, ph: 6.0, ec: 1.2, moisture: 85 } },
  sitaw: { seedling: { n: 50, p: 40, k: 90, ph: 6.0, ec: 1.0, moisture: 60 }, vegetative: { n: 100, p: 60, k: 160, ph: 6.2, ec: 1.4, moisture: 70 }, flowering: { n: 90, p: 80, k: 200, ph: 6.5, ec: 1.8, moisture: 75 } },
  talong: { seedling: { n: 100, p: 40, k: 110, ph: 5.8, ec: 1.2, moisture: 65 }, vegetative: { n: 190, p: 55, k: 210, ph: 6.2, ec: 2.0, moisture: 70 }, flowering: { n: 160, p: 65, k: 260, ph: 6.4, ec: 2.2, moisture: 75 }, fruiting: { n: 140, p: 70, k: 300, ph: 6.5, ec: 2.4, moisture: 80 } },
  silinglabuyo: { seedling: { n: 90, p: 40, k: 110, ph: 5.8, ec: 1.0, moisture: 65 }, vegetative: { n: 170, p: 50, k: 220, ph: 6.2, ec: 1.8, moisture: 70 }, flowering: { n: 130, p: 65, k: 250, ph: 6.3, ec: 1.8, moisture: 75 }, fruiting: { n: 110, p: 65, k: 290, ph: 6.5, ec: 2.2, moisture: 80 } },
  kamatis: { seedling: { n: 120, p: 50, k: 100, ph: 5.8, ec: 1.2, moisture: 65 }, vegetative: { n: 220, p: 60, k: 180, ph: 6.0, ec: 2.0, moisture: 70 }, flowering: { n: 180, p: 70, k: 250, ph: 6.2, ec: 2.5, moisture: 75 }, fruiting: { n: 160, p: 70, k: 300, ph: 6.5, ec: 2.5, moisture: 80 } },
  basil: { seedling: { n: 60, p: 30, k: 90, ph: 5.5, ec: 0.8, moisture: 60 }, vegetative: { n: 140, p: 45, k: 210, ph: 6.0, ec: 1.4, moisture: 70 } }
};

const readableCropNames = {
  pechay: "Pechay", kangkong: "Kangkong", sitaw: "Sitaw", talong: "Talong",
  silinglabuyo: "Siling Labuyo", kamatis: "Kamatis", basil: "Basil"
};

/* ---- Crop profiles: the crop library (2026-11-05) -------------------------------------------------
 * The two tables above are only the crops the website starts with. Operators and privileged accounts can change
 * or remove them and add their own plants, on Zone profiles (Crop profiles). Those changes live in
 * irrigation/config/crops/{cropKey} and take precedence: a full entry ({name, variety?, notes?, stages}) replaces
 * the built-in crop of the same key or adds a new one, and {removed: true} hides a built-in until it is restored.
 * Stages are kept by key, so a column's saved stage survives renaming it. EC is in mS/cm, moisture in %. ESP1 never
 * reads any of this: a column's own targets only change through SET_COLUMN (Save to rig). */
let cropOverrides = {};                     // irrigation/config/crops, as last read
const CROP_LIMITS = {
  n: [0, 2000], p: [0, 2000], k: [0, 2000], ph: [3, 9], ec: [0, 20], moisture: [0, 100], days: [0, 365]
};
const MAX_STAGES = 10;

function stageLabel(key) {
  return key ? key[0].toUpperCase() + key.slice(1) : "";
}

function builtInCrop(key) {
  const stages = cropDatabase[key];
  if (!stages) return null;
  return {
    key, name: readableCropNames[key] || key, variety: "", notes: "", builtIn: true, changed: false,
    stages: Object.entries(stages).map(([stageKey, t]) => ({
      key: stageKey, name: stageLabel(stageKey), n: t.n, p: t.p, k: t.k, ph: t.ph, ec: t.ec, moisture: t.moisture, days: null, notes: ""
    }))
  };
}

function cropFromStore(key, stored, builtIn) {
  const text = value => (typeof value === "string" ? value : "");
  const stages = Object.entries(stored.stages || {})
    .map(([stageKey, s]) => ({
      key: stageKey, order: Number(s?.order) || 0, name: text(s?.name) || stageLabel(stageKey),
      n: numberOrNull(s?.n), p: numberOrNull(s?.p), k: numberOrNull(s?.k), ph: numberOrNull(s?.ph),
      ec: numberOrNull(s?.ec), moisture: numberOrNull(s?.moisture), days: numberOrNull(s?.days), notes: text(s?.notes)
    }))
    .sort((a, b) => a.order - b.order);
  return {
    key, name: text(stored.name) || key, variety: text(stored.variety), notes: text(stored.notes), builtIn, changed: builtIn,
    stages, updatedAt: Number(stored.updatedAt) || 0, updatedBy: text(stored.updatedBy)
  };
}

// Every crop in use, by name: the built-ins (as changed, unless removed) and the ones added on this website.
function cropLibrary() {
  const list = [];
  Object.keys(cropDatabase).forEach(key => {
    const stored = cropOverrides[key];
    if (stored?.removed) return;
    list.push(stored?.stages ? cropFromStore(key, stored, true) : builtInCrop(key));
  });
  Object.entries(cropOverrides).forEach(([key, stored]) => {
    if (cropDatabase[key] || !stored || stored.removed || !stored.stages) return;
    list.push(cropFromStore(key, stored, false));
  });
  return list.filter(crop => crop.stages.length).sort((a, b) => a.name.localeCompare(b.name));
}
function findCrop(key) {
  return cropLibrary().find(crop => crop.key === key) || null;
}
function removedBuiltIns() {
  return Object.keys(cropDatabase).filter(key => cropOverrides[key]?.removed).map(key => ({ key, name: readableCropNames[key] || key }));
}
function cropLabel(crop) {
  return crop.variety ? `${crop.name} (${crop.variety})` : crop.name;
}

// The crop and stage a column is compared with: its saved choice, or, when that is gone from the list (removed on
// another device, or not loaded yet), the first there is. Only for showing: nothing is written.
function zoneCrop(zone) {
  const library = cropLibrary();
  const crop = library.find(c => c.key === zone.defaultCrop) || library[0] || null;
  const stage = crop ? (crop.stages.find(s => s.key === zone.defaultStage) || crop.stages[0] || null) : null;
  return { crop, stage };
}

// Kept separate from the mutable activeZones below so each zone's OWN original default survives a
// partial reload (see attachDatabaseListeners' zones.once() handler) instead of every never-saved
// zone falling back to one generic crop/name the moment any single zone is ever saved.
const DEFAULT_ZONES = [
  { id: "A", name: "SOIL ZONE A (Precise Node 01)", defaultCrop: "talong", defaultStage: "vegetative" },
  { id: "B", name: "SOIL ZONE B (Precise Node 02)", defaultCrop: "pechay", defaultStage: "vegetative" },
  { id: "C", name: "SOIL ZONE C (Precise Node 03)", defaultCrop: "kamatis", defaultStage: "fruiting" }
];
let activeZones = DEFAULT_ZONES.map(z => ({ ...z }));

let db = null;
let auth = null;
let liveData = {};
let commandData = [];
let firebaseReady = false;
let databaseListenersAttached = false;
let databaseAccessDenied = false;   // the rules refused a listener, which also ended it
let serverOffsetRef = null;
let firebaseLinkRef = null;
let firebaseLinkSeen = false;       // has this page connected yet? Until then, "not connected" is just loading
let firebaseConnected = true;       // this browser's own link to Firebase, which says nothing about ESP1
let lastKnownFresh = false;         // whether the page last drew ESP1 as reporting (see syncFreshness())
let liveRef = null;
let commandsRef = null;
let zonesRef = null;
let cropsRef = null;
let lastCommandAt = 0;
const COMMAND_COOLDOWN_MS = 10000;
// Grant duration for the operator's "temporary Manual/Test + System access" action (see
// isOperator()/canAccessPrivilegedTabs() below) -- must match the upper bound enforced server-side
// in firebase-rules.json's tempAccessUntil validator, or a grant this page requests could be
// rejected as exceeding the rules' own cap.
const TEMP_ACCESS_DURATION_MS = 60 * 60 * 1000;

// Mirrors of the firmware's own bounds (FORCE_MAX_LITERS / FORCE_MAX_DOSE_ML). Kept here so an
// out-of-range request is refused before it is written, rather than round-tripping to the rig
// only to come back "rejected".
const FORCE_MAX_LITERS = 20;
const FORCE_MAX_DOSE_ML = 500;
// Mirrors the firmware's forceBatchOk(): a run that doses needs its batch (the water less the 20 %
// flush) to reach MIXING_TANK_SAFE_MIN (5 L), which is 6.25 L of water. The form steps by 0.1 L, so
// the smallest amount it can actually send is 6.3 L.
const FORCE_MIN_DOSING_LITERS = 6.3;
// The years ESP1's SET_CLOCK accepts (RTC_YEAR_MIN / RTC_YEAR_MAX in the firmware's pure_math.h).
const RTC_YEAR_MIN = 2025;
const RTC_YEAR_MAX = 2099;
// The pump selector's TEST_PULSE targets and the side each one moves the Nano's servo to (pulseRowForTarget() in the
// firmware's pure_math.h: servoL = row 20, servoR = row 21). Up here because commandDetailText() uses it too.
const SERVO_TARGETS = { servoL: "TRANSFER", servoR: "BOOSTER" };

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>'"]/g, char => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[char]));
}

function setText(id, text) {
  const element = document.getElementById(id);
  if (!element) return;
  if (element.classList.contains("reading-value") && rollNumber(element, String(text))) return;
  element.textContent = text;
}

// Visuals (2026-11-05): a reading that changes from one number to another rolls to it instead of jumping. Only between
// two numbers with the same unit and decimals, never from "Unavailable", and not when the phone asks for reduced motion.
const reduceMotionQuery = window.matchMedia?.("(prefers-reduced-motion: reduce)") || null;
const NUMBER_ROLL_MS = 450;
const rollingNumbers = new WeakMap();
function rollNumber(element, text) {
  const running = rollingNumbers.get(element);
  if (running) { cancelAnimationFrame(running); rollingNumbers.delete(element); }
  const shown = element.textContent;
  if (shown === text) return true;
  if (reduceMotionQuery?.matches || document.hidden) return false;
  const parse = value => {
    const match = /^(-?\d+(?:\.(\d+))?)( .+)?$/.exec(value);
    return match ? { value: Number(match[1]), digits: (match[2] || "").length, unit: match[3] || "" } : null;
  };
  const from = parse(shown);
  const to = parse(text);
  if (!from || !to || from.unit !== to.unit || from.digits !== to.digits) return false;
  const started = performance.now();
  const step = now => {
    const t = Math.min(1, (now - started) / NUMBER_ROLL_MS);
    if (t >= 1) { element.textContent = text; rollingNumbers.delete(element); return; }
    const eased = 1 - Math.pow(1 - t, 3);
    element.textContent = `${(from.value + (to.value - from.value) * eased).toFixed(to.digits)}${to.unit}`;
    rollingNumbers.set(element, requestAnimationFrame(step));
  };
  rollingNumbers.set(element, requestAnimationFrame(step));
  return true;
}

function hasValue(value) {
  return value !== undefined && value !== null && value !== "" && !Number.isNaN(Number(value));
}

function numberText(value, digits = 1, unit = "") {
  if (!hasValue(value)) return "Unavailable";
  return `${Number(value).toFixed(digits)}${unit ? ` ${unit}` : ""}`;
}

// ESP1 keeps the AC current limits in whole tenths of an amp (5 = 0.5 A); shown with one decimal, as on the LCD.
function ampsFromTenthsText(tenths) {
  return hasValue(tenths) ? (Number(tenths) / 10).toFixed(1) : "?";
}

function rawText(value, fallback = "Unavailable") {
  return value === undefined || value === null || value === "" ? fallback : String(value);
}

function booleanText(value, yes = "OK", no = "Not OK") {
  if (value === undefined || value === null) return "Unavailable";
  return value ? yes : no;
}

// Number("") is 0, so a blank field would otherwise pass validation as a real value. These fields are
// read by plain buttons, not form submits, so the HTML min/max/step attributes never fire on them.
function numberFromInput(id) {
  const raw = document.getElementById(id)?.value?.trim();
  return raw ? Number(raw) : NaN;
}

// ESP1 publishes signed current as + = discharging. Below half a rounding step there is no direction
// worth claiming.
function signedCurrentText(amps, signed) {
  const text = numberText(amps, 2, "A");
  if (!hasValue(signed) || Math.abs(Number(signed)) < 0.005) return text;
  return `${text} ${Number(signed) > 0 ? "discharging" : "charging"}`;
}

// web v.3 publishes all zeros when the RTC's time cannot be trusted. Older firmware could publish
// whatever a wedged I2C bus returned, with rtcOk still true (seen live: "2165-85-165 165:165").
function deviceTimeText(value) {
  const text = rawText(value);
  const parts = text.match(/^(\d+)-(\d+)-(\d+) (\d+):(\d+)/);
  if (!parts) return text;
  const [month, day, hour, minute] = parts.slice(2).map(Number);
  const valid = month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59;
  return valid ? text : `Invalid reading (${text}): the clock can't be trusted`;
}

// "3 minutes ago", or "Unavailable" when there is no age to give.
function agoText(milliseconds) {
  const age = formatAge(milliseconds);
  return age === "Unavailable" ? age : `${age} ago`;
}

function formatAge(milliseconds) {
  const age = Number(milliseconds);
  if (!Number.isFinite(age) || age < 0 || age === 0xFFFFFFFF) return "Unavailable";
  if (age < 1000) return "under 1 second";
  const count = (amount, unit) => `${amount} ${unit}${amount === 1 ? "" : "s"}`;
  if (age < 60000) return count(Math.floor(age / 1000), "second");
  if (age < 3600000) return count(Math.floor(age / 60000), "minute");
  return count(Math.floor(age / 3600000), "hour");
}

// Firebase's clock, not this PC's. meta.updatedAt and a command's updatedAt are server timestamps, so
// on a PC whose clock runs slow a dead rig's snapshot came out with a negative age and read as fresh,
// which left the controls enabled. A PC clock ~18 hours off has happened on this project (see
// grantTempAccess()). The offset arrives from .info/serverTimeOffset in attachDatabaseListeners().
let serverTimeOffsetMs = 0;
function serverNow() {
  return Date.now() + serverTimeOffsetMs;
}

function snapshotAgeText() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (!stamp) return "Waiting for first snapshot";
  const age = Math.max(0, serverNow() - stamp);
  return `Snapshot received ${formatAge(age)} ago`;
}

// The age at the top of the dashboard, with the pace ESP1 is reporting at (web v.4 of 17 Nov 2026: fast while watched).
function liveAgeText() {
  const base = snapshotAgeText();
  if (!liveData.meta?.updatedAt || !deviceIsFresh()) return base;
  const every = Math.round(Number(liveData.meta.refreshMs || 60000) / 1000);
  return liveData.meta.live ? `${base} · live, every ${every} s` : `${base} · every ${every} s`;
}

function deviceIsFresh() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const refreshMs = Number(liveData.meta?.refreshMs || 60000);
  return Boolean(stamp) && serverNow() - stamp < Math.max(refreshMs * 2 + 15000, 90000);
}

// The stops (2026-11-06, the owner's choice): Emergency stop, switching pumps and valves off, and Cancel forced run
// are never queued while ESP1 isn't reading the queue. A stop waiting there fires whenever ESP1 next reads it, which
// can be hours later and into a different situation, while the page looks as if the rig has been told. So while ESP1
// isn't reporting, or this browser has lost Firebase, those buttons are off and say how else to stop the rig. The
// rules refuse a stop once ESP1's last report is over 3 minutes old; deviceIsFresh() gives up within 2.25 minutes
// (ESP1 reports at least once a minute), so the page always says so first.
const STOP_ELSEWHERE = "Use the physical cutoff, MODE + BACK on the unit, or SMS STOP,ALL.";
function stopOfflineReason() {
  if (!firebaseConnected) return `This browser is offline, so a stop can't be sent from here. ${STOP_ELSEWHERE}`;
  if (!deviceIsFresh()) return `ESP1 isn't reporting, so a stop can't be sent from here. ${STOP_ELSEWHERE}`;
  return "";
}

// The firmware publishes no version number, so its generation is inferred from a field that only
// web v.3 sends, and sends unconditionally (system.boardSupply in firebaseUploadLive).
function isV3Firmware() {
  return typeof liveData.system?.boardSupply === "string";
}

// For a value only web v.3 publishes. Older firmware cannot report it at all, which is a different
// situation from v.3 leaving it out because that sensor isn't answering right now ("Unavailable").
function v3Only(text) {
  return liveData.system && !isV3Firmware() ? "Not reported by this firmware" : text;
}

function currentUserIsSignedIn() {
  return Boolean(firebaseReady && auth?.currentUser);
}

/* User approval -----------------------------------------------------------------------------
 * A signed-in account is not automatically allowed to touch hardware -- see /users/{uid} in
 * firebase-rules.json. This constant and these two helpers are for UI purposes only (showing/
 * hiding the pending banner and the User Management tab, greying out buttons); the REAL
 * enforcement is the rules themselves, which check the identical literal UID server-side. */
// The operator accounts, granted identically by firebase-rules.json's own matching literals. Client-side use is
// UI-only (which tab/actions to show); the real enforcement is server-side, keyed on these same UIDs. Only the first
// is ever shown on this page: the other is never listed, named or mentioned anywhere a person can see.
const OPERATOR_UIDS = ["KaiqrwlOHeVdebbokPxZfcFLZiu2", "C8V39t4EDgUgjOt9GA6OeMukMsw2"];
const BACKUP_OPERATOR_UID = "C8V39t4EDgUgjOt9GA6OeMukMsw2";
// Same literal UID firebase-rules.json's ESP1-only branches use -- referenced here only for the
// User Management defense-in-depth exclusion (renderUserManagement()), never for any gating logic.
const ESP1_DEVICE_UID = "6hNg56ldAaSmpnodG1Xla0vn61O2";
let myAccountStatus = undefined; // undefined = not loaded yet, null = no record, else the status string
let myAccountName = "";          // the name on this account's own record, sent with a rig lock (see myDisplayName())
let myAccountRef = null;
// Deliberately NOT compared against lastSignInTime -- that raced against how fast the SDK updates
// it on a fresh sign-in and could re-kick the same session in a loop (see attachUserStatusListener).
// This just remembers, per page load, the highest kickToken this client has already acted on.
let lastHandledKickToken = 0;

function isOperator() {
  return Boolean(auth?.currentUser && OPERATOR_UIDS.includes(auth.currentUser.uid));
}
function isApprovedUser() {
  return isOperator() || myAccountStatus === "approved";
}

// A normal approved operator can be granted TEMPORARY access to Manual/Test + System by an operator
// (User Management > "Grant 1h Manual/Test + System", see renderUserManagement()) -- a time-bounded
// elevation stored as /users/{uid}/tempAccessUntil (a millisecond timestamp), enforced server-side
// in firebase-rules.json's commands/$commandId write rule, not just here. isOperator() itself is
// deliberately UNTOUCHED and still means only the two fixed accounts everywhere else (User
// Management visibility, granting/revoking this very grant, and the rules' own operator-only
// branches) -- canAccessPrivilegedTabs() is the one broadened check, used only for Manual/Test +
// System tab visibility and the commands they send.
let myTempAccessUntil = 0;
// tempAccessUntil is Firebase's time (grantTempAccess() sets it from the server clock), so compare it with
// Firebase's time too. A PC clock 18 hours slow kept an hour's grant open on this page all day.
function hasTempAccess() {
  return serverNow() < myTempAccessUntil;
}
// Sub-operator (2026-09-10): a SECOND, separate elevation from the 1h temp grant above -- persistent
// (no expiry clock) until an operator explicitly withholds it again, same on/off spirit as e.g. the
// preventive-exercise toggle. Stored as /users/{uid}/subOperator (boolean), settable only by either
// operator UID (see firebase-rules.json) -- every operator account can grant/withhold
// it, identically to how both can already grant/revoke temp access.
let mySubOperator = false;
function canAccessPrivilegedTabs() {
  // Deep-scan finding (2026-09-10): hasTempAccess() only checks the timestamp, not current standing --
  // an operator restricting/blocking this account mid-grant left it privileged until the grant's own
  // clock ran out (up to an hour), even though revoking approval is clearly meant to take that away
  // immediately. isApprovedUser() re-check closes that; isOperator() itself needs no such re-check,
  // since operator status is fixed for the session, never revoked mid-session. mySubOperator shares
  // the identical re-check for the identical reason -- a persistent grant must be just as immediately
  // voidable as a temporary one the moment approval is lost.
  return isOperator() || ((hasTempAccess() || mySubOperator) && isApprovedUser());
}
// The AC voltage and current limits (SET_AC_VOLT, SET_AC_AMP). From 14 Nov 2026 they were for operators and
// Sub-operators only; on 16 Nov 2026 the owner opened them to 1-hour access too, so they now follow the rest of the
// System tab. firebase-rules.json allows the same accounts.
function canSetAcVoltage() {
  return canAccessPrivilegedTabs();
}

// Access levels (2026-11-05, the owner's design). An approved account on its own can only look: the Live dashboard,
// Zone profiles (read-only) and Help. Two separate grants, each asked for on the website and given or taken back by
// an operator, open the rest:
//   Control    (users/{uid}/controlAccess):  the Controls and Diagnostics tabs, the Emergency stop, every rig command
//                                            except a column's own settings.
//   Edit zones (users/{uid}/zoneEditAccess): the column cards (settings, watering times, targets, on/off, crop, name).
// Sub-operators, 1-hour access and operators have both, plus Manual/Test, System, the rig lock and crop profiles.
// firebase-rules.json enforces the same split; these only decide what the page offers.
let myControlAccess = false;
let myZoneEditAccess = false;
function hasControlAccess() {
  return canAccessPrivilegedTabs() || (isApprovedUser() && myControlAccess);
}
function hasZoneEditAccess() {
  return canAccessPrivilegedTabs() || (isApprovedUser() && myZoneEditAccess);
}
const NO_CONTROL_TITLE = "Needs control access. Ask an operator for it on the Controls tab.";
const NO_ZONE_EDIT_TITLE = "Needs access to edit zone profiles. Ask an operator for it at the top of Zone profiles.";

// Access-control revision (2026-09-09): renderZonesUI()'s editable-vs-read-only variant depends on
// isApprovedUser(), which can change mid-session (operator approves/restricts/blocks an account
// while its tab stays open -- see Part 6 of the access-control spec). Re-rendering on every single
// syncControlAvailability() tick (called every 15s and on every live-data push) would be wasteful
// and would fight the capture/restore form-state machinery for no reason, so this only re-renders
// on an actual flip, tracked here. Called from attachUserStatusListener()'s status callback (the
// only thing that can change isApprovedUser() while isOperator() stays fixed for the session).
let lastZoneEditCapability = undefined;
function syncZoneEditCapability() {
  const canEdit = hasZoneEditAccess();
  if (canEdit === lastZoneEditCapability) return;
  lastZoneEditCapability = canEdit;
  renderZonesUI();
}

function setDeviceStatus(id, text, tone = "off") {
  const element = document.getElementById(id);
  if (!element) return;
  element.textContent = text;
  element.className = `device-status ${tone}`;
}

function setConnection(connected, label) {
  const dot = document.getElementById("connectionDot");
  if (dot) dot.style.backgroundColor = connected ? "var(--success)" : "var(--danger)";
  setText("connectionStatus", label);
  setDeviceStatus("sideConnection", connected ? "LIVE SNAPSHOT" : "NOT LIVE", connected ? "active" : "off");
}

/* Toast notifications (2026-09-10) -- a popup layer on top of (not instead of) setCommandStatus()'s
 * own persistent inline text: that stays the readable, still-there-later record; this is the "did you
 * see that" moment. Bottom-right on desktop, top on mobile (see .toast-stack in style.css).
 * Stacking rule: one toast gets the full TOAST_BASE_MS. Every additional toast CURRENTLY on screen
 * shortens EVERY toast's remaining time (recomputed on every add/remove), down to a TOAST_MIN_MS
 * floor, so a burst of notifications drains the stack rather than each sitting for a full 10s. */
const TOAST_BASE_MS = 10000;
const TOAST_MIN_MS = 2500;
let toastSeq = 0;
const activeToasts = new Map();   // id -> { el, timer }

function toastDurationForCount(count) {
  return Math.max(TOAST_MIN_MS, Math.round(TOAST_BASE_MS / Math.max(1, count)));
}

function rescheduleToasts() {
  const duration = toastDurationForCount(activeToasts.size);
  activeToasts.forEach((entry, id) => {
    clearTimeout(entry.timer);
    entry.timer = setTimeout(() => dismissToast(id), duration);
  });
}

function dismissToast(id) {
  const entry = activeToasts.get(id);
  if (!entry) return;
  clearTimeout(entry.timer);
  activeToasts.delete(id);
  entry.el.classList.remove("toast-in");
  entry.el.classList.add("toast-leaving");
  // Deep-scan finding (2026-09-10, low): transitionend never fires for an effectively-zero-duration
  // transition (e.g. a viewer with reduced-motion/animations forced off at the OS or browser level),
  // which would otherwise leave this element in the DOM forever -- activeToasts itself is already
  // cleaned up above, so this was a silent DOM leak, not a functional one. A fallback removal timer
  // races it; whichever fires first wins, the other is a harmless no-op on an already-detached node.
  const remove = () => entry.el.remove();
  entry.el.addEventListener("transitionend", remove, { once: true });
  setTimeout(remove, 300);
  rescheduleToasts();   // fewer toasts left now -- the remaining ones can afford to slow back down
}

// onClick (optional): what a click on the toast does as well as dismissing it, e.g. open the tab it is about.
function showToast(message, tone = "", onClick = null) {
  const text = String(message || "").trim();
  if (!text) return null;
  const stack = document.getElementById("toastStack");
  if (!stack) return null;
  const id = ++toastSeq;
  const el = document.createElement("div");
  el.className = `toast${tone ? ` toast-${tone}` : ""}`;
  el.textContent = text;
  el.title = onClick ? "Click to open" : "Click to dismiss";
  el.addEventListener("click", () => { dismissToast(id); if (onClick) onClick(); });
  stack.appendChild(el);
  activeToasts.set(id, { el, timer: null });
  // Force layout before adding the "in" class, or the transition never plays -- the browser would
  // otherwise coalesce "append with opacity:0" and "immediately set opacity:1" into one paint with no
  // visible transition at all.
  requestAnimationFrame(() => el.classList.add("toast-in"));
  rescheduleToasts();
  return id;
}

function setCommandStatus(text, tone = "") {
  const element = document.getElementById("commandStatus");
  if (element) {
    element.textContent = text;
    element.className = `command-status ${tone}`.trim();
  }
  return showToast(text, tone);
}

/* What ESP1 publishes that stops it starting something new. ESP1 checks again itself: these only keep the page
 * from offering a control that is bound to fail, and say why. */
const STATE_BUSY_TEXT = {
  ACTIVE_STATE: "a run or pump test is in progress",
  TEST_MODE: "a timed pulse or hold-to-run test is running",
  STARTUP_SYNC: "ESP1 is re-checking ESP2 after a restart or a stop",
  RECOVERY_STATE: "ESP1 is recovering its link to ESP2",
  BOOT_STATE: "ESP1 is still starting up"
};
let forceArmedShown = false;    // the "Forced run armed" panel is up (see updateForceArmed())

function rigBusyReason() {
  const d = liveData.diagnostics || {};
  const state = liveData.system?.state || "";
  if (rigLocked()) return "the rig is locked";
  if (actuationsAreOff()) return "actuations are disabled (pumps and valves are switched off)";
  if (d.fault?.held) return "a fault is held";
  if (state === "EMERGENCY_STOP") return "the emergency stop is active";
  // Not runProgress.active: that stays true while the LCD shows a finished run's receipt ("finishing"), when
  // ESP1 is idle again and accepts everything. These two are what its gates actually check.
  if (d.system?.workOrderActive || d.system?.pendingRun) return "a run is in progress";
  if (forceArmedShown) return "a forced run is armed";
  return "";
}

// FORCE_RUN's own gate in firebaseCommandTick(): idle, nothing armed, no pump test. Its Manual/Test check is left
// to ESP1, which refuses with a clear reason: the snapshot ESP1 forces when a hold is released can still carry
// "held" (see the note for the master), and the next one is a minute away, so checking it here would keep
// this button off for up to a minute after leaving Manual/Test.
function forceRunBlockReason() {
  const busy = rigBusyReason();
  if (busy) return busy === "a forced run is armed" ? "another forced run is already armed" : busy;
  const d = liveData.diagnostics || {};
  if (d.actuator?.pumpTestActive) return "a pump test is running";
  const state = liveData.system?.state || "";
  if (state && state !== "IDLE_STATE") return STATE_BUSY_TEXT[state] || `ESP1 is not idle (${state})`;
  return "";
}

// A hold has no command row, so ESP1 can't refuse one out loud: pulseTick() just never starts the relay, and
// the page went on to report the press as if it had run. TEST_MODE and ACTIVE_STATE alone don't count here,
// because this page's own pulse, hold or pump test causes them, and ESP1's report that it has finished can be
// a minute away. target "" asks about the rig as a whole.
function holdBlockReason(target) {
  const busy = rigBusyReason();
  if (busy) return busy;
  const state = liveData.system?.state || "";
  if (["STARTUP_SYNC", "RECOVERY_STATE", "BOOT_STATE"].includes(state)) return STATE_BUSY_TEXT[state];
  const column = /^col([ABC])$/.exec(target || "")?.[1];
  if (column && liveData.sensors?.zones?.[column]?.enabled === false) return `column ${column} is disabled on ESP1`;
  return "";
}

// ESP1 sends no report when actuations are switched on or off, so its next one can be a minute away. Its reply to
// the command says the same sooner, so the newer of the two counts, as for a column's settings (zoneSettings()).
function actuationsAreOff() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const reply = commandData
    .filter(c => (c.type === "ENABLE_ACTUATIONS" || c.type === "DISABLE_ACTUATIONS") && c.status === "completed"
                 && Number(c.updatedAt || 0) + ZONE_CONFIRM_SETTLE_MS > stamp)
    .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))[0];
  return reply ? reply.type === "DISABLE_ACTUATIONS" : Boolean(liveData.diagnostics?.actuationsDisabled);
}

/* ---- Rig lock (2026-11-05) ---------------------------------------------------------------------------
 * One lock for this website and the rig's own buttons: ESP1's Lock Screen flag (lcdLocked), published in every
 * snapshot. LCD_LOCK sets it from here; Settings > Lock Screen and the UP+DOWN unlock set it at the rig. While it
 * is on, the rules refuse every command but the stops (and the snooze), and refuse starting manual mode or a hold,
 * reading the same flag from the same snapshot, so the page switches those controls off and says why. Operators and
 * privileged accounts can lock and unlock. Automatic watering is not affected: the lock only gates controls. */
const LOCK_EXEMPT = new Set(["EMERGENCY_STOP", "DISABLE_ACTUATIONS", "CANCEL_FORCE", "ACK_FAULT", "LCD_LOCK"]);
const LOCKED_TITLE = "The rig is locked. Unlock it first: operators, and accounts with 1-hour access or Sub-operator, can.";
// A snapshot can carry state from a moment before its own stamp (see ZONE_CONFIRM_SETTLE_MS).
const LOCK_SETTLE_MS = 3000;
// ESP1 forces a report after LCD_LOCK, so the switch stops saying "Locking…" well before this.
const LOCK_WAIT_MAX_MS = 90000;
let lockSentKey = null;       // the LCD_LOCK row this page is waiting on
let lockSentWant = null;      // what it asked for: true = lock
let lockSeenOpenAt = 0;       // the newest snapshot stamp that showed the rig unlocked

function rigLocked() {
  return liveData.diagnostics?.system?.lcdLocked === true;
}
function lockKnown() {
  return typeof liveData.diagnostics?.system?.lcdLocked === "boolean";
}
// The name the rules accept with a lock: the account's own, or "Operator" for an operator account.
function myDisplayName() {
  return isOperator() ? "Operator" : myAccountName;
}

// "Locking…" or "Unlocking…" while a request from this page hasn't shown up in ESP1's report yet.
function lockWaitingText() {
  if (!lockSentKey) return "";
  const row = commandData.find(c => c.id === lockSentKey);
  const done = lockKnown() && rigLocked() === lockSentWant;
  const gaveUp = row && (["rejected", "failed", "withdrawn"].includes(row.status)
    || serverNow() - Number(row.updatedAt || row.requestedAt || 0) > LOCK_WAIT_MAX_MS);
  if (done || gaveUp) { lockSentKey = null; return ""; }
  return lockSentWant ? "Locking…" : "Unlocking…";
}

function lockTimeText(at) {
  const when = new Date(at);
  const today = when.toDateString() === new Date(serverNow()).toDateString();
  return today ? `at ${when.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}`
    : `on ${when.toLocaleString([], { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" })}`;
}

// Who locked it. ESP1 doesn't say, so this goes by the newest lock sent from this website: it counts unless a report
// since has shown the rig unlocked, in which case the lock now on was set at the rig.
function lockSourceText() {
  const row = commandData
    .filter(c => c.type === "LCD_LOCK" && c.status === "completed")
    .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))[0];
  const at = Number(row?.updatedAt || 0);
  if (row?.payload?.action === "lock" && lockSeenOpenAt <= at + LOCK_SETTLE_MS) {
    return `Locked from this website${row.byName ? ` by ${row.byName}` : ""} ${lockTimeText(at)}.`;
  }
  return "Locked at the rig, from its Settings menu (Lock Screen).";
}

function renderRigLock() {
  const known = lockKnown();
  const locked = rigLocked();
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (known && !locked && stamp > lockSeenOpenAt) lockSeenOpenAt = stamp;
  const waiting = lockWaitingText();
  const privileged = canAccessPrivilegedTabs();
  const block = !privileged ? "Only operators, and accounts with 1-hour access or Sub-operator, can lock or unlock the rig."
    : !known ? "ESP1 hasn't reported its lock yet."
    : waiting ? "Waiting for ESP1's report."
    : !firebaseConnected ? "This browser has lost its connection to Firebase."
    : !deviceIsFresh() ? "ESP1 isn't reporting, so it can't lock or unlock right now."
    : "";
  const word = waiting || (!known ? "Unknown" : locked ? "Locked" : "Unlocked");
  ["lockSwitch", "sysLockSwitch"].forEach(id => {
    const toggle = document.getElementById(id);
    if (!toggle) return;
    toggle.setAttribute("aria-checked", String(known && locked));
    toggle.disabled = Boolean(block);
    toggle.title = block;
  });
  setText("lockState", word);
  setText("sysLockStatus", known ? word : "Unavailable");
  setDeviceStatus("sideLock", word.toUpperCase(), known && locked ? "warn" : "off");
  const banner = document.getElementById("lockBanner");
  if (banner) {
    const show = known && locked && currentUserIsSignedIn() && isApprovedUser();
    banner.hidden = !show;
    if (show) setText("lockDetail", lockSourceText());
  }
  const unlock = document.getElementById("unlockBtn");
  if (unlock) {
    unlock.hidden = !privileged;
    unlock.disabled = Boolean(block);
    unlock.title = block;
  }
}

function toggleRigLock() {
  if (!canAccessPrivilegedTabs() || !lockKnown() || lockSentKey) return;
  const lock = !rigLocked();
  const question = lock
    ? "Lock the rig? Controls on this website switch off for everyone, and the buttons on the rig's screen lock too. Automatic watering carries on, and Emergency stop still works. At the rig, UP and DOWN held together unlock it."
    : "Unlock the rig? Controls on this website and the buttons on the rig's screen work again.";
  if (!confirm(question)) return;
  const name = myDisplayName();
  sendFromPanel(resultLine("sysLockResult"), "LCD_LOCK", { action: lock ? "lock" : "unlock" }, { operatorOnly: true, byName: name || undefined })
    .then(key => {
      if (!key) return;
      lockSentKey = key;
      lockSentWant = lock;
      renderRigLock();
    });
}

/* ---- Pumps and valves switch (Controls) ---------------------------------------------------------------
 * ESP1's actuation lockout as an ON/OFF switch, like a column's. "On" means allowed to run. Its position follows
 * ESP1 (its report, or its reply, whichever is newer), never just the click. */
let actSentKey = null;
let actSentAt = 0;

function renderActuationSwitch() {
  const toggle = document.getElementById("actSwitch");
  const known = Boolean(liveData.diagnostics) && typeof liveData.diagnostics.actuationsDisabled === "boolean";
  const off = actuationsAreOff();
  const row = actSentKey && commandData.find(c => c.id === actSentKey);
  // The row shows up here the moment it is written, so one missing for longer has been removed: stop waiting.
  const waiting = Boolean(actSentKey) && (row ? row.status === "queued" || row.status === "received" : Date.now() - actSentAt < 10000);
  if (actSentKey && !waiting) actSentKey = null;
  setText("actState", waiting ? "Sending…" : !known ? "--" : off ? "Off" : "On");
  setText("actStateText", !known ? "Waiting for ESP1's report."
    : off ? "Off: nothing runs until someone switches it back on."
    : "On: the schedule, forced runs and tests can run.");
  if (!toggle) return;
  toggle.setAttribute("aria-checked", String(known && !off));
  const signedIn = currentUserIsSignedIn();
  // Off is a stop: it works while the rig is locked, but like every stop it is never queued while ESP1 isn't
  // reporting (stopOfflineReason()). Back on is not a stop, so the rig lock holds it.
  const block = !signedIn ? "Sign in to use the system controls."
    : !hasControlAccess() ? (isApprovedUser() ? NO_CONTROL_TITLE : "Your account is awaiting operator approval before it can use this control.")
    : !known ? "ESP1 hasn't reported this yet."
    : waiting ? "Waiting for ESP1."
    : !off ? stopOfflineReason()
    : rigLocked() ? LOCKED_TITLE
    : "";
  toggle.disabled = Boolean(block);
  toggle.title = block;
}

function toggleActuations() {
  if (actSentKey) return;
  const show = resultLine("actResult");
  if (!actuationsAreOff()) {
    if (!confirm("Switch pumps and valves off? Any run in progress stops at once, and nothing runs again until someone switches them back on. Readings and logging carry on.")) { show("Nothing was sent."); return; }
    sendFromPanel(show, "DISABLE_ACTUATIONS", {}, { emergency: true }).then(waitForActuationReply);
    return;
  }
  if (!confirm("Switch pumps and valves back on? The schedule, forced runs and exercises can start again.")) { show("Nothing was sent."); return; }
  sendFromPanel(show, "ENABLE_ACTUATIONS", {}, { emergency: true }).then(waitForActuationReply);
}

function waitForActuationReply(key) {
  if (!key) return;
  actSentKey = key;
  actSentAt = Date.now();
  renderActuationSwitch();
}

function syncControlAvailability() {
  const signedIn = currentUserIsSignedIn();
  // Control access (2026-11-05): an approved account only looks until an operator gives it Control. It is ANDed
  // into every gate below, the same as approval was, rather than only checked at queueCommand() time.
  const control = hasControlAccess();
  const notApprovedTitle = !isApprovedUser() ? "Your account is awaiting operator approval before it can use this control." : NO_CONTROL_TITLE;
  // The rig lock: the rules refuse everything but the stops while it is on, so those controls are off too.
  const locked = rigLocked();
  const normalAllowed = signedIn && control && deviceIsFresh() && firebaseConnected && !locked;
  const normalTitle = (signedIn && !control) ? notApprovedTitle : locked ? LOCKED_TITLE
    : "Sign in and wait for a fresh ESP1 snapshot before using this control.";
  renderRigLock();
  renderActuationSwitch();
  // Every actuating control needs a fresh snapshot: ESP1 will refuse anything that arrives while it
  // is not idle, and without live data the page cannot tell "idle" from "device offline".
  // The pulse and the ESP2 sweep both reach the rig, so they belong with the other actuating
  // controls rather than looking alive and then failing inside queueCommand().
  ["transferPumpBtn", "boosterPumpBtn", "mixerBtn", "pulseBtn", "mtSweepBtn", "sweepBtn", "exSaveBtn", "sysClockSendBtn", "sysThreshSendBtn"].forEach(id => {
    const button = document.getElementById(id);
    if (!button) return;
    button.disabled = !normalAllowed;
    button.title = normalAllowed ? "" : normalTitle;
  });
  // Hold-to-run buttons: same normalAllowed gate as every other actuating Manual/Test control above.
  // If the rig drops out of "fresh" or the account loses approval WHILE a button is physically held,
  // the press must not keep silently running -- mtHoldStop() releases it immediately, same as losing
  // the Manual/Test hold itself does in renderManualHold().
  // A button ESP1 would ignore is off as well, with the reason beside the buttons (see holdBlockReason()) --
  // but never the one being held: a disabled button doesn't receive its own release.
  const holdAllowed = normalAllowed || (locked && signedIn && control && deviceIsFresh() && firebaseConnected);
  const holdTitle = (signedIn && !control) ? notApprovedTitle : "Sign in and wait for a fresh ESP1 snapshot before using a hold-to-run test.";
  // While locked, holdBlockReason() says so ("the rig is locked"), so the note below explains the buttons being off.
  document.querySelectorAll(".hold-btn").forEach(button => {
    const reason = holdAllowed ? holdBlockReason(button.dataset.holdTarget) : "";
    button.disabled = !holdAllowed || (Boolean(reason) && button !== mtHoldActiveBtn);
    button.title = !holdAllowed ? holdTitle : reason ? `Not available right now: ${reason}.` : "";
  });
  const holdNote = document.getElementById("mtHoldBlocked");
  if (holdNote) {
    const general = holdAllowed ? holdBlockReason("") : "";
    const offColumns = normalAllowed && !general ? ["A", "B", "C"].filter(id => liveData.sensors?.zones?.[id]?.enabled === false) : [];
    // A column that is simply switched off (C, on this rig) is a standing fact, not a problem, so it reads as a
    // plain note rather than an error.
    holdNote.textContent = general ? `ESP1 won't start a hold right now: ${general}.${general === "the rig is locked" ? " Unlock it first." : ""}`
      : offColumns.length > 1 ? `ESP1 has columns ${offColumns.join(" and ")} disabled, so their hold buttons are off.`
      : offColumns.length ? `ESP1 has column ${offColumns[0]} disabled, so its hold button is off.`
      : "";
    holdNote.className = general ? "control-result error" : "field-note";
    holdNote.hidden = !holdNote.textContent;
  }
  // ESP1 has already dropped the relay for these, and ignores every keep-alive until the button comes up. While
  // the rig is locked the rules refuse the keep-alives too.
  const fd = liveData.diagnostics || {};
  if (!normalAllowed || actuationsAreOff() || fd.fault?.held || liveData.system?.state === "EMERGENCY_STOP") mtHoldStop(true);

  // Zone profiles: each column's ON/OFF switch and Save to rig reach the rig, so they share the same gate.
  activeZones.forEach(zone => { renderZonePower(zone.id); syncZoneSaveButton(zone.id); });

  const forceButton = document.querySelector("#forceRunForm button[type=submit]");
  if (forceButton) {
    // ESP1 refuses a forced run unless the rig is idle (FORCE_RUN in firebaseCommandTick()). Checked here as
    // well, from what ESP1 publishes, so a request bound to be refused isn't sent, and the reason shows beside
    // the button instead of only on hover. It can't see everything ESP1 checks (someone in the LCD menu, the
    // mixing-tank check, a low reservoir), so ESP1's own reply still has the last word. A locked rig is one of
    // the reasons (rigBusyReason()), so it is said beside the button too.
    const reason = holdAllowed ? forceRunBlockReason() : "";
    forceButton.disabled = !holdAllowed || Boolean(reason);
    forceButton.title = !holdAllowed
      ? ((signedIn && !control) ? notApprovedTitle : "Sign in and wait for a fresh ESP1 snapshot before forcing a run.")
      : reason ? `ESP1 would refuse a forced run right now: ${reason}.` : "";
    const note = document.getElementById("forceRunBlocked");
    if (note) {
      note.textContent = reason ? `Not available right now: ${reason}.` : "";
      note.hidden = !reason;
    }
  }

  // Emergency stop ignores the rig lock and the normal cooldown, but not Control access: an account without it gets
  // no stop button at all (the owner's choice, 5 Nov 2026), and the rules refuse its EMERGENCY_STOP too. Nor is it
  // queued while ESP1 isn't reporting (6 Nov 2026, see stopOfflineReason()): the buttons are off then, and the notes
  // say how else to stop the rig. There are two copies: the Controls panel, and the bar on every tab.
  const stopBlocked = stopOfflineReason();
  document.querySelectorAll(".estop-btn").forEach(emergency => {
    emergency.disabled = !signedIn || !control || Boolean(stopBlocked);
    emergency.title = !signedIn ? "Sign in before sending an emergency-stop request." : !control ? notApprovedTitle : stopBlocked;
  });
  const estopBar = document.getElementById("estopBar");
  if (estopBar) estopBar.hidden = !signedIn || !control;
  const estopNote = document.getElementById("emergencyOfflineNote");
  if (estopNote) {
    estopNote.hidden = !signedIn || !control || !stopBlocked;
    if (stopBlocked) estopNote.textContent = stopBlocked;
  }
  // The bar is on every tab, so it carries a short form of the same note.
  const barNote = document.getElementById("estopBarNote");
  if (barNote) {
    barNote.hidden = !signedIn || !control || !stopBlocked;
    barNote.textContent = !firebaseConnected ? "Offline: stop it at the unit (MODE + BACK) or by SMS STOP,ALL."
      : "ESP1 isn't reporting: stop it at the unit (MODE + BACK) or by SMS STOP,ALL.";
  }
  // This page can't see why ESP1 stopped reporting, but the LCD can, so say where to look.
  const offlineHelp = document.getElementById("esp1OfflineHelp");
  if (offlineHelp) offlineHelp.hidden = !signedIn || !control || deviceIsFresh();

  // Recovery + reboot: signed-in AND Control, deliberately NOT freshness-gated. A stale snapshot is often
  // exactly why you are reaching for these. None of them is a stop, so the rig lock switches them off. The
  // pumps-and-valves switch has its own gate (renderActuationSwitch()).
  ["rebootNanoBtn2", "rebootEsp2Btn2", "rebootEsp1Btn2", "estopRecoverBtn", "enableActBtn"].forEach(id => {
    const b = document.getElementById(id);
    if (!b) return;
    b.disabled = !signedIn || !control || locked;
    b.title = !signedIn ? "Sign in to use the system controls." : !control ? notApprovedTitle : locked ? LOCKED_TITLE : "";
  });
  document.querySelectorAll("#faultRecovery button[data-recover]").forEach(b => {
    b.disabled = locked || !control;
    b.title = !control ? notApprovedTitle : locked ? LOCKED_TITLE : "";
  });

  // Restore Defaults: signed-in + approved, like the reboot buttons, PLUS idle-gated -- ESP1
  // refuses this one outright unless sysState is IDLE_STATE, matching the LCD's own gate.
  const restoreBtn = document.getElementById("sysRestoreDefaultsBtn");
  if (restoreBtn) {
    // SAFE_MODE is only ESP1's label for idle while locked out, so ESP1 accepts this then too.
    const notIdle = liveData.system?.state && !["IDLE_STATE", "SAFE_MODE"].includes(liveData.system.state);
    restoreBtn.disabled = !signedIn || !control || Boolean(notIdle) || locked;
    restoreBtn.title = !signedIn ? "Sign in to use the system controls."
                      : !control ? notApprovedTitle
                      : locked ? LOCKED_TITLE
                      : notIdle ? "ESP1 must be idle to restore defaults, same as the LCD menu."
                      : "";
  }

  // Nothing to clear unless ESP1 reports the fault as latched.
  const clearBattBtn = document.getElementById("sysClearBattSwitchBtn");
  if (clearBattBtn) {
    const latched = liveData.system?.battSwitchFault === true;
    clearBattBtn.disabled = !normalAllowed || !latched;
    clearBattBtn.title = !normalAllowed ? normalTitle
                        : !latched ? "ESP1 reports no battery-switch fault to clear."
                        : "";
  }

  // AC voltage limits: the privileged tier, and only once ESP1 reports the setting (firmware from
  // 14 Nov 2026). Older firmware would refuse it as "not a remotely safe control".
  const acVoltBtn = document.getElementById("sysAcVoltSendBtn");
  if (acVoltBtn) {
    const allowed = canSetAcVoltage();
    const reported = hasValue(liveData.system?.acVoltMin) && hasValue(liveData.system?.acVoltMax);
    acVoltBtn.disabled = !normalAllowed || !allowed || !reported;
    acVoltBtn.title = !normalAllowed ? normalTitle
                    : !allowed ? "For operators, Sub-operators and 1-hour access."
                    : !reported ? "ESP1's firmware doesn't have this setting yet. Update ESP1 and ESP2 first."
                    : "";
  }
  // AC current limits (2026-11-16): the same people, and only once ESP1 reports them (firmware from 16 Nov 2026).
  const acAmpBtn = document.getElementById("sysAcAmpSendBtn");
  if (acAmpBtn) {
    const allowed = canSetAcVoltage();
    const reported = hasValue(liveData.system?.acAmpMin10) && hasValue(liveData.system?.acAmpMax10);
    acAmpBtn.disabled = !normalAllowed || !allowed || !reported;
    acAmpBtn.title = !normalAllowed ? normalTitle
                   : !allowed ? "For operators, Sub-operators and 1-hour access."
                   : !reported ? "ESP1's firmware can't take this from the website yet. Update ESP1 first (16 Nov 2026)."
                   : "";
  }
  // The pump selector switch on Manual/Test: the same gate as the timed test's button.
  renderServoSwitch(normalAllowed, normalTitle);
}

function initializeFirebase() {
  const config = window.FIREBASE_CONFIG;
  if (!config || !config.databaseURL || config.apiKey === "PASTE_YOUR_API_KEY") {
    setConnection(false, "Firebase not configured");
    return;
  }
  try {
    if (!firebase.apps.length) firebase.initializeApp(config);
    db = firebase.database();
    auth = firebase.auth();
    firebaseReady = true;
    auth.onAuthStateChanged(user => {
      const loginScreen = document.getElementById("loginScreen");
      const logoutButton = document.getElementById("logoutBtn");
      const currentUserEmailEl = document.getElementById("currentUserEmail");
      if (!user) {
        detachDatabaseListeners();
        detachUserStatusListener();
        detachUserManagementListener();
        // Any live Manual/Test keep-alive interval from this session must not survive sign-out --
        // otherwise it keeps firing every 20s regardless of auth state, and on a shared terminal a
        // later, different user signing in (without ever opening Manual/Test themselves) could have
        // their session silently used to renew a hold they never requested. clearInterval() inside
        // here runs unconditionally; the Firebase-side release write itself is a no-op once already
        // signed out, which is fine -- ESP1's own 60s lease expires the hold on its own regardless.
        releaseManualHold();
        mtHoldStop(true);
        exTouched = false;             // let the exercise panel re-sync from ESP1 fresh on next sign-in
        lastZoneEditCapability = undefined;   // force a fresh evaluation on the next sign-in, not a stale match
        myTempAccessUntil = 0;         // a temp-access grant must never survive into a later sign-in
        mySubOperator = false;         // ditto for a persistent sub-operator grant
        myControlAccess = false;       // and for Control and Edit zones
        myZoneEditAccess = false;
        clearZoneDrafts();             // nobody else should find this person's half-made column edits
        lockSentKey = null;            // nor a "Locking…" or "Sending…" left from their own request
        actSentKey = null;
        detachPresence();              // mark this session offline immediately -- don't wait for onDisconnect
        // liveData is about to be wiped below -- reset this FIRST so the updateDashboard() call just
        // after doesn't see (wasHeldOrStopped=true, empty liveData) and announce a fabricated "The hold
        // cleared" from data loss, as if the fault/lockout had actually resolved.
        wasHeldOrStopped = false;
        liveData = {};
        commandData = [];
        sentCommands.clear();
        renderZonesUI();
        renderCropLibrary();
        updateDashboard();
        renderCommandHistory();
        renderAccountStatus(null);
        renderBlockedScreen();
        renderWaitingScreen();
        if (loginScreen) loginScreen.hidden = false;
        showAuthForm("login");
        if (logoutButton) logoutButton.hidden = true;
        if (currentUserEmailEl) { currentUserEmailEl.hidden = true; currentUserEmailEl.textContent = ""; }
        setConnection(false, "Sign in required");
        syncControlAvailability();
        refreshOperatorUI();
        return;
      }
      // ESP1's own login is not a person's account. The rules let it write any command row and its own
      // telemetry, so it must never run this page: it would also leave a stray /users and /presence record.
      if (user.uid === ESP1_DEVICE_UID) {
        const errorBox = document.getElementById("loginError");
        if (errorBox) errorBox.textContent = "That is ESP1's device account, which can't be used on this website. Sign in with your own account.";
        auth.signOut().catch(error => console.error(error));
        return;
      }
      if (loginScreen) loginScreen.hidden = true;
      if (logoutButton) logoutButton.hidden = false;
      // Lets whoever is at this screen confirm which account is actually signed in -- e.g. on a
      // shared terminal, or after a Kick silently swapped the session back to the login screen.
      if (currentUserEmailEl) { currentUserEmailEl.textContent = user.email || ""; currentUserEmailEl.hidden = !user.email; }
      attachDatabaseListeners();
      attachUserStatusListener(user.uid);
      attachPresence(user.uid);
      refreshOperatorUI();
      syncControlAvailability();
    });
  } catch (error) {
    console.error(error);
    setConnection(false, "Firebase setup failed");
  }
}

function attachDatabaseListeners() {
  if (databaseListenersAttached || !db) return;
  databaseListenersAttached = true;
  databaseAccessDenied = false;
  setConnection(false, "Connecting to Firebase…");
  serverOffsetRef = db.ref(".info/serverTimeOffset");
  serverOffsetRef.on("value", snapshot => { serverTimeOffsetMs = Number(snapshot.val()) || 0; });
  firebaseLinkRef = db.ref(".info/connected");
  firebaseLinkRef.on("value", snapshot => {
    const up = snapshot.val() === true;
    if (up) firebaseLinkSeen = true;
    if (!firebaseLinkSeen) return;
    firebaseConnected = up;
    syncConnection();
    syncControlAvailability();
  });
  liveRef = db.ref("irrigation/live");
  commandsRef = db.ref("irrigation/commands").limitToLast(25);

  liveRef.on("value", snapshot => {
    liveData = snapshot.val() || {};
    lastKnownFresh = deviceIsFresh();
    updateDashboard();
    setText("liveAge", liveAgeText());
    syncConnection();
    syncControlAvailability();
    if (refreshAskedAt && Number(liveData.meta?.updatedAt || 0) > refreshBaseStamp) finishRefresh(true);
  }, error => {
    if (error.code === "PERMISSION_DENIED") {
      // Live data needs an approved account. The denial also ends this listener, so
      // attachUserStatusListener() has to attach it again if the account gets approved in this session.
      databaseAccessDenied = true;
      setConnection(false, "Live data needs an approved account");
    } else {
      setConnection(false, `Live-data error: ${error.code || "unknown"}`);
    }
  });

  commandsRef.on("value", snapshot => {
    const raw = snapshot.val() || {};
    commandData = Object.entries(raw).map(([id, command]) => ({ id, ...command }));
    renderCommandHistory();
    announceCommandReplies();     // whichever tab is open, say what ESP1 answered
    renderPulseResult();          // the pulse verdict rides on its command status detail
    renderColumnCommandResults(); // each zone's own SET_COLUMN outcome, inline under its own button
    renderPanelCommandResults();  // the same, for the Controls, Manual/Test and System panels
    updateForceArmed();           // a FORCE_RUN row turning "accepted" is the first sign it is armed
    renderUpNext();               // ...and Up next lists it, and a completed SET_COLUMN or SET_THRESH changes the times
    syncControlAvailability();    // a reply can settle the lock switch or the pumps-and-valves switch before a report does
  }, error => {
    // The account banner already explains a denial: no need for an error toast on every sign-in as well.
    if (error.code === "PERMISSION_DENIED") { databaseAccessDenied = true; return; }
    setCommandStatus(`Command status unavailable: ${error.code || "unknown"}`, "error");
  });

  zonesRef = db.ref("irrigation/config/zones");
  // Live, not once(): a second tab/device editing a zone profile must not leave this one showing a
  // stale crop/stage/name (and the "lacking-nutrient" targets derived from it) until it is reloaded.
  zonesRef.on("value", snapshot => {
    const saved = snapshot.val();
    if (!saved) return;
    activeZones = DEFAULT_ZONES.map(def => {
      const zone = saved[def.id];
      // A zone absent from this snapshot (never saved, or only OTHER zones were ever saved) keeps
      // ITS OWN original default -- previously every unsaved zone fell back to one hardcoded
      // crop/name the moment any single zone was ever saved, silently mangling the other two.
      if (!zone) return { ...def, savedSettings: {} };
      // Kept as saved, even when it isn't in the crop list: that list arrives separately (and a crop added on
      // another device may not be here yet), so zoneCrop() resolves it when drawing instead.
      const crop = typeof zone.crop === "string" && zone.crop ? zone.crop : def.defaultCrop;
      const stage = typeof zone.stage === "string" && zone.stage ? zone.stage : def.defaultStage;
      // savedSettings: this column's own "Firmware save settings" library (see
      // saveColumnSettings()/renderZonesUI()) -- lives under the same dashboard-only config/zones
      // node as crop/stage, so no new Firebase rule or listener was needed; this one already
      // watches the whole subtree.
      return { id: def.id, name: zone.name || def.name, defaultCrop: crop, defaultStage: stage, savedSettings: zone.savedSettings || {} };
    });
    renderZonesUI();
    renderCropLibrary();          // "Used by column A" follows the columns' choices
    updateDashboard();
  }, error => console.warn("Could not read dashboard zone profiles", error));

  cropsRef = db.ref("irrigation/config/crops");
  cropsRef.on("value", snapshot => {
    cropOverrides = snapshot.val() || {};
    renderZonesUI();
    renderCropLibrary();
    updateDashboard();
  }, error => console.warn("Could not read the crop profiles", error));
}

// The top bar and sidebar. Called from a snapshot, from the 1 s tick when ESP1 goes quiet, and when this
// browser's own connection changes.
function syncConnection() {
  if (!databaseListenersAttached || databaseAccessDenied) return;
  if (!firebaseConnected) { setConnection(false, "This browser has lost its connection"); return; }
  const fresh = deviceIsFresh();
  setConnection(fresh, fresh ? "Live system connected" : "Waiting for a current ESP1 snapshot");
}

// A rig that stops reporting sends nothing to react to, so nothing else would redraw the page: the top bar
// would go on saying it's connected, and a pump last reported running would stay lit, for as long as the page
// stayed open.
function syncFreshness() {
  const fresh = deviceIsFresh();
  if (fresh === lastKnownFresh) return;
  lastKnownFresh = fresh;
  updateDashboard();
  syncConnection();
}

function detachDatabaseListeners() {
  if (serverOffsetRef) serverOffsetRef.off();
  if (firebaseLinkRef) firebaseLinkRef.off();
  if (liveRef) liveRef.off();
  if (commandsRef) commandsRef.off();
  if (zonesRef) zonesRef.off();
  if (cropsRef) cropsRef.off();
  if (logsRef) { logsRef.off(); logsRef = null; }
  logsIndex = {}; logsDenied = false;
  serverOffsetRef = null;
  firebaseLinkRef = null;
  firebaseLinkSeen = false;
  firebaseConnected = true;
  liveRef = null;
  commandsRef = null;
  zonesRef = null;
  cropsRef = null;
  // Back to the built-in list until it is read again; a half-made crop edit isn't kept for the next account.
  cropOverrides = {};
  cropDraft = null;
  databaseListenersAttached = false;
}

function showAuthForm(which) {
  const login = document.getElementById("loginForm");
  const signup = document.getElementById("signupForm");
  const reset = document.getElementById("resetForm");
  if (login) login.hidden = which !== "login";
  if (signup) signup.hidden = which !== "signup";
  if (reset) reset.hidden = which !== "reset";
}

/* Own-account approval status. Independent of attachDatabaseListeners() above -- this reads
 * /users/{myUid}, not ESP1's telemetry, and drives the account-status banner plus
 * isApprovedUser()'s gating everywhere else on the page. */
// Readings are for approved accounts and operators only: pending, restricted, rejected and blocked
// accounts must not see them. The rules enforce that when a listener starts, but a listener that is
// already running isn't reliably cut off when an operator restricts the account later, and what the
// page already received would stay on screen. So losing approval drops the listeners and wipes the
// data here too. Regaining it attaches them again, without needing a reload.
function syncLiveDataAccess() {
  if (isApprovedUser()) {
    if (databaseAccessDenied) {
      detachDatabaseListeners();
      attachDatabaseListeners();
    }
    return;
  }
  detachDatabaseListeners();
  databaseAccessDenied = true;
  wasHeldOrStopped = false;      // the data is going away, the fault isn't: don't announce it as cleared
  liveData = {};
  commandData = [];
  activeZones = DEFAULT_ZONES.map(zone => ({ ...zone }));
  clearZoneDrafts();
  renderZonesUI();
  renderCropLibrary();
  updateDashboard();
  renderCommandHistory();
  setConnection(false, "Live data needs an approved account");
}

function attachUserStatusListener(uid) {
  detachUserStatusListener();
  myAccountStatus = undefined;
  renderAccountStatus(undefined);
  renderBlockedScreen();
  myAccountRef = db.ref(`users/${uid}`);
  myAccountRef.on("value", snapshot => {
    const record = snapshot.val();
    // Kick: a kickToken newer than the last one THIS client already handled means the operator
    // wants this session ended -- sign out immediately. A kick is a ONE-TIME "please sign in
    // again," never a standing restriction, so the marker is cleared (the rules' own kickToken
    // rule permits the record's owner to clear -- never set -- their own kickToken) before signing
    // out -- otherwise it would linger forever and re-trigger on this account's every later sign-in
    // AND on every plain page refresh from then on, not just the one time it was meant to fire.
    // lastHandledKickToken stays as a same-page fallback for the rare case the clear write itself
    // fails; a real page reload resets it to 0, so a closed tab reopening still correctly gets
    // kicked once on reconnect if it was kicked while away, and a later, genuinely new Kick (a
    // bigger token) still correctly fires again.
    // Real limit, stated plainly: this only works because this exact client code chooses to
    // cooperate -- it cannot revoke a token already extracted and replayed outside this page.
    const kt = record?.kickToken || 0;
    if (kt && kt > lastHandledKickToken) {
      lastHandledKickToken = kt;
      myAccountRef.child("kickToken").remove()
        .catch(error => console.warn("Could not clear kickToken", error))
        .finally(() => releaseHoldsThenSignOut());
      return;
    }
    const wasApproved = myAccountStatus === "approved";
    const hadControl = hasControlAccess();
    myAccountStatus = record ? record.status : null;
    myAccountName = typeof record?.name === "string" ? record.name : "";
    myTempAccessUntil = Number(record?.tempAccessUntil) || 0;
    mySubOperator = Boolean(record?.subOperator);
    myControlAccess = record?.controlAccess === true;
    myZoneEditAccess = record?.zoneEditAccess === true;
    syncLiveDataAccess();
    renderAccountStatus(record);
    renderBlockedScreen();
    renderWaitingScreen();
    syncControlAvailability();
    syncZoneEditCapability();
    refreshOperatorUI();   // an operator's grant or take-back must show/hide my tabs live
    renderAccessRequests();
    // Block/Restrict must release an in-progress Manual/Test hold immediately, the same way Kick's
    // forced sign-out already does via onAuthStateChanged -- otherwise the rig stays out of
    // automatic for up to the full 60s lease after the operator believes access was cut off at once.
    if (wasApproved && myAccountStatus !== "approved") releaseManualHold();
    if (hadControl && !hasControlAccess()) mtHoldStop(true);
  }, error => {
    console.warn("Could not read account status", error);
    myAccountStatus = null;
    myTempAccessUntil = 0;
    mySubOperator = false;
    myControlAccess = false;
    myZoneEditAccess = false;
    syncLiveDataAccess();
    renderAccountStatus(null);
    renderBlockedScreen();
    renderWaitingScreen();
    syncControlAvailability();
    syncZoneEditCapability();
    refreshOperatorUI();
  });
  attachMyRequestsListener(uid);
}
function detachUserStatusListener() {
  if (myAccountRef) myAccountRef.off();
  myAccountRef = null;
  myAccountStatus = undefined;
  myAccountName = "";
  myControlAccess = false;
  myZoneEditAccess = false;
  detachMyRequestsListener();
}

const ACCOUNT_STATUS_TEXT = {
  pending: {
    title: "Waiting for operator approval",
    detail: "Your account has been created, but an operator has not approved it yet. Live readings and every control stay unavailable until you're approved. This page updates by itself once you are."
  },
  rejected: {
    title: "Access not approved",
    detail: "An operator reviewed this account and did not approve it, so live readings and controls are unavailable."
  },
  restricted: {
    title: "Restricted access",
    detail: "An operator has restricted this account, so live readings and controls are unavailable. The operator can unrestrict it to restore full access at any time."
  }
  // No "disabled" entry here on purpose -- a blocked account gets the separate full-screen
  // #blockedScreen takeover (see renderBlockedScreen()) instead of this banner, and is excluded
  // below before this lookup ever runs.
};

function renderAccountStatus(record) {
  const banner = document.getElementById("accountStatusBanner");
  const title = document.getElementById("accountStatusTitle");
  const detail = document.getElementById("accountStatusDetail");
  if (!banner) return;
  // Signed-out is not "loading" or any other status -- must be checked before the undefined/pending
  // branches below, or the sign-out path's renderAccountStatus(null) call (after
  // detachUserStatusListener() has already reset myAccountStatus to undefined) would flash this
  // banner with "Loading your account status..." on the login screen.
  // "disabled" (blocked) gets its own full-screen takeover (#blockedScreen, see renderBlockedScreen())
  // instead of this banner -- a blocked account should see nothing else on the page at all, not a
  // dismissible banner over an otherwise-normal dashboard.
  // Pending, rejected and restricted accounts get the full-screen waiting page instead (renderWaitingScreen()).
  if (!currentUserIsSignedIn() || isOperator() || ["approved", "disabled", ...WAITING_STATUSES].includes(myAccountStatus)) { banner.hidden = true; return; }
  banner.hidden = false;
  if (myAccountStatus === undefined) {
    if (title) title.textContent = "Loading your account status…";
    if (detail) detail.textContent = "--";
    return;
  }
  const text = ACCOUNT_STATUS_TEXT[myAccountStatus] || {
    title: "Setting up your account…",
    detail: "If this doesn't change in a moment, refresh the page. A brand-new account's record can take a second to appear."
  };
  if (title) title.textContent = text.title;
  if (detail) detail.textContent = text.detail;
}

/* Full-screen takeover for a blocked ("disabled") account -- deliberately bypasses the fault banner,
 * the account-status banner, and the normal dashboard entirely. Re-checked on every account-status
 * snapshot (see attachUserStatusListener()), so an Unblock takes this away live without a reload,
 * same as every other status change on this page. */
function renderBlockedScreen() {
  const screen = document.getElementById("blockedScreen");
  if (!screen) return;
  screen.hidden = !(currentUserIsSignedIn() && !isOperator() && myAccountStatus === "disabled");
  syncTakeover();
}
// While the blocked or waiting page is up, the site itself isn't laid out at all (body.takeover in style.css).
function syncTakeover() {
  const covered = ["blockedScreen", "waitingScreen"].some(id => document.getElementById(id)?.hidden === false);
  document.body.classList.toggle("takeover", covered);
}
document.getElementById("blockedSignOutBtn")?.addEventListener("click", () => {
  if (auth) releaseHoldsThenSignOut().catch(error => console.error(error));
});

/* ---- Waiting page and access requests (2026-11-05) ------------------------------------------------------------
 * A new, rejected or restricted account sees nothing of the rig: only this page, with one button to ask for approval
 * (or to be unrestricted). A blocked account gets #blockedScreen and can't ask. An approved account asks for Control
 * on the Controls tab and for Edit zones on Zone profiles. Requests live in accessRequests/{uid}/{kind} as
 * {state: "waiting" | "declined", at, decidedAt?}; the rules let an account write only its own, only the kind its
 * status allows, and only when none is already waiting. Operators see them at once (a pop-up and a count on User
 * Management) and answer there. */
const WAITING_STATUSES = ["pending", "rejected", "restricted"];
// ask: the button; asked: "You asked for ..."; what: an operator's "<name> asks for ..."; log: the activity trail.
const REQUEST_WORDS = {
  approval:   { ask: "Ask for approval",              asked: "approval",                        what: "approval of their account",          log: "Asked for approval" },
  unrestrict: { ask: "Ask to be unrestricted",        asked: "your account to be unrestricted", what: "their account to be unrestricted",   log: "Asked to be unrestricted" },
  zones:      { ask: "Request to edit zone profiles", asked: "access to edit zone profiles",    what: "access to edit zone profiles",       log: "Asked to edit zone profiles" },
  control:    { ask: "Request control",               asked: "control access",                  what: "control access",                     log: "Asked for control" }
};
const WAITING_TEXT = {
  pending: ["Waiting for approval", "Your account is created. An operator has to approve it before you can see the rig. Ask for approval below, and this page opens by itself once you are approved."],
  rejected: ["Not approved", "An operator looked at this account and didn't approve it. You can ask again below."],
  restricted: ["Restricted", "An operator has restricted this account, so the rig can't be seen from it for now. You can ask to be unrestricted below."]
};
let myRequestsRef = null;
let myRequests = {};

function renderWaitingScreen() {
  const screen = document.getElementById("waitingScreen");
  if (!screen) return;
  const status = myAccountStatus;
  const show = currentUserIsSignedIn() && !isOperator() && WAITING_STATUSES.includes(status);
  screen.hidden = !show;
  syncTakeover();
  if (!show) return;
  const [title, detail] = WAITING_TEXT[status];
  setText("waitingTitle", title);
  setText("waitingDetail", detail);
  const account = document.getElementById("waitingAccount");
  if (account) account.textContent = auth?.currentUser?.email ? `Signed in as ${auth.currentUser.email}` : "";
  renderRequestControls("waiting", status === "restricted" ? "unrestrict" : "approval");
}

function attachMyRequestsListener(uid) {
  detachMyRequestsListener();
  if (!db || !uid) return;
  myRequestsRef = db.ref(`accessRequests/${uid}`);
  myRequestsRef.on("value", snapshot => {
    myRequests = snapshot.val() || {};
    renderAccessRequests();
  }, error => console.warn("Could not read your requests", error));
}
function detachMyRequestsListener() {
  if (myRequestsRef) myRequestsRef.off();
  myRequestsRef = null;
  myRequests = {};
}

// One request button with its state, in a slot of the page: "waiting" (the waiting page), "zones" (Zone profiles) or
// "control" (the Controls tab). Each slot has #<slot>RequestBtn, #<slot>RequestState and #<slot>RequestWithdraw.
function renderRequestControls(slot, kind) {
  const button = document.getElementById(`${slot}RequestBtn`);
  const state = document.getElementById(`${slot}RequestState`);
  const withdraw = document.getElementById(`${slot}RequestWithdraw`);
  if (!button) return;
  const request = myRequests[kind];
  const waiting = request?.state === "waiting";
  const declined = request?.state === "declined";
  button.dataset.kind = kind;
  button.hidden = waiting;
  button.textContent = declined ? `${REQUEST_WORDS[kind].ask} again` : REQUEST_WORDS[kind].ask;
  button.disabled = !firebaseConnected;
  if (withdraw) { withdraw.hidden = !waiting; withdraw.dataset.kind = kind; }
  if (state) {
    state.textContent = waiting
      ? `You asked for ${REQUEST_WORDS[kind].asked} ${formatAge(Math.max(0, serverNow() - Number(request.at || 0)))} ago. An operator will see it, and this page changes by itself when they answer.`
      : declined
        ? `An operator declined your request${request.decidedAt ? ` on ${new Date(request.decidedAt).toLocaleDateString()}` : ""}. You can ask again.`
        : "";
    state.hidden = !state.textContent;
  }
}

// Every request slot on the page, from the account's own status and grants.
function renderAccessRequests() {
  renderWaitingScreen();
  const approvedOnly = isApprovedUser() && !isOperator() && !canAccessPrivilegedTabs();
  const zonePanel = document.getElementById("zoneRequestPanel");
  if (zonePanel) zonePanel.hidden = !(approvedOnly && !myZoneEditAccess);
  if (zonePanel && !zonePanel.hidden) renderRequestControls("zones", "zones");
  const controlPanel = document.getElementById("controlRequestPanel");
  const controlsBody = document.getElementById("controlsBody");
  const control = hasControlAccess();
  if (controlsBody) controlsBody.hidden = !control;
  if (controlPanel) controlPanel.hidden = control || !isApprovedUser();
  if (controlPanel && !controlPanel.hidden) renderRequestControls("control", "control");
}

function sendAccessRequest(kind) {
  const uid = auth?.currentUser?.uid;
  if (!uid || !REQUEST_WORDS[kind]) return;
  db.ref(`accessRequests/${uid}/${kind}`).set({ state: "waiting", at: firebase.database.ServerValue.TIMESTAMP })
    .then(() => {
      logActivity(REQUEST_WORDS[kind].log);
      showToast(`Asked. An operator will see your request for ${REQUEST_WORDS[kind].asked}.`);
    })
    .catch(error => showToast(`Could not send your request: ${error.code === "PERMISSION_DENIED" ? "this account can't ask for that right now" : error.message}.`, "error"));
}
function withdrawAccessRequest(kind) {
  const uid = auth?.currentUser?.uid;
  if (!uid || myRequests[kind]?.state !== "waiting") return;
  db.ref(`accessRequests/${uid}/${kind}`).remove()
    .then(() => logActivity(`Took back: ${REQUEST_WORDS[kind].log.toLowerCase()}`))
    .catch(error => showToast(`Could not withdraw your request: ${error.message}.`, "error"));
}
["waiting", "zones", "control"].forEach(slot => {
  document.getElementById(`${slot}RequestBtn`)?.addEventListener("click", event => sendAccessRequest(event.currentTarget.dataset.kind));
  document.getElementById(`${slot}RequestWithdraw`)?.addEventListener("click", event => withdrawAccessRequest(event.currentTarget.dataset.kind));
});
document.getElementById("waitingSignOutBtn")?.addEventListener("click", () => {
  if (auth) releaseHoldsThenSignOut().catch(error => console.error(error));
});

// Releases whatever this session holds, THEN signs out. Signing out first drops those writes, since
// writeManualHold()/writeTestHold() need a signed-in user, and leaves want:true in the database. ESP1
// acts on a leftover testHold want:true the next time it boots (testHoldSeq starts at -1, so any stored
// seq counts as new), which would start a relay with nobody at the page. The cap is for an offline
// browser, whose writes are never acknowledged but which must still be able to sign out.
function releaseHoldsThenSignOut() {
  // Presence goes offline here too, for the same reason: the rules refuse that write once signed out, and
  // cancelling onDisconnect then left the account showing "Online" for good (seen live, for weeks).
  const releases = Promise.allSettled([releaseManualHold(), mtHoldStop(true), logActivity("Signed out"), detachPresence()]);
  const capped = Promise.race([releases, new Promise(resolve => setTimeout(resolve, 1500))]);
  return capped.then(() => auth.signOut());
}

/* User Management (operator-only). The tab/section being hidden from non-operators is a UI
 * convenience -- the actual boundary is /users' ".read" rule, which only the operator UID passes,
 * so this listener is only ever attached in the first place when isOperator() is true. */
let usersRef = null;
function refreshOperatorUI() {
  const tab = document.getElementById("usersTab");
  const op = isOperator();
  if (tab) tab.hidden = !op;
  if (op) attachUserManagementListener(); else detachUserManagementListener();
  if (!op && document.querySelector('.tab[data-view="users"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  // Access-control revision (2026-09-09): System and Manual/Test are privileged-operator-only --
  // they contain Set Clock/Thresholds/Restore Defaults and Pump Exercise/Timed Pulses/Testing
  // respectively, none of which a normal approved (non-operator) account should be able to reach
  // UNLESS an operator has granted it temporary access (canAccessPrivilegedTabs(), added alongside
  // this note) -- deliberately a SEPARATE check from `op` above: User Management stays real-
  // operator-only always, since a temp-elevated account granting/revoking access (including its own)
  // would defeat the whole point of the grant being operator-controlled. This is the UI-convenience
  // half only -- queueCommand()'s operatorOnly check (added alongside this) is what actually refuses
  // the command if someone reaches these controls anyway (devtools, a stale tab left open across a
  // role change, an expired grant before the next 15s recheck, etc).
  const privileged = canAccessPrivilegedTabs();
  const systemTab = document.getElementById("systemTab");
  if (systemTab) systemTab.hidden = !privileged;
  const acVoltPanel = document.getElementById("sysAcVoltPanel");
  if (acVoltPanel) acVoltPanel.hidden = !canSetAcVoltage();
  const acAmpPanel = document.getElementById("sysAcAmpPanel");
  if (acAmpPanel) acAmpPanel.hidden = !canSetAcVoltage();
  if (!privileged && document.querySelector('.tab[data-view="system"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  const manualTestTab = document.getElementById("manualtestTab");
  if (manualTestTab) manualTestTab.hidden = !privileged;
  if (!privileged && document.querySelector('.tab[data-view="manualtest"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  // Diagnostics comes with Control (2026-11-05). The Controls tab stays for every approved account: without Control
  // it holds only the request for it.
  const control = hasControlAccess();
  const diagnosticsTab = document.getElementById("diagnosticsTab");
  if (diagnosticsTab) diagnosticsTab.hidden = !control;
  if (!control && document.querySelector('.tab[data-view="diagnostics"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  // Changing crop profiles is for the same tier: redraw the list (and drop a half-made edit) only when that flips,
  // since this runs every 15 s.
  if (canEditCrops() !== lastCropEditCapability) renderCropLibrary();
  renderAccessRequests();
  syncHelpAudience();
  if (op) attachRequestsWatch(); else detachRequestsWatch();
}
// Help is split by account type. Re-filtered only when what this account can do changes: this runs every 15 s, and
// filtering folds the open entries.
let lastHelpAudience = null;
function syncHelpAudience() {
  const signature = ["all", "asker", "zones", "control", "privileged", "operator"].map(a => (helpAudienceAllowed(a) ? "1" : "0")).join("");
  if (signature === lastHelpAudience) return;
  lastHelpAudience = signature;
  filterHelp(document.getElementById("helpSearch")?.value || "");
}
let lastKnownUsers = {};
function attachUserManagementListener() {
  if (usersRef) return;
  usersRef = db.ref("users");
  usersRef.on("value", snapshot => {
    lastKnownUsers = snapshot.val() || {};
    renderUserManagement();
  }, error => {
    const container = document.getElementById("usersContainer");
    if (container) container.innerHTML = `<p class="muted">Could not load accounts: ${escapeHtml(error.message)}</p>`;
  });
  attachPresenceWatch();
  attachActivityWatch();
}
function detachUserManagementListener() {
  if (usersRef) usersRef.off();
  usersRef = null;
  detachPresenceWatch();
  detachActivityWatch();
}

/* Online/offline presence (User Management only reads this; every signed-in session writes its
 * OWN, see attachPresence()/detachPresence() below). Standard Firebase pattern: `.info/connected`
 * is a special always-available path that flips true once this client's websocket is actually up;
 * onDisconnect() registers a write the SERVER performs the instant it detects this connection is
 * gone (tab closed, network dropped, crash) -- not just a graceful sign-out, which is the whole
 * point over e.g. a plain "last active" timestamp written on a timer.
 * Known limitation: this is the single-connection version of the pattern (Firebase's own docs show
 * a per-connection-id counter for a account open in several tabs/devices at once); with two
 * operators and a handful of field users this was judged not worth the extra complexity -- closing
 * one of two open tabs for the same account will show it as offline even though the other tab is
 * still live. Upgrade to the counter form if that ever actually causes confusion.
 * It did (2026-11-05): an account nobody was using showed "Online now", because a tab left open anywhere kept the
 * one flag on, and closing one of two tabs turned it off. Now each open page has its own entry,
 * presence/{uid}/sessions/{id} = {since, lastActive, visible}, removed by the server when that page goes; lastActive
 * moves only on real use (a tap, a key, scrolling) while the page is on screen; presence/{uid}/lastActive and
 * lastSeen keep the latest across them for when no page is open. See presenceStatus() for how it reads. */
let presenceRef = null;
let presenceData = {};
// Real use within this long, on a page that is on screen, counts as "Active now".
const ACTIVE_NOW_MS = 3 * 60 * 1000;
// What User Management says about an account, from its pages' entries. Records written before 5 Nov 2026 have only
// {online, lastChanged}; their "online" can be stale for weeks, so only lastChanged is used, as a last-seen time.
function presenceStatus(uid) {
  const p = presenceData[uid] || {};
  const sessions = Object.values(p.sessions || {}).filter(s => s && typeof s === "object");
  const now = serverNow();
  const lastActive = Math.max(Number(p.lastActive) || 0, ...sessions.map(s => Number(s.lastActive) || 0));
  if (sessions.length) {
    if (sessions.some(s => s.visible === true && now - (Number(s.lastActive) || 0) < ACTIVE_NOW_MS)) {
      return { online: true, active: true, label: "Active now" };
    }
    const where = sessions.every(s => s.visible !== true) ? "Page open in the background" : "Page open but idle";
    return { online: true, active: false, label: `${where}, last used ${formatAge(Math.max(0, now - lastActive))} ago${sessions.length > 1 ? ` (${sessions.length} pages)` : ""}` };
  }
  const seen = lastActive || Number(p.lastSeen) || Number(p.lastChanged) || 0;
  return { online: false, active: false, label: seen ? `Offline, last active ${formatAge(Math.max(0, now - seen))} ago` : "Not seen on the website yet" };
}
function attachPresenceWatch() {
  if (presenceRef) return;
  presenceRef = db.ref("presence");
  presenceRef.on("value", snapshot => {
    presenceData = snapshot.val() || {};
    renderUserManagement();
  }, error => console.warn("Could not read presence", error));
}
function detachPresenceWatch() {
  if (presenceRef) presenceRef.off();
  presenceRef = null;
  presenceData = {};
}

/* ---- Access requests, the operators' side (2026-11-05) --------------------------------------------------------
 * Every operator account reads accessRequests/. A request that arrives while one is signed in pops up at once, and
 * all waiting ones are counted on the User Management tab and listed at the top of it, to accept or decline. */
let requestsRef = null;
let requestsData = {};
const announcedRequests = new Set();
let requestsLoadedOnce = false;
const ACCEPT_WORDS = { approval: "Approve", unrestrict: "Unrestrict", zones: "Give zone editing", control: "Give control" };

function attachRequestsWatch() {
  if (requestsRef || !db) return;
  requestsRef = db.ref("accessRequests");
  requestsRef.on("value", snapshot => {
    requestsData = snapshot.val() || {};
    announceNewRequests();
    renderRequestsBadge();
    renderUserManagement();
  }, error => console.warn("Could not read access requests", error));
}
function detachRequestsWatch() {
  if (requestsRef) requestsRef.off();
  requestsRef = null;
  requestsData = {};
  announcedRequests.clear();
  requestsLoadedOnce = false;
  renderRequestsBadge();
}
// Waiting requests, oldest first. Never one from an operator account (the rules don't let one exist anyway).
function waitingRequests() {
  const list = [];
  Object.entries(requestsData).forEach(([uid, kinds]) => {
    if (OPERATOR_UIDS.includes(uid)) return;
    Object.entries(kinds || {}).forEach(([kind, request]) => {
      if (request?.state === "waiting" && REQUEST_WORDS[kind]) list.push({ uid, kind, at: Number(request.at) || 0 });
    });
  });
  return list.sort((a, b) => a.at - b.at);
}
function requesterName(uid) {
  const user = lastKnownUsers[uid];
  return user?.name || user?.email || "Someone";
}
function openUserManagement() {
  document.querySelector('.tab[data-view="users"]')?.click();
}
// The pop-up. On the first read, several waiting requests are summed up in one; after that each new one gets its own.
function announceNewRequests() {
  const fresh = waitingRequests().filter(r => !announcedRequests.has(`${r.uid}/${r.kind}/${r.at}`));
  fresh.forEach(r => announcedRequests.add(`${r.uid}/${r.kind}/${r.at}`));
  const first = !requestsLoadedOnce;
  requestsLoadedOnce = true;
  if (!fresh.length) return;
  if (first && fresh.length > 1) {
    showToast(`${fresh.length} access requests are waiting. Click to answer them in User Management.`, "pending", openUserManagement);
    return;
  }
  fresh.forEach(r => showToast(`${requesterName(r.uid)} asks for ${REQUEST_WORDS[r.kind].what}. Click to answer in User Management.`, "pending", openUserManagement));
}
function renderRequestsBadge() {
  const tab = document.getElementById("usersTab");
  if (!tab) return;
  const count = isOperator() ? waitingRequests().length : 0;
  let badge = tab.querySelector(".tab-badge");
  if (!count) {
    badge?.remove();
    tab.removeAttribute("aria-label");
    return;
  }
  if (!badge) {
    badge = document.createElement("span");
    badge.className = "tab-badge";
    badge.setAttribute("aria-hidden", "true");
    tab.append(badge);
  }
  badge.textContent = String(count);
  tab.setAttribute("aria-label", `User Management, ${count} ${count === 1 ? "request" : "requests"} waiting`);
}
function requestsSectionHtml() {
  const waiting = waitingRequests();
  if (!waiting.length) return "";
  return `<section class="user-group requests-group" aria-label="Requests waiting">
      <h3>Requests waiting <span class="user-count">${waiting.length}</span></h3>
      ${waiting.map(r => {
        const user = lastKnownUsers[r.uid] || {};
        return `<article class="user-row request-row">
          <div class="user-row-info">
            <strong>${escapeHtml(user.name || "(no name)")}</strong>
            <span class="muted">${escapeHtml(user.email || "")}</span>
            <span>Asks for ${escapeHtml(REQUEST_WORDS[r.kind].what)}, ${escapeHtml(formatAge(Math.max(0, serverNow() - r.at)))} ago.</span>
          </div>
          <div class="user-row-actions">
            <button type="button" class="user-action" data-request-answer="accept" data-kind="${r.kind}" data-uid="${escapeHtml(r.uid)}">${ACCEPT_WORDS[r.kind]}</button>
            <button type="button" class="user-action warn" data-request-answer="decline" data-kind="${r.kind}" data-uid="${escapeHtml(r.uid)}">Decline</button>
          </div>
        </article>`;
      }).join("")}
    </section>`;
}
// Accepting gives the access (or the status) and clears the request in one write; declining keeps it, marked, so the
// account sees the answer and can ask again.
function answerAccessRequest(uid, kind, accept) {
  if (!isOperator() || OPERATOR_UIDS.includes(uid) || !REQUEST_WORDS[kind]) return Promise.resolve();
  const name = requesterName(uid);
  const updates = {};
  if (accept) {
    if (kind === "approval" || kind === "unrestrict") updates[`users/${uid}/status`] = "approved";
    if (kind === "zones") updates[`users/${uid}/zoneEditAccess`] = true;
    if (kind === "control") updates[`users/${uid}/controlAccess`] = true;
    updates[`accessRequests/${uid}/${kind}`] = null;
  } else {
    updates[`accessRequests/${uid}/${kind}/state`] = "declined";
    updates[`accessRequests/${uid}/${kind}/decidedAt`] = firebase.database.ServerValue.TIMESTAMP;
  }
  return db.ref().update(updates).then(() => {
    logActivity(`${accept ? ACCEPT_WORDS[kind] : "Declined"}: ${name}${accept ? "" : `, ${REQUEST_WORDS[kind].log.toLowerCase()}`}`.slice(0, 80));
    showToast(accept ? `${name}: done. ${kind === "zones" ? "They can edit zone profiles now." : kind === "control" ? "They have control access now." : "Their account is approved."}` : `Declined. ${name} sees that, and can ask again.`);
  });
}

/* Per-account recent activity (2026-09-10) -- operator-viewing-only (see firebase-rules.json's
 * activity/.read: only the operator accounts can read ANY of it, not even a user their own trail).
 * Same whole-tree-listener shape as attachPresenceWatch() above; fine at this project's scale (a
 * handful of accounts) -- see that function's own note on why a per-row listener wasn't used either. */
let activityRef = null;
let activityData = {};
function attachActivityWatch() {
  if (activityRef) return;
  activityRef = db.ref("activity");
  activityRef.on("value", snapshot => {
    activityData = snapshot.val() || {};
    renderUserManagement();
  }, error => console.warn("Could not read activity", error));
}
function detachActivityWatch() {
  if (activityRef) activityRef.off();
  activityRef = null;
  activityData = {};
}
// Newest-first, capped at 15 -- a recent-history glance, not a full audit trail (irrigation/commands
// + ESP1's own SD log already are the real record of everything that happened).
function recentActivityFor(uid) {
  const entries = Object.values(activityData[uid] || {});
  return entries.sort((a, b) => Number(b?.at || 0) - Number(a?.at || 0)).slice(0, 15);
}
// Which accounts' activity list is currently expanded (2026-09-10) -- renderUserManagement() rebuilds
// the ENTIRE #usersContainer innerHTML on every presence/activity/users update, so an open/closed
// <details> element's own state would otherwise be destroyed and reset on the very next render (e.g.
// a heartbeat presence update landing while an operator has one expanded). Tracked here, outside the
// render, and reapplied via the `open` attribute each time; kept in sync by the delegated toggle
// listener below.
let expandedActivityUids = new Set();

let myPresenceRef = null;          // presence/{uid}
let mySessionRef = null;           // presence/{uid}/sessions/{this page}
let myConnectedRef = null;
let lastUseWrittenAt = 0;
// Real use is written at most this often, so a busy page doesn't write on every tap.
const PRESENCE_USE_WRITE_MS = 60000;
function attachPresence(uid) {
  if (!db) return;
  detachPresence();
  myPresenceRef = db.ref(`presence/${uid}`);
  mySessionRef = myPresenceRef.child("sessions").push();     // this page's own entry
  myConnectedRef = db.ref(".info/connected");
  myConnectedRef.on("value", snap => {
    if (snap.val() !== true) return;
    const now = firebase.database.ServerValue.TIMESTAMP;
    // The disconnect writes go FIRST: if the connection drops before the entry lands, the server still removes it
    // and stamps lastSeen, instead of leaving this page counted as open.
    mySessionRef.onDisconnect().remove()
      .then(() => myPresenceRef.child("lastSeen").onDisconnect().set(now))
      .then(() => mySessionRef.set({ since: now, lastActive: now, visible: !document.hidden }))
      // The single flag from before 5 Nov 2026 goes, so nothing reads a stale "online" again.
      .then(() => myPresenceRef.update({ lastActive: now, online: null, lastChanged: null }))
      .then(() => { lastUseWrittenAt = Date.now(); })
      .catch(error => console.warn("Could not set presence", error));
  });
}
function detachPresence() {
  if (myConnectedRef) myConnectedRef.off();
  let offline = Promise.resolve();
  if (mySessionRef && myPresenceRef) {
    mySessionRef.onDisconnect().cancel();
    myPresenceRef.child("lastSeen").onDisconnect().cancel();
    offline = Promise.all([
      mySessionRef.remove(),
      myPresenceRef.update({ lastSeen: firebase.database.ServerValue.TIMESTAMP })
    ]).catch(() => {});
  }
  myConnectedRef = null;
  myPresenceRef = null;
  mySessionRef = null;
  return offline;
}
// A tap, a key or scrolling while this page is on screen: the account is in use.
function notePresenceUse() {
  if (!mySessionRef || !myPresenceRef || document.hidden) return;
  if (Date.now() - lastUseWrittenAt < PRESENCE_USE_WRITE_MS) return;
  lastUseWrittenAt = Date.now();
  const now = firebase.database.ServerValue.TIMESTAMP;
  mySessionRef.update({ lastActive: now, visible: true }).catch(() => {});
  myPresenceRef.update({ lastActive: now }).catch(() => {});
}
["pointerdown", "keydown", "wheel", "touchstart"].forEach(type => document.addEventListener(type, notePresenceUse, { passive: true, capture: true }));
document.addEventListener("visibilitychange", () => {
  if (!mySessionRef) return;
  mySessionRef.update({ visible: !document.hidden }).catch(() => {});
  if (!document.hidden) { lastUseWrittenAt = 0; notePresenceUse(); }
});

// One group per status, in this order: the accounts that need something done come first. The labels are what
// people read; the values are what the rules enforce ("disabled" is shown as Blocked).
const USER_GROUPS = [
  ["pending", "Waiting for approval"],
  ["approved", "Approved"],
  ["restricted", "Restricted"],
  ["disabled", "Blocked"],
  ["rejected", "Rejected"]
];
const STATUS_DISPLAY_LABEL = { disabled: "BLOCKED", restricted: "RESTRICTED" };
// Button words. The data-user-action values stay as they were, because the handler below goes by them.
const USER_ACTION_LABEL = {
  approve: "Approve", reject: "Reject", restrict: "Restrict", unrestrict: "Unrestrict", block: "Block", unblock: "Unblock",
  kick: "Sign them out", delete: "Delete",
  givezones: "Give zone editing", takezones: "Take back zone editing",
  givecontrol: "Give control", takecontrol: "Take back control",
  granttemp: "Give 1-hour access", revoketemp: "End 1-hour access",
  grantsubop: "Make Sub-operator", withholdsubop: "Remove Sub-operator"
};
const USER_ACTION_INFO = {
  givezones: ["Zone editing", "Lets this account change the column cards on Zone profiles: settings, watering times, targets, on or off, crop and name. It lasts until you take it back."],
  takezones: ["Zone editing", "Ends this account's zone editing now. It can still see everything, and can ask again."],
  givecontrol: ["Control", "Lets this account use the Controls tab (Emergency stop, pumps and valves, forced runs, restarts) and see Diagnostics. It lasts until you take it back."],
  takecontrol: ["Control", "Ends this account's control access now. It can still see the Live dashboard and Zone profiles, and can ask again."],
  granttemp: ["1-hour access", "Opens Manual/Test, System (the AC voltage and current limits included) and the rig lock to this account for exactly 1 hour, then it ends by itself. Use it for a one-off job."],
  revoketemp: ["1-hour access", "Ends this account's 1-hour access now, instead of waiting for its hour to run out."],
  grantsubop: ["Sub-operator", "The same access as 1-hour access, with no end: it lasts until an operator removes it. Use it for someone who needs it all the time."],
  withholdsubop: ["Sub-operator", "Ends this account's Sub-operator access now. Nothing else ever ends it."]
};
function renderUserManagement() {
  const container = document.getElementById("usersContainer");
  if (!container) return;
  const users = lastKnownUsers;
  // The unlisted operator account is deliberately invisible here -- filtered out of the list itself,
  // not merely stripped of actions like the primary operator's own row is (see isAnyOperator
  // below). Nothing in this app ever needs to manage the backup account as if it were a normal user.
  // ESP1's own account isn't a person either. It has no actions, so a stray record for it (the live database
  // has one, from before the page refused that login) would sit under "pending" for good.
  const rows = Object.entries(users).filter(([uid]) => uid !== BACKUP_OPERATOR_UID && uid !== ESP1_DEVICE_UID);
  if (!rows.length) { container.innerHTML = `<p class="muted">No registered accounts yet.</p>`; return; }
  const rowHtml = ([uid, u]) => {
    const status = u?.status || "unknown";
    const created = u?.createdAt ? new Date(u.createdAt).toLocaleString() : "Unknown";
    // Either operator's own row (if a /users record happens to exist at all) gets zero actions --
    // real enforcement is the rules' own guard refusing either operator write to either operator's
    // record; this is the UI half, so there is nothing to accidentally click in the first place.
    // Same exclusion for ESP1's device UID as defense-in-depth, though nothing today ever creates a
    // /users record for it. Kept separate from "is this row the person actually viewing the screen"
    // below -- with more than one operator account, a row is not automatically "you" just because it's *an* operator.
    const isAnyOperator = OPERATOR_UIDS.includes(uid) || uid === ESP1_DEVICE_UID;
    const isViewerSelf = uid === auth.currentUser?.uid;
    // Temporary Manual/Test + System access (see canAccessPrivilegedTabs() in the capability-check
    // section above) -- only ever meaningful for an approved, non-operator account; an operator
    // already has full access, and a pending/restricted/blocked account can't be elevated without
    // first being approved.
    // Server timestamps, all of them, so they are measured against Firebase's clock, not this PC's.
    const tempActive = status === "approved" && !isAnyOperator && Number(u?.tempAccessUntil) > serverNow();
    // Sub-operator (2026-09-10): a SECOND, separate elevation to Manual/Test + System -- persistent
    // (no expiry) rather than the 1h temp grant above, toggled on/off like a switch until an operator
    // changes it again. Independent of tempActive -- an account can hold either, both, or neither;
    // canAccessPrivilegedTabs() ORs them together client-side.
    const subOpActive = status === "approved" && !isAnyOperator && Boolean(u?.subOperator);
    // Control and Edit zones (2026-11-05), each given and taken back on its own. A Sub-operator has both anyway.
    const controlActive = status === "approved" && !isAnyOperator && u?.controlAccess === true;
    const zonesActive = status === "approved" && !isAnyOperator && u?.zoneEditAccess === true;
    // Online/offline (see attachPresence()/attachPresenceWatch() above) -- also decides whether
    // Kick is even offered: kicking an already-offline account just wrote a fresh kickToken that
    // would immediately re-fire the moment they reconnect, which is exactly the "kicked N times"
    // symptom that clicking Kick repeatedly on an offline user produced before this gate existed.
    const presence = presenceStatus(uid);
    const isOnline = presence.online;
    const presenceLabel = presence.label;
    // Recent activity (2026-09-10): the last commands this account actually queued (see
    // activitySummary()/queueCommand() above), newest first. Operator-viewing-only, same as presence
    // -- enforced server-side by activity/.read, not just by this tab being hidden from everyone else.
    const recent = recentActivityFor(uid);
    const actions = [];
    if (!isAnyOperator) {
      if (status === "pending")    actions.push("approve", "reject", "restrict", "delete");
      if (status === "approved") {
        if (!subOpActive) actions.push(zonesActive ? "takezones" : "givezones", controlActive ? "takecontrol" : "givecontrol");
        actions.push(tempActive ? "revoketemp" : "granttemp", subOpActive ? "withholdsubop" : "grantsubop");
        actions.push(...(isOnline ? ["kick"] : []), "restrict", "block", "delete");
      }
      if (status === "restricted") actions.push("unrestrict", ...(isOnline ? ["kick"] : []), "block", "delete");
      if (status === "rejected")   actions.push("approve", "restrict", "block", "delete");
      if (status === "disabled")   actions.push("unblock", "delete");
    }
    const tone = status === "approved" ? "active" : (status === "pending" || status === "restricted") ? "off" : "danger";
    const tones = { delete: " danger", block: " warn", revoketemp: " warn", withholdsubop: " warn", takezones: " warn", takecontrol: " warn" };
    const buttons = actions.map(action => {
      const btn = `<button type="button" class="user-action${tones[action] || ""}" data-user-action="${action}" data-uid="${escapeHtml(uid)}">${escapeHtml(USER_ACTION_LABEL[action])}</button>`;
      // The two grants look alike at a glance, so each says how it ends. The icon is a sibling, never inside the
      // button, as everywhere else on this page.
      const info = USER_ACTION_INFO[action];
      return info
        ? `<span class="btn-with-info">${btn}<button type="button" class="info-icon" aria-haspopup="dialog" aria-expanded="false" aria-label="About ${info[0]}" data-info-title="${info[0]}" data-info-text="${info[1]}" data-help="${/zones$|control$/.test(action) ? "help-access" : "help-grants"}">i</button></span>`
        : btn;
    }).join("");
    // Zone editing and Control aren't spelled out here (the owner's choice, 6 Nov 2026): the Give / Take back buttons
    // beside the row already say which the account has.
    const access = [];
    if (tempActive) access.push(`Can use Manual/Test, System and the rig lock until ${new Date(u.tempAccessUntil).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })} (1-hour access).`);
    if (subOpActive) access.push("Sub-operator: can use Manual/Test, System (including the AC voltage and current limits) and the rig lock until you remove it.");
    return `<article class="user-row" data-email="${escapeHtml(u?.email || "")}">
      <div class="user-row-info">
        <strong>${escapeHtml(u?.name || "(no name)")}${isViewerSelf ? " (you, the operator)" : ""}</strong>
        <span class="muted">${escapeHtml(u?.email || "(no email)")}</span>
        <span class="${presence.active ? "user-online" : isOnline ? "user-idle" : "muted"}">${escapeHtml(presenceLabel)}</span>
        <span class="muted">Signed up ${escapeHtml(created)}</span>
        ${access.map(line => `<span class="user-access">${escapeHtml(line)}</span>`).join("")}
        ${recent.length
          ? `<details class="user-activity" data-uid="${escapeHtml(uid)}"${expandedActivityUids.has(uid) ? " open" : ""}>
              <summary>Last action: ${escapeHtml(rawText(recent[0]?.what, "?"))}, ${escapeHtml(formatAge(serverNow() - Number(recent[0]?.at || 0)))} ago${recent.length > 1 ? ` (${recent.length} in the list)` : ""}</summary>
              <ul class="user-activity-list">${recent.map(a =>
                `<li>${escapeHtml(rawText(a?.what, "?"))}, ${escapeHtml(formatAge(serverNow() - Number(a?.at || 0)))} ago</li>`
              ).join("")}</ul>
            </details>`
          : `<span class="muted">No recent actions on the website.</span>`}
      </div>
      <span class="device-status ${isAnyOperator ? "active" : tone}">${escapeHtml(isAnyOperator ? "OPERATOR" : STATUS_DISPLAY_LABEL[status] || status.toUpperCase())}</span>
      <div class="user-row-actions">${buttons}</div>
    </article>`;
  };
  // An operator's own sign-in can leave a "pending" record (the sign-in path recreates a missing one for any
  // account), so operator accounts get a group of their own, rather than being counted as waiting for approval.
  const isOperatorRow = ([uid]) => OPERATOR_UIDS.includes(uid);
  const known = new Set(USER_GROUPS.map(([status]) => status));
  const groups = [...USER_GROUPS, ["", "Other"], ["operator", "Operator accounts"]].map(([status, title]) => {
    const members = status === "operator" ? rows.filter(isOperatorRow)
      : rows.filter(row => !isOperatorRow(row)).filter(([, u]) => status ? u?.status === status : !known.has(u?.status));
    if (!members.length) return "";
    members.sort((a, b) => Number(b[1]?.createdAt || 0) - Number(a[1]?.createdAt || 0));
    return `<section class="user-group" aria-label="${escapeHtml(title)}">
      <h3>${escapeHtml(title)} <span class="user-count">${members.length}</span></h3>
      ${members.map(rowHtml).join("")}
    </section>`;
  });
  container.innerHTML = requestsSectionHtml() + groups.join("");
}
/* The one privileged operation this static site cannot do itself: deleting another user's
 * Firebase Auth identity. Calls the operator-only endpoint in delete-user-api/ (a separate Vercel
 * project -- see its own file for why: no client SDK call can delete a different uid's account,
 * only the Admin SDK can, which only runs in a trusted server, not here). The bearer token proves
 * to that server who is actually asking, the same way Firebase's own rules trust auth.uid. */
async function deleteUserAccount(uid) {
  const url = window.DELETE_USER_API_URL;
  if (!url || url.includes("YOUR-PROJECT")) {
    throw new Error("Delete-user API is not configured yet -- set window.DELETE_USER_API_URL in firebase-config.js.");
  }
  const idToken = await auth.currentUser.getIdToken();
  const response = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", "Authorization": `Bearer ${idToken}` },
    body: JSON.stringify({ uid })
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message = result.error || `Request failed (${response.status})`;
    // The login is already gone (deleted in the Firebase console, say), so the API's deleteUser() fails
    // before it reaches its own cleanup, and the row could never be removed from this list. Only the record
    // is left, and the rules let an operator remove that directly.
    if (/no user record/i.test(message)) {
      await db.ref(`users/${uid}`).remove();
      return { success: true, recordOnly: true };
    }
    throw new Error(message);
  }
  return result;
}
/* Bug report (2026-09-10): granting temp access failed with PERMISSION_DENIED for BOTH operator
 * accounts. Traced to this machine's system clock running ~18.5 hours behind real time (confirmed
 * against an external HTTPS Date header) -- Date.now() + TEMP_ACCESS_DURATION_MS landed the computed
 * tempAccessUntil roughly 17.5 hours in the PAST relative to Firebase's real server clock, which the
 * rules' own server-relative validate window (firebase-rules.json's tempAccessUntil ".validate")
 * correctly rejected. Fixing the PC's clock fixes this, but relying on every operator's machine
 * having an accurate clock is fragile for something a security rule specifically checks -- so this
 * bootstraps off Firebase's OWN clock instead of ever trusting the browser's: the first write
 * resolves to the server's real "now" (trivially inside the validate window, since the rule's own
 * `now` and the resolved value are the same instant), then the second write is server-now-at-readback
 * plus the duration -- neither write ever depends on this browser's Date.now(). */
async function grantTempAccess(uid) {
  const ref = db.ref(`users/${uid}/tempAccessUntil`);
  await ref.set(firebase.database.ServerValue.TIMESTAMP);
  const snap = await ref.once("value");
  const serverNow = Number(snap.val());
  if (!Number.isFinite(serverNow)) throw new Error("Could not read back the server's clock.");
  await ref.set(serverNow + TEMP_ACCESS_DURATION_MS);
}
// Keeps expandedActivityUids (above) in sync with the actual <details> elements the operator opens/
// closes -- registered with capture:true because the native `toggle` event does not bubble, so a
// plain (bubbling-phase) listener on this ancestor would never see it; capture delivery happens on
// the way DOWN to the target and fires regardless of whether the event bubbles back up afterward.
document.getElementById("usersContainer")?.addEventListener("toggle", event => {
  const uid = event.target?.dataset?.uid;
  if (!uid) return;
  if (event.target.open) expandedActivityUids.add(uid); else expandedActivityUids.delete(uid);
}, true);
document.getElementById("usersContainer")?.addEventListener("click", event => {
  const answer = event.target.closest("[data-request-answer]");
  if (answer) {
    answer.disabled = true;
    answerAccessRequest(answer.dataset.uid, answer.dataset.kind, answer.dataset.requestAnswer === "accept")
      .catch(error => showToast(`Could not answer the request: ${error.message}`, "error"))
      .finally(() => { answer.disabled = false; });
    return;
  }
  const button = event.target.closest("[data-user-action]");
  if (!button) return;
  const uid = button.dataset.uid;
  const action = button.dataset.userAction;
  if (!uid || !action || OPERATOR_UIDS.includes(uid) || uid === ESP1_DEVICE_UID) return; // matches renderUserManagement()'s own guard
  const row = button.closest(".user-row");
  const name = row?.querySelector(".user-row-info strong")?.textContent || "this account";
  const email = row?.dataset.email || "";
  // Each change is also kept in this operator's own recent actions.
  const run = (write, done) => {
    button.disabled = true;
    write()
      .then(() => { if (done) logActivity(`${done}: ${name}`.slice(0, 80)); })
      .catch(error => showToast(`Could not update this account: ${error.message}`, "error"))
      .finally(() => { button.disabled = false; });
  };
  // Control and Edit zones (2026-11-05). Giving one also clears a waiting request for it.
  if (action === "givezones" || action === "givecontrol") {
    const zones = action === "givezones";
    run(() => db.ref().update({
      [`users/${uid}/${zones ? "zoneEditAccess" : "controlAccess"}`]: true,
      [`accessRequests/${uid}/${zones ? "zones" : "control"}`]: null
    }), zones ? "Gave zone editing" : "Gave control");
    return;
  }
  if (action === "takezones" || action === "takecontrol") {
    const zones = action === "takezones";
    if (!confirm(zones
      ? `Take back ${name}'s zone editing? They can still see every column, and can ask again.`
      : `Take back ${name}'s control access? They lose the Controls and Diagnostics tabs and the Emergency stop, and can ask again.`)) return;
    run(() => db.ref(`users/${uid}/${zones ? "zoneEditAccess" : "controlAccess"}`).remove(), zones ? "Took back zone editing" : "Took back control");
    return;
  }

  if (action === "delete") {
    // Deliberately the strongest confirmation of the three (a plain confirm() for the others) --
    // this is the one truly irreversible action. Routed through delete-user-api/ (a separate
    // Vercel project), not a direct database write: no client-side API can delete another uid's
    // Firebase Auth identity, only the Admin SDK can, which only runs there. That endpoint deletes
    // both the Auth account and /users/{uid} in one trusted call.
    const typed = prompt(`Delete ${name}? Their account (login and all) will be permanently deleted -- they would need to sign up again as a brand-new account. This cannot be undone.\n\nType the account's email to confirm:\n${email}`);
    if (typed === null || typed.trim() !== email) return;
    run(() => deleteUserAccount(uid).then(() => db.ref(`accessRequests/${uid}`).remove().catch(() => {})), "Deleted");
    return;
  }
  if (action === "kick") {
    // Defense-in-depth re-check: renderUserManagement() already omits this button once offline,
    // but re-verify against the live presence data at click time too, in case this row was mid-
    // re-render (a stale "Kick" button briefly clickable is otherwise possible in that gap).
    if (!presenceStatus(uid).online) { showToast(`${name} has no page open, so there is nobody to sign out.`, "error"); return; }
    if (!confirm(`Sign ${name} out now? They can sign in again straight away.`)) return;
    run(() => db.ref(`users/${uid}`).update({ kickToken: firebase.database.ServerValue.TIMESTAMP }), "Signed out");
    return;
  }
  if (action === "granttemp") {
    // Time-bounded elevation, not a role change -- status stays "approved" throughout. Enforced
    // server-side too: firebase-rules.json's tempAccessUntil validator caps this at the same
    // TEMP_ACCESS_DURATION_MS from the moment the write actually lands, so a slow request can't
    // grant longer than intended. grantTempAccess() anchors to Firebase's own server clock rather
    // than this browser's Date.now() -- see its own comment (bug report, 2026-09-10) for why.
    if (!confirm(`Give ${name} 1-hour access to Manual/Test, System and the rig lock? It ends by itself after an hour.`)) return;
    run(() => grantTempAccess(uid), "Gave 1-hour access");
    return;
  }
  if (action === "revoketemp") {
    run(() => db.ref(`users/${uid}`).update({ tempAccessUntil: null }), "Ended 1-hour access");
    return;
  }
  if (action === "grantsubop") {
    // Persistent elevation, not a role change -- status stays "approved" throughout, same as temp
    // access above. No expiry: stays in effect until an operator withholds it again. Given by an operator, never
    // asked for (the owner's design): a Sub-operator has no request buttons.
    if (!confirm(`Make ${name} a Sub-operator? They can use Manual/Test, System (including the AC voltage and current limits) and the rig lock until you remove it.`)) return;
    run(() => db.ref().update({
      [`users/${uid}/subOperator`]: true,
      [`accessRequests/${uid}/zones`]: null,
      [`accessRequests/${uid}/control`]: null
    }), "Made Sub-operator");
    return;
  }
  if (action === "withholdsubop") {
    run(() => db.ref(`users/${uid}`).update({ subOperator: null }), "Removed Sub-operator");
    return;
  }
  const statusMap = { approve: "approved", reject: "rejected", block: "disabled", unblock: "approved", restrict: "restricted", unrestrict: "approved" };
  const newStatus = statusMap[action];
  if (!newStatus) return;
  if (action === "block" && !confirm(`Block ${name}? They will immediately lose access.`)) return;
  if (action === "restrict" && !confirm(`Restrict ${name}? They keep their account, but can't see live readings or use any control until you unrestrict them.`)) return;
  // Deep-scan finding (2026-09-10): reject/block/restrict previously left any outstanding
  // tempAccessUntil grant untouched -- re-approving the account before that grant's own clock ran
  // out (up to an hour later) would silently hand privileged access straight back with no fresh
  // grant from an operator. Clear it alongside any status change that revokes approval. Sub-operator
  // (2026-09-10) is persistent rather than time-limited, so this matters even more for it -- without
  // this, re-approving a once-blocked sub-operator would silently hand the elevation straight back
  // with no fresh grant, indefinitely, not just within some grant's remaining hour.
  // Control and Edit zones (2026-11-05) go the same way. A status change also answers every request the account has
  // made: an approval or an unrestrict is what it asked for, and anything else no longer applies.
  const updates = { [`users/${uid}/status`]: newStatus };
  if (newStatus !== "approved") {
    ["tempAccessUntil", "subOperator", "controlAccess", "zoneEditAccess"].forEach(field => { updates[`users/${uid}/${field}`] = null; });
  }
  Object.keys(REQUEST_WORDS).forEach(kind => { updates[`accessRequests/${uid}/${kind}`] = null; });
  const done = { approve: "Approved", reject: "Rejected", block: "Blocked", unblock: "Unblocked", restrict: "Restricted", unrestrict: "Unrestricted" }[action];
  run(() => db.ref().update(updates), done);
});

function writeZoneProfile(zone) {
  if (!currentUserIsSignedIn()) return;
  // Audit fix (2026-09-08): every other write path (queueCommand, writeManualHold) gates on
  // isApprovedUser(), not just sign-in. The Firebase rule already rejects this write for a
  // non-approved account either way, so there was no security gap -- but a pending/rejected account
  // changing a crop/stage dropdown got a raw PERMISSION_DENIED error instead of the same friendly
  // "awaiting approval" message shown everywhere else. Since 2026-11-05 it needs Edit zones, as the rules do.
  if (!hasZoneEditAccess()) {
    setCommandStatus(NO_ZONE_EDIT_TITLE, "error");
    return;
  }
  // update(), not set(): the column's "Firmware save settings" library lives under this same node, and
  // set() wiped it every time someone changed the crop or stage.
  db.ref(`irrigation/config/zones/${zone.id}`).update({
    name: zone.name,
    crop: zone.defaultCrop,
    stage: zone.defaultStage,
    updatedAt: firebase.database.ServerValue.TIMESTAMP
  }).catch(error => setCommandStatus(`Could not save dashboard-only zone profile: ${error.message}`, "error"));
}

// Best-effort per-account activity trail (2026-09-10), operator-viewing-only -- see
// renderUserManagement(). A failure here must never affect the action it records, so it has its own
// silent catch. auth.currentUser can legitimately be null by the time this runs, if the user signed out
// just after the action; skip the entry rather than throw inside the caller's promise chain.
// Since 2026-11-05 it records every change made on the website (sign-ins, column and crop edits, requests, account
// decisions, Manual/Test), not only rig commands. Returns the write, so signing out can wait for its own entry.
function logActivity(what) {
  const myUid = auth?.currentUser?.uid;
  const text = String(what || "").trim().slice(0, 80);
  if (!myUid || !text) return Promise.resolve();
  return db.ref(`activity/${myUid}`).push({
    what: text,
    at: firebase.database.ServerValue.TIMESTAMP
  }).catch(error => console.warn("Could not log activity", error));
}

// These skip the freshness gate on purpose, because a stale snapshot is often why someone reaches for
// them. But the firmware runs any queued row whenever it next reads it, with no age limit, so sent while
// ESP1 is offline they would fire when it reconnects, possibly days later and into a changed situation.
// Stops are not listed: they aren't sent at all while ESP1 is offline (stopOfflineReason(), 6 Nov 2026).
const OFFLINE_CONFIRM_TYPES = new Set(["ENABLE_ACTUATIONS", "RECOVER", "ESTOP_RECOVER", "REBOOT"]);

// What this session has sent: id -> the reply last announced, and the toast showing the latest news. Only
// some panels show their own command's reply, and Command status is on the Live dashboard tab alone. ESP1
// doesn't publish a snapshot after most commands either, so without this a stop pressed on another tab
// could go up to a minute with no sign that it worked. Each reply replaces the toast before it: a stack of
// them can cover the buttons underneath, and it's only the latest news about a command that matters.
const sentCommands = new Map();
const COMMAND_END_STATUSES = new Set(["completed", "failed", "rejected", "withdrawn"]);
// Their effect only reaches the rest of the page in ESP1's next scheduled snapshot.
const SHOWN_IN_NEXT_REPORT = new Set(["EMERGENCY_STOP", "ESTOP_RECOVER", "ENABLE_ACTUATIONS", "DISABLE_ACTUATIONS", "SET_COLUMN", "SET_EXERCISE", "SET_CLOCK", "RESTORE_DEFAULTS", "CLEAR_BATT_SWITCH"]);

// ESP1 answers CANCEL_FORCE with "armed forced run cancelled" even if the run started a moment before, and the
// run carries on. A cancel takes up to 3 s to be picked up, so this is easy to hit near the end of a countdown.
// Only the run's own row and the snapshot can tell. The age limit is for rows left on "accepted" for good.
const FORCED_RUN_LONGEST_MS = 15 * 60 * 1000;
function forcedRunHasStarted() {
  if (liveData.diagnostics?.runProgress?.active === true) return true;
  return commandData.some(row => row.type === "FORCE_RUN" && row.status === "accepted"
    && String(row.detail || "").startsWith("force run started")
    && serverNow() - Number(row.updatedAt || 0) < FORCED_RUN_LONGEST_MS);
}

function announceCommandReplies() {
  sentCommands.forEach((sent, id) => {
    const command = commandData.find(row => row.id === id);
    if (!command || command.status === "queued" || command.status === "received") return;
    const reply = `${command.status}|${command.detail}`;
    if (reply === sent.reply) return;
    sent.reply = reply;
    if (COMMAND_END_STATUSES.has(command.status)) sentCommands.delete(id);
    dismissToast(sent.toast);
    const name = rawText(command.type, "Command").replaceAll("_", " ");
    if (command.status === "withdrawn") {
      setCommandStatus(`${name} withdrawn. ESP1 won't run it.`);
      return;
    }
    if (command.status === "completed" && command.type === "CANCEL_FORCE" && forcedRunHasStarted()) {
      sent.toast = setCommandStatus("Too late to cancel: ESP1 replied \"cancelled\", but the run had already started and is still going. Use Emergency stop to stop it.", "error");
      return;
    }
    let text = `${name}: ${command.status} — ${commandDetailText(command, "no detail from ESP1")}.`;
    if (command.status === "completed" && SHOWN_IN_NEXT_REPORT.has(command.type)) {
      text += " The rest of this page catches up with ESP1's next report, within about a minute.";
    }
    // ESP1 replies "completed" once it has acted on a recovery, even if the resume was then refused.
    if (command.status === "completed" && command.type === "RECOVER") {
      text += " That only means ESP1 acted on it. The fault banner shows the result in ESP1's next report, within about a minute.";
    }
    sent.toast = setCommandStatus(text, commandStatusTone(command.status));
  });
}

function queueCommand(type, payload = {}, options = {}) {
  const emergency = Boolean(options.emergency);
  if (!currentUserIsSignedIn()) {
    setCommandStatus("Sign in before sending a command.", "error");
    return Promise.resolve(false);
  }
  // The single choke point every command type funnels through -- covers every button on every tab,
  // including the fault banner's copies, without needing an approval check hunted down at each one
  // individually. Firebase rules enforce the identical check server-side; this is the client-side
  // half, for a clean message instead of a silent rules rejection.
  if (!isApprovedUser()) {
    setCommandStatus("Your account is awaiting operator approval before it can send commands to the rig.", "error");
    return Promise.resolve(false);
  }
  // Access levels (2026-11-05): a column's own settings need Edit zones, everything else Control (the rules agree).
  if (!options.operatorOnly && type === "SET_COLUMN" && !hasZoneEditAccess()) {
    setCommandStatus(`Not sent. ${NO_ZONE_EDIT_TITLE}`, "error");
    return Promise.resolve(false);
  }
  if (!options.operatorOnly && type !== "SET_COLUMN" && !hasControlAccess()) {
    setCommandStatus(`Not sent. ${NO_CONTROL_TITLE}`, "error");
    return Promise.resolve(false);
  }
  // Access-control revision (2026-09-09): a handful of command types (Manual/Test's pump exercise/
  // timed pulse/flow sweep, System's Set Clock/Thresholds/Restore Defaults) are meant for the two
  // privileged operator accounts -- or a normal approved account an operator has temporarily
  // elevated (canAccessPrivilegedTabs() = isOperator() || hasTempAccess()) -- never a plain approved
  // account on its own. Centralized in this one choke point rather than duplicated per call site,
  // matching every other gate in this function. As of the 2026-09-09 rules tightening, this is no
  // longer just a client-side convenience: firebase-rules.json's commands/$commandId write rule
  // independently enforces the identical operator-or-temp-access check for these six command types.
  if (options.operatorOnly && !canAccessPrivilegedTabs()) {
    setCommandStatus("This control is for operators, and accounts with 1-hour access or Sub-operator.", "error");
    return Promise.resolve(false);
  }
  // The rules refuse everything but the stops while ESP1 reports the rig locked: say so instead of a bare refusal.
  if (rigLocked() && !LOCK_EXEMPT.has(type)) {
    setCommandStatus(`${type.replaceAll("_", " ")} not sent: the rig is locked. Unlock it first (operators, and accounts with 1-hour access or Sub-operator, can).`, "error");
    return Promise.resolve(false);
  }
  // A stop is never queued while ESP1 isn't reading the queue (2026-11-06, stopOfflineReason()); the rules refuse it
  // too once ESP1's last report is over 3 minutes old.
  if (NOT_WITHDRAWABLE.has(type) && stopOfflineReason()) {
    const name = { EMERGENCY_STOP: "Emergency stop", DISABLE_ACTUATIONS: "Pumps and valves off", CANCEL_FORCE: "Cancel forced run" }[type];
    setCommandStatus(`${name} not sent. ${stopOfflineReason()}`, "error");
    return Promise.resolve(false);
  }
  // Firebase holds a write made while this browser is offline and sends it on reconnect, perhaps much later, and
  // ESP1 runs a queued row however old it is. So nothing is sent while the link is down, stops included.
  if (!firebaseConnected) {
    setCommandStatus(`${type.replaceAll("_", " ")} not sent: this browser has lost its connection to Firebase. Try again once it's back.`, "error");
    return Promise.resolve(false);
  }
  if (OFFLINE_CONFIRM_TYPES.has(type) && !deviceIsFresh() &&
      !confirm(`ESP1 appears offline. ${type.replaceAll("_", " ")} will wait in the queue and run whenever ESP1 reconnects, even if that is days from now. Queue it anyway?`)) {
    setCommandStatus(`${type.replaceAll("_", " ")} not sent.`);
    return Promise.resolve(false);
  }
  if (!emergency && !deviceIsFresh()) {
    setCommandStatus("Normal command not sent: ESP1 live status is stale or offline.", "error");
    return Promise.resolve(false);
  }
  if (!emergency) {
    const remaining = COMMAND_COOLDOWN_MS - (Date.now() - lastCommandAt);
    if (remaining > 0) {
      setCommandStatus(`Normal command not sent: wait ${Math.ceil(remaining / 1000)} seconds before another request.`, "error");
      return Promise.resolve(false);
    }
    lastCommandAt = Date.now();
  }
  const command = {
    type,
    payload,
    status: "queued",
    source: "dashboard",
    requestedAt: firebase.database.ServerValue.TIMESTAMP
  };
  // Who sent it, for the rig lock's banner. The rules only accept the sender's own name.
  if (options.byName) command.byName = options.byName;
  const toast = setCommandStatus(queuedStatusText(type, payload, emergency), "pending");
  const pushed = db.ref("irrigation/commands").push(command);
  sentCommands.set(pushed.key, { reply: "", toast });
  return pushed
    .then(reference => {
      if (!emergency) lastCommandAt = Date.now();
      logActivity(activitySummary(type, payload));
      return reference.key;
    })
    .catch(error => {
      sentCommands.delete(pushed.key);
      if (!emergency) lastCommandAt = 0;
      setCommandStatus(`Could not queue command: ${error.message}`, "error");
      return false;
    });
}

// For a panel with its own result line. queueCommand() can refuse to send (cooldown, stale snapshot), and
// without this the line would go on saying "Sending to ESP1…" although nothing was ever sent.
function sendFromPanel(show, type, payload = {}, options = {}) {
  show("Sending to ESP1…", false);
  return queueCommand(type, payload, options).then(key => {
    if (!key) show("Not sent. See the status message for the reason.", true);
    return key;
  });
}

// The "show" function sendFromPanel() needs, for a panel with a plain result line.
function resultLine(id) {
  return (text, error = false) => {
    const line = document.getElementById(id);
    if (!line) return;
    line.textContent = text;
    line.className = `control-result${error ? " error" : ""}`;
  };
}

// Short, human-readable "what did they do" label for the activity trail above -- deliberately NOT
// the full command detail (that lives in irrigation/commands itself, and needs ESP1's own reply to
// mean anything); just enough context to recognise the action at a glance in User Management.
// In plain words (2026-11-05); entries written before then keep the command names they were written with.
function activitySummary(type, payload) {
  const p = payload || {};
  const board = { nano: "the Nano", esp1: "ESP1", esp2: "ESP2" };
  switch (type) {
    case "FORCE_RUN":          return `Forced run: column ${rawText(p.columns, "?")}, ${numberText(p.liters, 1, "L")}`;
    case "SET_COLUMN":         return hasValue(p.enabled) && Object.keys(p).length === 2
                                 ? `Switched column ${rawText(p.col, "?")} ${Number(p.enabled) ? "on" : "off"}`
                                 : `Changed column ${rawText(p.col, "?")}'s settings`;
    case "TEST_PULSE":         return SERVO_TARGETS[p.target] ? `Moved the pump selector to the ${SERVO_TARGETS[p.target].toLowerCase()} side`
                                 : `Timed test: ${rawText(p.target, "?")} for ${rawText(p.seconds, "?")} s`;
    case "RUN_PUMP_TEST":      return `Exercised the ${rawText(p.pump, "?")} pump`;
    case "RECOVER":            return `Fault recovery: ${rawText(p.action, "?")}`;
    case "REBOOT":             return `Restarted ${board[p.target] || rawText(p.target, "?")}`;
    case "SET_EXERCISE":       return `Pump exercise ${p.exerciseEnabled ? "on" : "off"}, ${rawText(p.exerciseSeconds, "?")} s`;
    case "DIAG_SWEEP":         return "Read ESP2's sensors";
    case "EMERGENCY_STOP":     return "Emergency stop";
    case "ESTOP_RECOVER":      return "Return to normal after an emergency stop";
    case "ENABLE_ACTUATIONS":  return "Switched pumps and valves on";
    case "DISABLE_ACTUATIONS": return "Switched pumps and valves off";
    case "CANCEL_FORCE":       return "Cancelled a forced run";
    case "ACK_FAULT":          return "Asked to be reminded of a fault in 2 minutes";
    case "SET_CLOCK":          return "Set ESP1's clock";
    case "SET_THRESH":         return `Watering thresholds: start ${rawText(p.thStart, "?")} %, gap ${rawText(p.thGap, "?")}`;
    case "SET_AC_VOLT":        return `AC voltage limits: ${rawText(p.vMin, "?")}–${rawText(p.vMax, "?")} V`;
    case "SET_AC_AMP":         return `AC current limits: ${ampsFromTenthsText(p.aMin10)}–${ampsFromTenthsText(p.aMax10)} A`;
    case "RESTORE_DEFAULTS":   return "Restored factory settings";
    case "CLEAR_BATT_SWITCH":  return "Cleared the battery switch fault";
    case "LCD_LOCK":           return p.action === "unlock" ? "Unlocked the rig" : "Locked the rig";
    default:                   return type;
  }
}

// The status line shown the instant a command is queued -- before ESP1 has even seen it, let alone
// acted on it. Every branch must say "queued", never claim an outcome. Previously this only branched
// on the {emergency:true} flag, so RECOVER and ESTOP_RECOVER -- which also need that flag, to bypass
// the freshness/cooldown gate during a fault -- displayed the literal text "Emergency stop queued"
// regardless of which recovery button (Hold/Release/Only irrigate/Resume normal/Return to normal) was
// actually pressed.
function queuedStatusText(type, payload, emergency) {
  // A stop only gets this far while ESP1 is reporting (stopOfflineReason() in queueCommand()).
  if (type === "EMERGENCY_STOP") return "Emergency stop queued. This is not physical-stop confirmation; wait for ESP1 status.";
  if (type === "RECOVER") return `Recovery (${rawText(payload.action, "?")}) queued. This is not confirmation the rig has resumed; wait for ESP1 status.`;
  if (type === "ESTOP_RECOVER") return "Return to normal queued. This is not confirmation ESP1 has recovered; wait for ESP1 status.";
  if (emergency) return `${type.replaceAll("_", " ")} queued. This is not confirmation it has taken effect; wait for ESP1 status.`;
  return `${type.replaceAll("_", " ")} queued. Waiting for ESP1 validation.`;
}

/* ---- Zone profiles: one card per column --------------------------------------------------------
 * Each card says what the column's soil reads now and what ESP1 does with it, in words, with an ON/OFF
 * switch and an editor that sends ESP1 only what was changed (redesign, 2026-11-05: this replaced the
 * Auto / Irrigation only / Off buttons, which duplicated the form, and a schedule whose times only
 * appeared after picking "Manual window"). The values shown are ESP1's own, from its snapshot, plus any
 * change ESP1 has confirmed since (zoneSettings()), so a save or a switch shows at once instead of at
 * ESP1's next report, up to a minute later. */

// ESP1's built-in watering windows (DEF_WIN_START / DEF_WIN_END in its main.cpp), in minutes after
// midnight. ESP1 only publishes the window a column is using, so the page keeps the defaults to name
// them while a column is on its own time.
const DEFAULT_WINDOWS = { A: [360, 480], B: [480, 600], C: [360, 480] };
const MODE_TEXT = { AUTO: "Water + nutrients when the soil needs them", IRRIGATION_ONLY: "Water only" };
// ESP1's own limits for a target (columnTargetInRange() in its main.cpp).
const TARGET_FIELDS = [
  { key: "targetN",  input: "cfgN",  label: "N",  unit: "ppm", min: 0, max: 2000, step: "1" },
  { key: "targetP",  input: "cfgP",  label: "P",  unit: "ppm", min: 0, max: 2000, step: "1" },
  { key: "targetK",  input: "cfgK",  label: "K",  unit: "ppm", min: 0, max: 2000, step: "1" },
  { key: "targetPH", input: "cfgPH", label: "pH", unit: "",    min: 3, max: 9,    step: "0.1" }
];
// A snapshot can carry state from a moment before its own stamp, so a confirmed change stays on show
// until a snapshot this much newer than ESP1's reply.
const ZONE_CONFIRM_SETTLE_MS = 3000;
// column id -> the editor's values while it is open. Kept here, not in the form, because a zone config
// change anywhere (a crop picked on another card, a setup saved) rebuilds every card.
const zoneDrafts = {};
// column id -> the SET_COLUMN key of a switch press still waiting for ESP1.
const zonePowerSent = {};
// column id -> the name being typed while that column is renamed (website only).
const zoneRenames = {};
const ZONE_NAME_MAX = 40;
// column id -> the card's last note about a website-only step (a rename, a saved or loaded setup): { text, error, at }.
// Kept here because the zones listener rebuilds every card, and fires a second time once the server fills in
// updatedAt. Shown until a SET_COLUMN outcome newer than it (renderColumnCommandResults()).
const zoneNotes = {};

function clearZoneDrafts() {
  Object.keys(zoneDrafts).forEach(id => delete zoneDrafts[id]);
  Object.keys(zonePowerSent).forEach(id => delete zonePowerSent[id]);
  Object.keys(zoneRenames).forEach(id => delete zoneRenames[id]);
  Object.keys(zoneNotes).forEach(id => delete zoneNotes[id]);
}

// The card's result line, for a note that should outlive the card being rebuilt.
function zoneNote(id) {
  const show = resultLine(`cfgResult${id}`);
  return (text, error = false) => {
    zoneNotes[id] = { text, error, at: serverNow() };
    show(text, error);
  };
}

// Saves a column's website name with its crop choice (config/zones/{id}). The rules allow any approved account, the
// same as the crop choice, and check the length.
function saveZoneName(zone) {
  const show = resultLine(`cfgResult${zone.id}`);       // about the open form: no need to outlive it
  const name = String(zoneRenames[zone.id] ?? "").trim();
  if (!name) { show("Type a name first. Nothing was saved.", true); return; }
  if (name.length > ZONE_NAME_MAX) { show(`Names are up to ${ZONE_NAME_MAX} characters. Nothing was saved.`, true); return; }
  delete zoneRenames[zone.id];
  if (name === zone.name) { renderZonesUI(); return; }
  zone.name = name;
  zoneNote(zone.id)(`Renamed to "${name}". The new name is on this website only: the LCD and SMS keep ESP1's own name for the column.`);
  writeZoneProfile(zone);
  logActivity(`Renamed column ${zone.id} to "${name}"`);
  renderZonesUI();
  document.getElementById(`zoneRename${zone.id}`)?.focus();
}

function numberOrNull(value) {
  return hasValue(value) ? Number(value) : null;
}

// ESP1 from 5 Nov 2026 can water a column in up to MAX_RUNS_PER_DAY windows a day, and says so by publishing each
// column's "runs". Older firmware waters once a day in one window, and only understands winStart/winEnd.
const MAX_RUNS_PER_DAY = 4;
let zoneCardsMultiRun = false;     // whether the cards were last built with the several-times editor
function multiRunSupported() {
  return Object.values(liveData.sensors?.zones || {}).some(z => hasValue(z?.runs));
}
function windowPairs(list) {
  if (!Array.isArray(list)) return null;
  const pairs = list.filter(p => Array.isArray(p) && p.length === 2 && hasValue(p[0]) && hasValue(p[1])).map(([a, b]) => [Number(a), Number(b)]);
  return pairs.length ? pairs : null;
}

// What ESP1 has for a column right now.
function zoneSettings(id) {
  const z = liveData.sensors?.zones?.[id] || {};
  const s = {
    known: hasValue(z.enabled),
    enabled: z.enabled === true || Number(z.enabled) === 1,
    mode: z.mode === "AUTO" || z.mode === "IRRIGATION_ONLY" ? z.mode : null,
    sched: hasValue(z.schedMode) ? Number(z.schedMode) : null,
    ws: numberOrNull(z.winStart),
    we: numberOrNull(z.winEnd),
    // Every window in use (several a day since 5 Nov 2026); from older firmware, the one it has.
    windows: windowPairs(z.windows),
    runsToday: numberOrNull(z.runsToday),
    fedToday: z.fedToday === true,
    targets: Object.fromEntries(TARGET_FIELDS.map(f => [f.key, numberOrNull(z[f.key])]))
  };
  if (!s.windows && s.ws !== null && s.we !== null) s.windows = [[s.ws, s.we]];
  // ESP1 applies a SET_COLUMN all at once or not at all, so a completed one is exactly what it now has.
  const stamp = Number(liveData.meta?.updatedAt || 0);
  commandData
    .filter(c => c.type === "SET_COLUMN" && c.status === "completed" && c.payload?.col === id &&
                 Number(c.updatedAt || 0) + ZONE_CONFIRM_SETTLE_MS > stamp)
    .sort((a, b) => Number(a.updatedAt || 0) - Number(b.updatedAt || 0))
    .forEach(({ payload: p }) => {
      if (hasValue(p.enabled)) { s.known = true; s.enabled = p.enabled === true || Number(p.enabled) === 1; }
      if (p.mode === "AUTO" || p.mode === "IRRIGATION_ONLY") s.mode = p.mode;
      // ESP1 keeps the own times it was sent even while on the rig default, and reports the windows in use.
      const sentWindows = windowPairs(p.windows);
      if (sentWindows) { s.ownWindows = sentWindows; }
      else if (hasValue(p.winStart) || hasValue(p.winEnd)) {
        const first = s.ownWindows?.[0] || (s.sched === 1 ? s.windows?.[0] : null) || [s.ws, s.we];
        s.ownWindows = [[hasValue(p.winStart) ? Number(p.winStart) : first[0], hasValue(p.winEnd) ? Number(p.winEnd) : first[1]], ...(s.ownWindows || []).slice(1)];
      }
      if (hasValue(p.schedMode)) s.sched = Number(p.schedMode);
      TARGET_FIELDS.forEach(f => { if (hasValue(p[f.key])) s.targets[f.key] = Number(p[f.key]); });
    });
  // On the default window ESP1 reports the default, whatever own times it has kept; on its own times, those.
  if (s.sched === 0 && DEFAULT_WINDOWS[id]) s.windows = [DEFAULT_WINDOWS[id]];
  else if (s.sched === 1 && s.ownWindows) s.windows = s.ownWindows;
  delete s.ownWindows;
  if (s.windows) [s.ws, s.we] = s.windows[0];
  return s;
}

// "06:00–07:00", "06:00–07:00 and 12:00–13:00", "06:00–07:00, 12:00–13:00 and 17:00–18:00"
function windowListText(windows) {
  const parts = (windows || []).map(([a, b]) => `${minutesToHhmm(a)}–${minutesToHhmm(b)}`);
  return parts.length > 1 ? `${parts.slice(0, -1).join(", ")} and ${parts.at(-1)}` : parts[0] || "";
}
function scheduleText(id, sched, windows) {
  if (sched === 0) {
    const [ds, de] = DEFAULT_WINDOWS[id] || windows?.[0] || [null, null];
    return `${minutesToHhmm(ds)}–${minutesToHhmm(de)} (rig default)`;
  }
  if (sched === 1 && windows?.length) return `${windowListText(windows)} (${windows.length > 1 ? "my own times" : "my own time"})`;
  return "Unavailable";
}

function targetsText(targets) {
  if (!TARGET_FIELDS.every(f => hasValue(targets[f.key]))) return "Unavailable";
  const t = targets;
  const none = Number(t.targetN) === 0 && Number(t.targetP) === 0 && Number(t.targetK) === 0;
  return `N ${t.targetN}, P ${t.targetP}, K ${t.targetK} ppm, pH ${t.targetPH}` +
         (none ? ". None set, so nutrients are never added" : "");
}

// The same gate as every other control that reaches the rig (normalAllowed in syncControlAvailability()), as words:
// why a column's switch or Save to rig is off, or "" when they can be used.
function rigControlBlock() {
  if (!currentUserIsSignedIn() || !isApprovedUser()) return "Needs an approved account.";
  if (!hasZoneEditAccess()) return NO_ZONE_EDIT_TITLE;
  if (!firebaseConnected) return "This browser has lost its connection to Firebase.";
  if (!deviceIsFresh()) return "Needs a fresh ESP1 snapshot: ESP1 isn't reporting right now.";
  if (rigLocked()) return LOCKED_TITLE;
  return "";
}

// "Today: watered twice (10.0 L), nutrients once (12 mL)." from a column's zones.today, or "" when ESP1 doesn't send it.
function dayTallyText(t) {
  if (!t || !hasValue(t.waterings)) return "";
  const times = n => n === 1 ? "once" : n === 2 ? "twice" : `${n} times`;
  const n = Number(t.waterings) || 0;
  const f = Number(t.feedings) || 0;
  const ml = Number(t.ml) || 0;
  const water = n ? `watered ${times(n)} (${numberText(t.liters, 1, "L")})` : "not watered yet";
  const feed = f ? `, nutrients ${times(f)} (${numberText(ml, ml >= 10 ? 0 : 1, "mL")})` : "";
  return `Today: ${water}${feed}.`;
}

// The card's own words for what ESP1 does with the column. Runs on every snapshot and command update.
function renderZoneSummary(zone) {
  const id = zone.id;
  const s = zoneSettings(id);
  const block = document.querySelector(`.zone-block[data-zone="${id}"]`);
  if (block) block.classList.toggle("is-off", s.known && !s.enabled);
  setText(`zoneDoes${id}`, s.mode ? MODE_TEXT[s.mode] : "Unavailable");
  const runs = s.sched === 1 && s.windows ? s.windows.length : 1;
  setText(`zoneWhen${id}`, s.sched === null ? "Unavailable"
    : `${scheduleText(id, s.sched, s.windows)}, ${runs > 1 ? "at most once in each" : "once a day"}, when the soil is drier than Soil start %`);
  // How far through the day it is. Only ESP1 from 5 Nov 2026 reports it. "Done" rather than "watered": ESP1 also
  // closes the rest of the day on a STOP reply or a lost fault. A time that passed while the soil was wet stays open.
  const today = document.getElementById(`zoneToday${id}`);
  if (today) {
    const done = s.runsToday === null ? null : Math.min(s.runsToday, runs);
    const shown = done !== null && s.sched !== null && !(s.known && !s.enabled);
    today.hidden = !shown;
    if (shown) {
      // What reached the column today, scheduled and forced, from ESP1 from 6 Nov 2026 (zones.today). Older firmware
      // only says how many of the watering times are done.
      const tally = dayTallyText(liveData.sensors?.zones?.[id]?.today);
      const progress = tally
        ? (runs > 1 ? ` ${done} of ${runs} watering times done.` : done ? " Done for today." : "")
        : (runs === 1 ? (done ? "Done for today." : "Not watered so far today.") : `Today: ${done} of ${runs} watering times done.`);
      today.textContent = tally + progress +
        (s.fedToday && s.mode === "AUTO" && done < runs ? " Nutrients already went in today, so the rest of today's waterings are plain water." : "");
    }
  }
  setText(`zoneTargets${id}`, targetsText(s.targets));
  const offNote = document.getElementById(`zoneOffNote${id}`);
  if (offNote) offNote.hidden = !(s.known && !s.enabled);
  renderZonePower(id, s);
  if (zoneDrafts[id]) renderZoneChanges(id);
}

/* ---- ON/OFF switch ---- */
function renderZonePower(id, s = zoneSettings(id)) {
  const button = document.getElementById(`zonePower${id}`);
  const state = document.getElementById(`zonePowerState${id}`);
  const sentKey = zonePowerSent[id];
  const row = sentKey && commandData.find(c => c.id === sentKey);
  const waiting = Boolean(sentKey) && (!row || row.status === "queued" || row.status === "received");
  if (sentKey && !waiting) delete zonePowerSent[id];
  if (state) state.textContent = waiting ? "Sending…" : !s.known ? "Not reported" : s.enabled ? "On" : "Off";
  if (!button) return;
  button.setAttribute("aria-checked", String(s.known && s.enabled));
  button.dataset.waiting = waiting ? "1" : "";
  const block = rigControlBlock();
  button.disabled = waiting || Boolean(block);
  button.title = waiting ? "Waiting for ESP1." : block;
}

function toggleZonePower(id) {
  const s = zoneSettings(id);
  const result = resultLine(`cfgResult${id}`);
  if (!s.known) { result("ESP1 hasn't reported this column yet, so its switch can't be used.", true); return; }
  if (zonePowerSent[id]) return;
  const turnOn = !s.enabled;
  if (turnOn && !confirmEnableColumn(id)) { result("Nothing was sent."); return; }
  if (!turnOn && !confirm(`Turn column ${id} off? ESP1 stops watering it, and stops sending its readings here. Its settings are kept for when you turn it back on.`)) {
    result("Nothing was sent.");
    return;
  }
  sendFromPanel(result, "SET_COLUMN", { col: id, enabled: turnOn ? 1 : 0 }).then(key => {
    if (!key) return;
    zonePowerSent[id] = key;
    renderZonePower(id);
  });
}

// ESP1's column list is editable from here, but ESP2's is compiled into its firmware. Turning on a column
// that ESP2 has off makes ESP2 refuse every run for it: ESP1 raises COL_MISMATCH (a Major alert and SMS)
// and skips the column for the day.
function confirmEnableColumn(id) {
  return confirm(`Turn column ${id} on? Only do this if it is wired and plumbed, and ESP2's firmware has it enabled too: ESP2's column list is built into its firmware, and if it disagrees, ESP1 raises COL_MISMATCH and skips the column for the day.`);
}

/* ---- Edit settings ---- */
// The editor's watering-time boxes: window 1 keeps the ids it has always had, the rest are numbered (cfgWinStartA2).
function winInputId(prefix, id, w) {
  return w === 0 ? `${prefix}${id}` : `${prefix}${id}${w + 1}`;
}
const ORDINALS = ["1st", "2nd", "3rd", "4th"];
// ESP1's own defaults for watering times 2 to 4 (COL_EXTRA_START/END in its main.cpp).
const EXTRA_WINDOW_DEFAULTS = [[720, 780], [960, 1020], [1080, 1140]];
// Where a newly added watering time starts: ESP1's default for it when that comes after the time above, otherwise an
// hour long, an hour after the time above. Either way it is checked like a typed one.
function suggestWindow(w, prevEnd) {
  const def = EXTRA_WINDOW_DEFAULTS[w - 1];
  if (def && (prevEnd === null || def[0] > prevEnd)) return def;
  const start = Math.min((prevEnd ?? 0) + 60, 1438);
  return [start, Math.min(start + 60, 1439)];
}
function sameWindows(a, b) {
  return Boolean(a && b) && a.length === b.length && a.every(([s, e], i) => s === b[i][0] && e === b[i][1]);
}

function openZoneEditor(id) {
  const s = zoneSettings(id);
  const own = s.sched === 1 && s.windows;
  zoneDrafts[id] = {
    mode: s.mode || "",
    sched: s.sched === null ? "" : String(s.sched),
    // How many watering times, and every box's text, kept for all four so a row hidden by a lower count keeps
    // what was typed in it.
    runs: own ? Math.min(s.windows.length, MAX_RUNS_PER_DAY) : 1,
    wins: Array.from({ length: MAX_RUNS_PER_DAY }, (_, w) => own && s.windows[w] ? s.windows[w].map(minutesToHhmm) : ["", ""]),
    targets: Object.fromEntries(TARGET_FIELDS.map(f => [f.key, s.targets[f.key] === null ? "" : String(s.targets[f.key])])),
    saveName: "",
    setupsOpen: false
  };
  renderZonesUI();
  document.querySelector(`#zoneForm${id} input`)?.focus();
}

function closeZoneEditor(id) {
  delete zoneDrafts[id];
  renderZonesUI();
  document.getElementById(`zoneEdit${id}`)?.focus();
}

// Copies the draft into the form. Called whenever the card is built.
function fillZoneForm(id) {
  const d = zoneDrafts[id];
  const form = document.getElementById(`zoneForm${id}`);
  if (!d || !form) return;
  form.querySelectorAll(`input[name="cfgMode${id}"]`).forEach(radio => { radio.checked = radio.value === d.mode; });
  form.querySelectorAll(`input[name="cfgSched${id}"]`).forEach(radio => { radio.checked = radio.value === d.sched; });
  const runs = document.getElementById(`cfgRuns${id}`);
  if (runs) runs.value = String(d.runs);
  for (let w = 0; w < MAX_RUNS_PER_DAY; w++) fillWindowRow(id, w);
  TARGET_FIELDS.forEach(f => { const el = document.getElementById(`${f.input}${id}`); if (el) el.value = d.targets[f.key]; });
  const name = document.getElementById(`cfgSaveName${id}`);
  if (name) name.value = d.saveName;
  syncZoneTimeInputs(id);
  renderZoneChanges(id);
}

function fillWindowRow(id, w) {
  const d = zoneDrafts[id];
  const start = document.getElementById(winInputId("cfgWinStart", id, w));
  const end = document.getElementById(winInputId("cfgWinEnd", id, w));
  if (start) start.value = d.wins[w][0];
  if (end) end.value = d.wins[w][1];
}

// The times only mean something for "My own time", so they are greyed out otherwise. Only as many rows as
// "Waterings a day" asks for are shown.
function syncZoneTimeInputs(id) {
  const d = zoneDrafts[id];
  if (!d) return;
  const own = d.sched === "1";
  const runs = document.getElementById(`cfgRuns${id}`);
  if (runs) runs.disabled = !own;
  for (let w = 0; w < MAX_RUNS_PER_DAY; w++) {
    const row = document.getElementById(`cfgWinRow${id}${w + 1}`);
    if (row) row.hidden = w >= d.runs;
    ["cfgWinStart", "cfgWinEnd"].forEach(prefix => {
      const el = document.getElementById(winInputId(prefix, id, w));
      if (el) el.disabled = !own;
    });
  }
  document.getElementById(`zoneForm${id}`)?.classList.toggle("several-windows", d.runs > 1);
}

function readZoneForm(id) {
  const d = zoneDrafts[id];
  const form = document.getElementById(`zoneForm${id}`);
  if (!d || !form) return;
  d.mode = form.querySelector(`input[name="cfgMode${id}"]:checked`)?.value || d.mode;
  const sched = form.querySelector(`input[name="cfgSched${id}"]:checked`)?.value ?? d.sched;
  for (let w = 0; w < MAX_RUNS_PER_DAY; w++) {
    const start = document.getElementById(winInputId("cfgWinStart", id, w));
    const end = document.getElementById(winInputId("cfgWinEnd", id, w));
    if (start) d.wins[w][0] = start.value;
    if (end) d.wins[w][1] = end.value;
  }
  if (sched === "1" && d.sched !== "1" && !d.wins[0][0] && !d.wins[0][1]) {
    // Switching to own time starts from the window in use now, not from two blank boxes.
    const s = zoneSettings(id);
    const [ws, we] = s.sched === 1 && s.windows ? s.windows[0] : (DEFAULT_WINDOWS[id] || [360, 480]);
    d.wins[0] = [minutesToHhmm(ws), minutesToHhmm(we)];
    fillWindowRow(id, 0);
  }
  // A row added by a higher count starts from a suggestion, not blank boxes. Only rows that just appeared, so a box
  // being cleared to retype it is left alone.
  const runs = Number(document.getElementById(`cfgRuns${id}`)?.value || d.runs);
  for (let w = d.runs; w < runs; w++) {
    if (d.wins[w][0] || d.wins[w][1]) continue;
    d.wins[w] = suggestWindow(w, hhmmToMinutes(d.wins[w - 1][1])).map(minutesToHhmm);
    fillWindowRow(id, w);
  }
  d.runs = runs;
  d.sched = sched;
  TARGET_FIELDS.forEach(f => { const el = document.getElementById(`${f.input}${id}`); if (el) d.targets[f.key] = el.value.trim(); });
  d.saveName = document.getElementById(`cfgSaveName${id}`)?.value ?? d.saveName;
  syncZoneTimeInputs(id);
  renderZoneChanges(id);
}

// What the editor would change, as ESP1's own fields plus words for the person. Also checks each value
// against the firmware's limits, so a bad one is refused here rather than by ESP1.
function zoneEditPlan(id) {
  const d = zoneDrafts[id];
  const s = zoneSettings(id);
  const payload = { col: id };
  const words = [];
  const problems = [];
  if (d.mode && d.mode !== s.mode) {
    payload.mode = d.mode;
    words.push(`what it does, from "${s.mode ? MODE_TEXT[s.mode] : "unknown"}" to "${MODE_TEXT[d.mode]}"`);
  }
  if (d.sched === "1") {
    // ESP1 that can water several times a day takes them all at once as windows[]; older firmware only knows one.
    const multi = multiRunSupported();
    const count = multi ? d.runs : 1;
    const wins = d.wins.slice(0, count).map(([a, b]) => [hhmmToMinutes(a), hhmmToMinutes(b)]);
    // The same checks as ESP1's windowsValid(): each inside the day and starting before it ends, each ending before the
    // next starts. Times are whole minutes and both ends count, so 07:00 can't end one and start the next.
    const late = wins.findIndex(([a], w) => w > 0 && a <= wins[w - 1][1]);
    if (wins.some(([a, b]) => a === null || b === null)) {
      problems.push(count > 1 ? "Pick a start and an end for each watering time." : "Pick both a start and an end time for My own time.");
    } else if (wins.some(([a, b]) => a >= b)) {
      problems.push(`${count > 1 ? "Each watering time" : "The watering time"} must start before it ends, on the same day: the rig can't water past midnight.`);
    } else if (late > 0) {
      problems.push(`The ${ORDINALS[late]} watering time must start after the ${ORDINALS[late - 1]} one ends. Put them in order through the day, without overlapping.`);
    } else if (s.sched !== 1 || !sameWindows(wins, s.windows)) {
      payload.schedMode = 1;
      if (multi) payload.windows = wins;
      else { payload.winStart = wins[0][0]; payload.winEnd = wins[0][1]; }
      words.push(`when it runs, from ${scheduleText(id, s.sched, s.windows)} to ${scheduleText(id, 1, wins)}`);
    }
  } else if (d.sched === "0" && s.sched !== 0) {
    payload.schedMode = 0;
    words.push(`when it runs, from ${scheduleText(id, s.sched, s.windows)} to ${scheduleText(id, 0)}`);
  }
  TARGET_FIELDS.forEach(f => {
    const raw = d.targets[f.key];
    if (raw === "" || raw === undefined) return;               // blank: leave ESP1's value alone
    const v = Number(raw);
    if (!Number.isFinite(v) || v < f.min || v > f.max) { problems.push(`${f.label} must be a number from ${f.min} to ${f.max}.`); return; }
    if (s.targets[f.key] !== null && Math.abs(v - s.targets[f.key]) < 1e-6) return;
    payload[f.key] = v;
    words.push(`${f.label} target, from ${s.targets[f.key] ?? "unknown"} to ${v}${f.unit ? ` ${f.unit}` : ""}`);
  });
  return { payload, words, problems };
}

function renderZoneChanges(id) {
  const box = document.getElementById(`zoneChanges${id}`);
  if (!box || !zoneDrafts[id]) return;
  const { words, problems } = zoneEditPlan(id);
  box.classList.toggle("error", problems.length > 0);
  box.textContent = problems.length ? problems.join(" ")
    : words.length ? `You changed ${words.join("; ")}.`
    : "Nothing changed yet. ESP1 keeps what it has until you press Save to rig.";
  const save = document.getElementById(`cfgSave${id}`);
  if (save) save.dataset.nothing = words.length && !problems.length ? "" : "1";
  syncZoneSaveButton(id);
}

function syncZoneSaveButton(id) {
  const save = document.getElementById(`cfgSave${id}`);
  if (!save) return;
  const block = rigControlBlock();
  save.disabled = Boolean(block) || save.dataset.nothing === "1";
  save.title = block || (save.dataset.nothing === "1" ? "Change something first." : "");
}

function saveZoneEditor(id) {
  readZoneForm(id);
  const { payload, problems } = zoneEditPlan(id);
  const show = resultLine(`cfgResult${id}`);
  if (problems.length) { show(`${problems.join(" ")} Nothing was sent.`, true); return; }
  if (Object.keys(payload).length < 2) { show("Nothing changed, so nothing was sent."); return; }
  sendFromPanel(show, "SET_COLUMN", payload).then(key => { if (key) closeZoneEditor(id); });
}

// The crop profile is the page's own planning note; the rig only learns a target through SET_COLUMN. This
// copies the profile's numbers into the editor without sending, so they go through the same review.
function fillTargetsFromCrop(zone) {
  const id = zone.id;
  const show = resultLine(`cfgResult${id}`);
  const { crop, stage } = zoneCrop(zone);
  if (!crop || !stage || !zoneDrafts[id]) { show("That crop and stage has no stored profile. Nothing was filled in.", true); return; }
  Object.assign(zoneDrafts[id].targets, { targetN: String(stage.n), targetP: String(stage.p), targetK: String(stage.k), targetPH: String(stage.ph) });
  fillZoneForm(id);
  zoneNote(id)(`Filled from ${cropLabel(crop)}, ${stage.name}. Check them, then press Save to rig.`);
}

// Saved setups: the editor's values under a name, kept with the page's own zone config (ESP1 never reads
// it). Older saves, from the form this replaced, use the same fields; their "enabled" is ignored, because
// turning a column on or off is the switch's job.
function saveColumnSettings(id) {
  const show = resultLine(`cfgResult${id}`);
  if (!hasZoneEditAccess()) { show(NO_ZONE_EDIT_TITLE, true); return; }
  readZoneForm(id);
  const d = zoneDrafts[id];
  if (!d) return;
  const own = d.sched === "1";
  const snapshot = {
    name: d.saveName.trim() || `Saved ${new Date().toLocaleString()}`,
    savedAt: firebase.database.ServerValue.TIMESTAMP,
    mode: d.mode, schedMode: d.sched,
    // winStart/winEnd stay the first time, as older saves have it; "windows" carries every one (5 Nov 2026).
    winStart: own ? d.wins[0][0] : "", winEnd: own ? d.wins[0][1] : "",
    targetN: d.targets.targetN, targetP: d.targets.targetP, targetK: d.targets.targetK, targetPH: d.targets.targetPH
  };
  if (own) snapshot.windows = d.wins.slice(0, d.runs).map(([a, b]) => ({ from: a, to: b }));
  db.ref(`irrigation/config/zones/${id}/savedSettings`).push(snapshot)
    .then(() => {
      if (zoneDrafts[id]) zoneDrafts[id].saveName = "";
      const name = document.getElementById(`cfgSaveName${id}`);
      if (name) name.value = "";
      zoneNote(id)(`Saved as "${snapshot.name}". It isn't on the rig until you press Save to rig.`);
      logActivity(`Column ${id}: saved setup "${snapshot.name}"`);
    })
    .catch(error => show(`Could not save the setup: ${error.message}`, true));
}

function loadSavedSetup(zone, key) {
  const chosen = zone.savedSettings?.[key];
  const d = zoneDrafts[zone.id];
  if (!chosen || !d) return;
  if (chosen.mode === "AUTO" || chosen.mode === "IRRIGATION_ONLY") d.mode = chosen.mode;
  if (["0", "1"].includes(String(chosen.schedMode))) d.sched = String(chosen.schedMode);
  if (d.sched === "1") {
    // Firebase hands a saved list back as an array, or as an object when it has gaps; older saves have one time only.
    const saved = (Array.isArray(chosen.windows) ? chosen.windows : Object.values(chosen.windows || {}))
      .filter(t => t && typeof t === "object").slice(0, MAX_RUNS_PER_DAY)
      .map(t => [rawText(t.from, ""), rawText(t.to, "")]);
    const times = saved.length ? saved : [[rawText(chosen.winStart, ""), rawText(chosen.winEnd, "")]];
    // A save with more times than this ESP1 can take keeps only the first.
    d.runs = multiRunSupported() ? times.length : 1;
    times.forEach((t, w) => { d.wins[w] = t; });
  }
  TARGET_FIELDS.forEach(f => { if (hasValue(chosen[f.key])) d.targets[f.key] = String(chosen[f.key]); });
  fillZoneForm(zone.id);
  zoneNote(zone.id)(`Loaded "${rawText(chosen.name, "Untitled")}". Check it, then press Save to rig.`);
}

// Each column's latest SET_COLUMN outcome, under its own card, in words. ESP1's own reply follows in
// brackets, and the dashboard's Command status keeps the full record.
function renderColumnCommandResults() {
  activeZones.forEach(zone => {
    renderZoneSummary(zone);
    const result = document.getElementById(`cfgResult${zone.id}`);
    if (!result) return;
    const latest = commandData
      .filter(c => c.type === "SET_COLUMN" && c.payload && c.payload.col === zone.id)
      .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
    // A website-only note (a rename, a setup) stays until ESP1 says something newer about the column.
    const note = zoneNotes[zone.id];
    if (note && (!latest || note.at >= Math.max(Number(latest.requestedAt || 0), Number(latest.updatedAt || 0)))) {
      result.textContent = note.text;
      result.className = `control-result${note.error ? " error" : ""}`;
      return;
    }
    if (!latest) return;
    const reply = commandDetailText(latest, "");
    result.textContent = latest.status === "completed" ? `Saved on ESP1${reply ? ` (its reply: ${reply})` : ""}.`
      : latest.status === "rejected" || latest.status === "failed" ? `ESP1 refused it: ${reply || "no reason given"}.`
      : latest.status === "withdrawn" ? "Withdrawn before ESP1 read it."
      : "Sent. Waiting for ESP1…";
    result.className = `control-result${commandStatusTone(latest.status) === "error" ? " error" : ""}`;
  });
}

function renderZonesUI() {
  const container = document.getElementById("dynamic-zones-container");
  if (!container) return;
  container.innerHTML = "";
  // Access-control revision (2026-09-09): a pending/restricted account gets read-only text, not
  // controls that are simply expected to fail. The real boundary is still queueCommand() and the rules.
  const canEdit = hasZoneEditAccess();
  const multi = multiRunSupported();
  zoneCardsMultiRun = multi;
  activeZones.forEach(zone => {
    const id = zone.id;
    const block = document.createElement("article");
    block.className = "zone-block";
    block.dataset.zone = id;
    // Short words here; the full explanation is in the Help entry each icon links to.
    const info = (title, text, help) => `<button type="button" class="info-icon" aria-haspopup="dialog" aria-expanded="false" aria-label="About ${title}" data-info-title="${title}" data-info-text="${text}"${help ? ` data-help="${help}"` : ""}>i</button>`;
    const resolved = zoneCrop(zone);
    const crop = resolved.crop ? cropLabel(resolved.crop) : "--";
    const stage = resolved.stage ? resolved.stage.name : "--";
    const reading = (label, valueId, noteId, infoText, help, note = "Crop profile: --") => `
        <div class="reading">
          <dt>${label}${info(label, infoText, help)}</dt>
          <dd class="reading-value" id="${valueId}">Unavailable</dd>
          <dd class="reading-note"${noteId ? ` id="${noteId}"` : ""}>${note}</dd>
        </div>`;
    const draft = canEdit ? zoneDrafts[id] : null;
    const [defStart, defEnd] = DEFAULT_WINDOWS[id] || [null, null];

    const power = canEdit
      ? `<div class="zone-power">
          <span class="zone-power-state" id="zonePowerState${id}">Not reported</span>
          <button type="button" role="switch" aria-checked="false" class="switch zone-switch" id="zonePower${id}" aria-label="Column ${id} on or off"></button>
          ${info("Column on or off", "Off: ESP1 stops watering this column and stops sending its readings here. Its settings are kept for when you turn it back on.", "help-column-onoff")}
        </div>`
      : `<div class="zone-power"><span class="zone-power-state" id="zonePowerState${id}">Not reported</span></div>`;

    const compare = canEdit
      ? `<div class="zone-compare">
          <span class="label-row">Compared with${info("Crop profile", "Pick the crop and stage growing here. A reading turns red when it is below that crop's number. This stays on the website: ESP1 never sees it. The crops and their numbers are under Crop profiles, below the columns.", "help-crop-profile")}</span>
          <select id="cropSelect${id}" aria-label="Crop growing in column ${id}"></select>
          <select id="growthStage${id}" aria-label="Growth stage in column ${id}"></select>
        </div>`
      : `<p class="zone-compare">Compared with ${escapeHtml(crop)}, ${escapeHtml(stage)}</p>`;

    const summary = `
        <dl class="zone-summary">
          <dt>What it does${info("What it does", "Water + nutrients lets ESP1 add nutrients when the soil is short of them. Water only never adds any.", "help-what-it-does")}</dt>
          <dd id="zoneDoes${id}">Unavailable</dd>
          <dt>When it runs${info("When it runs", "The times of day this column may be watered: at most once in each, and only when its soil is dry enough.", "help-window")}</dt>
          <dd id="zoneWhen${id}">Unavailable</dd>
          <dd class="zone-today" id="zoneToday${id}" hidden></dd>
          <dt>Targets${info("Targets", "The N, P and K levels ESP1 aims for. It adds nutrients when a reading falls short of one by the Fertigation gap. pH is recorded, not corrected.", "help-npk")}</dt>
          <dd id="zoneTargets${id}">Unavailable</dd>
        </dl>
        ${canEdit ? `<button type="button" id="zoneEdit${id}" class="secondary">Edit settings</button>`
                  : `<p class="field-note">Read-only. Changing a column's settings needs access to edit zone profiles${isApprovedUser() && !isOperator() ? ", which you can ask for at the top of this tab" : ""}.</p>`}`;

    const form = `
        <form id="zoneForm${id}" class="zone-form" novalidate>
          <fieldset>
            <legend>What it does${info("What it does", "Water + nutrients lets ESP1 add nutrients when the soil is short of them. Water only never adds any.", "help-what-it-does")}</legend>
            <label class="choice"><input type="radio" name="cfgMode${id}" value="AUTO">Water + nutrients when the soil needs them</label>
            <label class="choice"><input type="radio" name="cfgMode${id}" value="IRRIGATION_ONLY">Water only</label>
          </fieldset>
          <fieldset>
            <legend>When it runs${info("When it runs", multi
              ? "Rig default uses the one window built into the firmware. My own time uses up to 4 times you pick, in order through the day, each ending the same day."
              : "Rig default uses the window built into the firmware. My own time uses the start and end you pick, and must end the same day.", "help-set-time")}</legend>
            <label class="choice"><input type="radio" name="cfgSched${id}" value="0">Rig default, ${minutesToHhmm(defStart)}–${minutesToHhmm(defEnd)}, once a day</label>
            <label class="choice"><input type="radio" name="cfgSched${id}" value="1">My own time</label>
            ${multi ? `<label class="runs-pick">Waterings a day<select id="cfgRuns${id}">${ORDINALS.map((_, w) => `<option value="${w + 1}">${w + 1}</option>`).join("")}</select></label>` : ""}
            ${Array.from({ length: multi ? MAX_RUNS_PER_DAY : 1 }, (_, w) => `
            <div class="time-row" id="cfgWinRow${id}${w + 1}" role="group" aria-label="${multi ? `${ORDINALS[w]} watering time` : "Watering time"}">
              ${multi ? `<span class="time-row-label" aria-hidden="true">${ORDINALS[w]}</span>` : ""}
              <label>From<input id="${winInputId("cfgWinStart", id, w)}" type="time"></label>
              <label>To<input id="${winInputId("cfgWinEnd", id, w)}" type="time"></label>
            </div>`).join("")}
            <p class="field-note">${multi
              ? "At most once in each time, and only when the soil is drier than Soil start %. Nutrients go in at most once a day: later waterings that day are plain water. Times follow the rig's clock."
              : "At most once a day, inside this time, when the soil is drier than Soil start %. Times follow the rig's clock."}</p>
          </fieldset>
          <fieldset>
            <legend>Targets${info("Targets", "N, P and K in ppm (0 to 2000), pH from 3 to 9. A blank box leaves ESP1's value as it is.", "help-targets")}</legend>
            <div class="target-row">
              ${TARGET_FIELDS.map(f => `<label>${f.label}${f.unit ? ` (${f.unit})` : ""}<input id="${f.input}${id}" type="number" min="${f.min}" max="${f.max}" step="${f.step}" inputmode="decimal"></label>`).join("")}
            </div>
            <button type="button" id="cfgFromCrop${id}" class="secondary">Fill from crop profile (${escapeHtml(crop)}, ${escapeHtml(stage)})</button>
          </fieldset>
          <details class="zone-setups" id="zoneSetups${id}">
            <summary>Saved setups${info("Saved setups", "Named copies of this form, kept on the website. Loading one fills the form; nothing reaches the rig until you press Save to rig.", "help-saved-setups")}</summary>
            <label>Load a saved setup<select id="cfgSavedList${id}"><option value="">Choose one</option></select></label>
            <div class="setup-save">
              <label>Save these as a setup<input id="cfgSaveName${id}" type="text" maxlength="40" placeholder="Name, for example Vegetative high-N"></label>
              <button type="button" id="cfgSaveSettingsBtn${id}" class="secondary">Save setup</button>
            </div>
          </details>
          <p class="zone-changes" id="zoneChanges${id}" aria-live="polite"></p>
          <div class="config-actions">
            <button type="submit" id="cfgSave${id}">Save to rig</button>
            <button type="button" id="cfgCancel${id}" class="secondary">Cancel</button>
          </div>
        </form>`;

    // Renaming (2026-11-05): the website's own name for the column, kept with its crop in config/zones. ESP1 keeps a
    // separate name (SMS NAME / the setup portal) that this doesn't touch, as the owner chose.
    const renaming = canEdit && zoneRenames[id] !== undefined;
    const nameLine = renaming
      ? `<form class="zone-rename" id="zoneRenameForm${id}" novalidate>
            <label>Name for column ${id}<input id="zoneRenameInput${id}" maxlength="${ZONE_NAME_MAX}" autocomplete="off"></label>
            <button type="submit">Save name</button>
            <button type="button" class="secondary" id="zoneRenameCancel${id}">Cancel</button>
          </form>`
      : `<p class="zone-sub">${escapeHtml(zone.name)}${canEdit ? ` <button type="button" class="inline-link" id="zoneRename${id}">Rename</button>` : ""}</p>`;
    block.innerHTML = `
      <header class="zone-head">
        <div>
          <h3>Column ${id}</h3>
          ${nameLine}
        </div>
        ${power}
      </header>
      <div class="zone-body">
        <section class="zone-now" aria-label="Column ${id}: soil now">
          <div class="zone-section-head">
            <h4>Soil now</h4>
            ${compare}
          </div>
          <p class="zone-off-note" id="zoneOffNote${id}" hidden>This column is off. ESP1 isn't watering it, and isn't sending its readings. Its settings are kept.</p>
          <dl class="reading-grid">
            <div class="reading reading-gauge">
              <dt>Soil moisture${info("Soil moisture", "The column's main moisture figure. ESP1 waters it when this is below Soil start %, the marked line on the gauge. The other mark is what the crop profile wants.", "help-moisture")}</dt>
              <dd class="gauge-wrap">${moistureGaugeSvg(id)}</dd>
              <dd class="reading-value" id="soil${id}">Unavailable</dd>
              <dd class="reading-note note-crop" id="targetMoisture${id}">Crop profile: --</dd>
              <dd class="reading-note note-start" id="soilStartNote${id}">Waters below 35 %</dd>
            </div>
            ${reading("Nitrogen (N)", `nitrogen${id}`, `targetN${id}`, "Nitrogen in the soil, in ppm, from the 7-in-1 probe. Red: below the crop profile's number.", "help-npk")}
            ${reading("Phosphorus (P)", `phosphorus${id}`, `targetP${id}`, "Phosphorus in the soil, in ppm. Red: below the crop profile's number.", "help-npk")}
            ${reading("Potassium (K)", `potassium${id}`, `targetK${id}`, "Potassium in the soil, in ppm. Red: below the crop profile's number.", "help-npk")}
            ${reading("Soil pH", `soilPH${id}`, `targetPH${id}`, "How acidic (under 7) or alkaline (over 7) the soil is.", "help-soil-ec")}
            ${reading("Soil EC", `soilEC${id}`, `targetEC${id}`, "Dissolved nutrients in the soil, in µS/cm. Higher means richer.", "help-soil-ec")}
            ${reading("NPK probe moisture", `npkMoist${id}`, "", "The NPK probe's own moisture reading. It counts toward Soil moisture when the two agree.", "help-moisture", "Counts toward soil moisture when it agrees")}
            ${reading("Soil temperature", `soilTemp${id}`, "", "Root-zone temperature, from the 7-in-1 probe.", "", "Root zone")}
          </dl>
        </section>
        <section class="zone-does" aria-label="Column ${id}: what ESP1 does with it">
          <div class="zone-section-head"><h4>What ESP1 does with it</h4></div>
          ${draft ? form : summary}
          <p id="cfgResult${id}" class="control-result" aria-live="polite"></p>
        </section>
      </div>`;
    container.appendChild(block);

    if (canEdit) {
      block.querySelector(`#zonePower${id}`)?.addEventListener("click", () => toggleZonePower(id));
      block.querySelector(`#zoneEdit${id}`)?.addEventListener("click", () => openZoneEditor(id));
      block.querySelector(`#zoneRename${id}`)?.addEventListener("click", () => {
        zoneRenames[id] = zone.name;
        renderZonesUI();
        document.getElementById(`zoneRenameInput${id}`)?.select();
      });
      const renameForm = block.querySelector(`#zoneRenameForm${id}`);
      if (renameForm) {
        const input = renameForm.querySelector(`#zoneRenameInput${id}`);
        input.value = zoneRenames[id];
        input.addEventListener("input", () => { zoneRenames[id] = input.value; });
        renameForm.addEventListener("submit", event => { event.preventDefault(); saveZoneName(zone); });
        block.querySelector(`#zoneRenameCancel${id}`)?.addEventListener("click", () => {
          delete zoneRenames[id];
          renderZonesUI();
          document.getElementById(`zoneRename${id}`)?.focus();
        });
      }
      const editor = block.querySelector(`#zoneForm${id}`);
      if (editor) {
        editor.addEventListener("input", () => readZoneForm(id));
        editor.addEventListener("change", () => readZoneForm(id));
        editor.addEventListener("submit", event => { event.preventDefault(); saveZoneEditor(id); });
        block.querySelector(`#cfgCancel${id}`)?.addEventListener("click", () => closeZoneEditor(id));
        block.querySelector(`#cfgFromCrop${id}`)?.addEventListener("click", () => fillTargetsFromCrop(zone));
        block.querySelector(`#cfgSaveSettingsBtn${id}`)?.addEventListener("click", () => saveColumnSettings(id));
        const setups = block.querySelector(`#zoneSetups${id}`);
        if (setups) {
          setups.open = Boolean(draft.setupsOpen);
          setups.addEventListener("toggle", () => { if (zoneDrafts[id]) zoneDrafts[id].setupsOpen = setups.open; });
        }
        // Newest first, capped at 10: a convenience list, not an archive. Every save stays in Firebase.
        const savedSelect = block.querySelector(`#cfgSavedList${id}`);
        Object.entries(zone.savedSettings || {})
          .sort((a, b) => (b[1]?.savedAt || 0) - (a[1]?.savedAt || 0))
          .slice(0, 10)
          .forEach(([key, s]) => savedSelect?.add(new Option(`${s?.name || "Untitled"} (N ${rawText(s?.targetN, "-")}, P ${rawText(s?.targetP, "-")}, K ${rawText(s?.targetK, "-")}, pH ${rawText(s?.targetPH, "-")})`, key)));
        savedSelect?.addEventListener("change", () => {
          const key = savedSelect.value;
          savedSelect.value = "";   // back to the prompt, so picking the same one again still fires "change"
          if (key) loadSavedSetup(zone, key);
        });
        fillZoneForm(id);
      }

      const cropSelect = block.querySelector(`#cropSelect${id}`);
      const stageSelect = block.querySelector(`#growthStage${id}`);
      cropLibrary().forEach(c => cropSelect.add(new Option(cropLabel(c), c.key, false, c.key === resolved.crop?.key)));
      populateStageOptions(zone, stageSelect);
      cropSelect.addEventListener("change", () => {
        zone.defaultCrop = cropSelect.value;
        zone.defaultStage = findCrop(zone.defaultCrop)?.stages[0]?.key || "";
        populateStageOptions(zone, stageSelect);
        updateZoneTargets(zone);
        writeZoneProfile(zone);
        const { crop, stage } = zoneCrop(zone);
        if (crop) logActivity(`Column ${id}: compared with ${cropLabel(crop)}${stage ? `, ${stage.name}` : ""}`);
      });
      stageSelect.addEventListener("change", () => {
        zone.defaultStage = stageSelect.value;
        updateZoneTargets(zone);
        writeZoneProfile(zone);
        const { stage } = zoneCrop(zone);
        if (stage) logActivity(`Column ${id}: growth stage ${stage.name}`);
      });
    }
    updateZoneTargets(zone);
  });
  // The cards just built are blank, and the next snapshot can be a minute away. This also runs when an
  // account's approval arrives, which is usually after the first snapshot.
  updateZoneReadings();
  renderColumnCommandResults();
}

// The crop's own stages, in their order. The column's saved stage isn't changed here even when it is missing (see
// zoneCrop()): the crop list can arrive after the columns, and a stage added on another device may not be here yet.
function populateStageOptions(zone, stageSelect) {
  stageSelect.innerHTML = "";
  const { crop, stage } = zoneCrop(zone);
  (crop?.stages || []).forEach(s => stageSelect.add(new Option(s.name, s.key, false, s.key === stage?.key)));
}

// The crop profile's reference numbers, under each reading. Planning only: ESP1's own targets are the
// card's Targets line.
function updateZoneTargets(zone) {
  const target = zoneCrop(zone).stage;
  if (!target) return;
  setText(`targetN${zone.id}`, `Crop profile: ${target.n} ppm`);
  setText(`targetP${zone.id}`, `Crop profile: ${target.p} ppm`);
  setText(`targetK${zone.id}`, `Crop profile: ${target.k} ppm`);
  setText(`targetPH${zone.id}`, `Crop profile: ${target.ph}`);
  setText(`targetEC${zone.id}`, `Crop profile: ${Math.round(target.ec * 1000)} µS/cm`);
  setText(`targetMoisture${zone.id}`, `Crop profile: ${target.moisture} %`);
}

function zoneMetric(zone, metric, id, digits, unit, targetKey) {
  const value = liveData.sensors?.zones?.[zone.id]?.[metric];
  const element = document.getElementById(id);
  if (!element) return;
  if (!hasValue(value)) {
    element.textContent = "Unavailable";
    element.classList.remove("lacking-nutrient");
    return;
  }
  element.textContent = numberText(value, digits, unit);
  const target = zoneCrop(zone).stage?.[targetKey];
  element.classList.toggle("lacking-nutrient", Number.isFinite(Number(target)) && Number(value) < Number(target));
}

// The 7-in-1 probe's EC register reaches the page as a whole number in µS/cm: the Nano sends it with no decimals
// and ESP1 doesn't scale it (calNpkScale in the firmware). The crop table holds mS/cm, as agronomy tables do.
function updateSoilEc(zone) {
  const element = document.getElementById(`soilEC${zone.id}`);
  if (!element) return;
  const value = liveData.sensors?.zones?.[zone.id]?.ec;
  if (!hasValue(value)) {
    element.textContent = "Unavailable";
    element.classList.remove("lacking-nutrient");
    return;
  }
  element.textContent = numberText(value, 0, "µS/cm");
  const target = zoneCrop(zone).stage?.ec;
  element.classList.toggle("lacking-nutrient", Number.isFinite(Number(target)) && Number(value) < Number(target) * 1000);
}

function updateZoneReadings() {
  activeZones.forEach(zone => {
    zoneMetric(zone, "nitrogen", `nitrogen${zone.id}`, 1, "ppm", "n");
    zoneMetric(zone, "phosphorus", `phosphorus${zone.id}`, 1, "ppm", "p");
    zoneMetric(zone, "potassium", `potassium${zone.id}`, 1, "ppm", "k");
    zoneMetric(zone, "ph", `soilPH${zone.id}`, 2, "", "ph");
    updateSoilEc(zone);
    zoneMetric(zone, "moisture", `soil${zone.id}`, 0, "%", "moisture");
    // No target key for these two: the probe's own moisture is shown for comparison against the
    // blended figure, and soil temperature has no configured target to fall short of.
    const z = liveData.sensors?.zones?.[zone.id] || {};
    setText(`npkMoist${zone.id}`, numberText(z.npkMoisture, 1, "%"));
    setText(`soilTemp${zone.id}`, numberText(z.soilTemperature, 1, "°C"));
    updateMoistureGauge(zone);
    renderZoneSummary(zone);
  });
}

/* Soil moisture gauge (2026-11-05): a half dial that fills to the reading, amber while the soil is drier than Soil start
 * % (so ESP1 would water it in its watering time), with that threshold and the crop profile's moisture marked. */
const GAUGE_CENTER = [60, 62];
const GAUGE_RADIUS = 50;
function moistureGaugeSvg(id) {
  const [cx, cy] = GAUGE_CENTER;
  const arc = `M${cx - GAUGE_RADIUS} ${cy} A${GAUGE_RADIUS} ${GAUGE_RADIUS} 0 0 1 ${cx + GAUGE_RADIUS} ${cy}`;
  return `<svg class="moisture-gauge" viewBox="0 0 120 70" aria-hidden="true" focusable="false">
      <path class="gauge-track" d="${arc}"/>
      <path class="gauge-fill unknown" id="gaugeFill${id}" d="${arc}" pathLength="100"/>
      <line class="gauge-tick gauge-start" id="gaugeStart${id}" x1="${cx}" y1="${cy}" x2="${cx}" y2="${cy}"/>
      <line class="gauge-tick gauge-crop" id="gaugeCrop${id}" x1="${cx}" y1="${cy}" x2="${cx}" y2="${cy}"/>
    </svg>`;
}
function placeGaugeTick(line, pct) {
  if (!line) return;
  const theta = Math.PI * (1 - Math.min(100, Math.max(0, pct)) / 100);
  const [cx, cy] = GAUGE_CENTER;
  const at = r => [cx + r * Math.cos(theta), cy - r * Math.sin(theta)];
  const [x1, y1] = at(GAUGE_RADIUS - 11);
  const [x2, y2] = at(GAUGE_RADIUS + 9);
  line.setAttribute("x1", x1.toFixed(1));
  line.setAttribute("y1", y1.toFixed(1));
  line.setAttribute("x2", x2.toFixed(1));
  line.setAttribute("y2", y2.toFixed(1));
}
function updateMoistureGauge(zone) {
  const fill = document.getElementById(`gaugeFill${zone.id}`);
  if (!fill) return;
  const z = liveData.sensors?.zones?.[zone.id] || {};
  const known = hasValue(z.moisture) && z.enabled !== false;
  const pct = known ? Math.min(100, Math.max(0, Number(z.moisture))) : 0;
  const start = soilStartPercent();
  fill.style.strokeDashoffset = String(100 - pct);
  fill.classList.toggle("dry", known && pct < start);
  fill.classList.toggle("unknown", !known);
  placeGaugeTick(document.getElementById(`gaugeStart${zone.id}`), start);
  const target = zoneCrop(zone).stage?.moisture;
  const crop = document.getElementById(`gaugeCrop${zone.id}`);
  if (crop) {
    crop.style.display = hasValue(target) ? "" : "none";
    if (hasValue(target)) placeGaugeTick(crop, Number(target));
  }
  setText(`soilStartNote${zone.id}`, `Waters below ${start} %${start === 35 ? "" : " (as last sent)"}`);
}

/* ---- Crop profiles: the library panel under the columns (2026-11-05) -------------------------------------------
 * Every crop with its stages and numbers. Operators and privileged accounts (1-hour access, Sub-operator) can add a
 * crop, change any crop, built-in ones too, remove one, and restore a built-in one; the rules enforce that and check
 * every value. A crop or a stage a column uses can't be removed until that column picks another (the owner's
 * choice). One editor at a time, kept in cropDraft so a list update from another device doesn't wipe it. */
let cropDraft = null;              // { key, isNew, originalName, name, variety, notes, stages: [...], tried }
let cropDraftFocus = "";           // what to focus once the editor is drawn: "name", or "stage:<index>"
let lastCropEditCapability = undefined;
const STAGE_FIELD_WORD = { n: "N", p: "P", k: "K", ph: "pH", ec: "EC", moisture: "Moisture", days: "Days" };

function canEditCrops() {
  return canAccessPrivilegedTabs();
}

// "A", "A and B", "A, B and C"
function columnList(ids) {
  return ids.length > 1 ? `${ids.slice(0, -1).join(", ")} and ${ids.at(-1)}` : ids[0] || "";
}
function columnsUsing(cropKey, stageKey) {
  return activeZones.filter(zone => zone.defaultCrop === cropKey && (stageKey === undefined || zone.defaultStage === stageKey)).map(zone => zone.id);
}
function columnWord(ids) {
  return `${ids.length > 1 ? "Columns" : "Column"} ${columnList(ids)}`;
}

function newStageKey() {
  return db ? db.ref().push().key : `st${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
}
function newDraftStage(name = "") {
  return { key: newStageKey(), name, n: "", p: "", k: "", ph: "", ec: "", moisture: "", days: "", notes: "" };
}

function cropNumber(value) {
  return hasValue(value) ? String(Number(value)) : "";
}

function stageTableHtml(crop) {
  const rows = crop.stages.map(s => `
          <tr>
            <th scope="row">${escapeHtml(s.name)}</th>
            <td>${escapeHtml(cropNumber(s.n))}</td><td>${escapeHtml(cropNumber(s.p))}</td><td>${escapeHtml(cropNumber(s.k))}</td>
            <td>${escapeHtml(cropNumber(s.ph))}</td><td>${escapeHtml(cropNumber(s.ec))}</td><td>${escapeHtml(cropNumber(s.moisture))}</td>
            <td>${escapeHtml(cropNumber(s.days))}</td>
            <td class="stage-notes">${escapeHtml(s.notes || "")}</td>
          </tr>`).join("");
  return `
      <div class="table-scroll">
        <table class="stage-table">
          <thead><tr><th scope="col">Stage</th><th scope="col">N (ppm)</th><th scope="col">P (ppm)</th><th scope="col">K (ppm)</th><th scope="col">pH</th><th scope="col">EC (mS/cm)</th><th scope="col">Moisture (%)</th><th scope="col">Days</th><th scope="col">Notes</th></tr></thead>
          <tbody>${rows}</tbody>
        </table>
      </div>`;
}

function cropCardHtml(crop, showActions) {
  const users = columnsUsing(crop.key);
  const origin = !crop.builtIn ? "Added on this website" : crop.changed ? "Built-in, changed" : "Built-in";
  const changed = crop.updatedAt
    ? `Last changed ${new Date(crop.updatedAt).toLocaleDateString()}${crop.updatedBy ? ` by ${crop.updatedBy}` : ""}` : "";
  const key = escapeHtml(crop.key);
  const actions = showActions ? `
        <div class="crop-actions">
          <button type="button" class="secondary" data-crop-edit="${key}">Edit</button>
          ${crop.builtIn && crop.changed ? `<button type="button" class="secondary" data-crop-restore="${key}">Restore original</button>` : ""}
          <button type="button" class="secondary" data-crop-remove="${key}">Remove</button>
        </div>` : "";
  return `
    <article class="crop-card" data-crop="${key}">
      <header class="crop-head">
        <div>
          <h3>${escapeHtml(crop.name)}${crop.variety ? ` <span class="crop-variety">${escapeHtml(crop.variety)}</span>` : ""}</h3>
          <p class="crop-meta">
            <span class="crop-badge${crop.builtIn ? "" : " own"}">${origin}</span>
            <span>${users.length ? `Used by ${columnWord(users).toLowerCase().replace(/ ([a-c])(?=\b)/g, m => m.toUpperCase())}` : "Not used by a column"}</span>
            ${changed ? `<span>${escapeHtml(changed)}</span>` : ""}
          </p>
        </div>${actions}
      </header>
      ${crop.notes ? `<p class="crop-notes">${escapeHtml(crop.notes)}</p>` : ""}
      ${stageTableHtml(crop)}
    </article>`;
}

function cropEditorHtml(draft) {
  const number = (field, label, min, max, step) =>
    `<label>${label}<input data-field="${field}" type="number" min="${min}" max="${max}" step="${step}" inputmode="decimal"></label>`;
  const stages = draft.stages.map((s, index) => `
          <div class="stage-edit" data-stage-index="${index}">
            <div class="stage-edit-head">
              <strong>Stage ${index + 1}</strong>
              <span class="stage-edit-actions">
                <button type="button" class="secondary small" data-stage-up="${index}"${index === 0 ? " disabled" : ""} aria-label="Move stage ${index + 1} up">Up</button>
                <button type="button" class="secondary small" data-stage-down="${index}"${index === draft.stages.length - 1 ? " disabled" : ""} aria-label="Move stage ${index + 1} down">Down</button>
                <button type="button" class="secondary small" data-stage-remove="${index}"${draft.stages.length === 1 ? " disabled" : ""} aria-label="Remove stage ${index + 1}">Remove</button>
              </span>
            </div>
            <div class="stage-fields">
              <label class="stage-name">Name<input data-field="name" maxlength="30" placeholder="e.g. Seedling"></label>
              ${number("n", "N (ppm)", 0, 2000, "any")}
              ${number("p", "P (ppm)", 0, 2000, "any")}
              ${number("k", "K (ppm)", 0, 2000, "any")}
              ${number("ph", "pH", 3, 9, "0.1")}
              ${number("ec", "EC (mS/cm)", 0, 20, "0.1")}
              ${number("moisture", "Moisture (%)", 0, 100, "1")}
              ${number("days", "Days (optional)", 0, 365, "1")}
            </div>
            <label class="stage-notes-field">Notes (optional)<input data-field="notes" maxlength="200" placeholder="e.g. Transplant at 3 to 4 true leaves"></label>
          </div>`).join("");
  return `
    <form class="crop-form" id="cropForm" novalidate>
      <h3>${draft.isNew ? "Add a crop" : `Change ${escapeHtml(draft.originalName)}`}</h3>
      <div class="crop-form-row">
        <label>Crop name<input id="cropName" maxlength="40" placeholder="e.g. Ampalaya"></label>
        <label>Variety or seedling type (optional)<input id="cropVariety" maxlength="40" placeholder="e.g. Galaxy F1"></label>
      </div>
      <label>Notes (optional)<textarea id="cropNotes" maxlength="300" rows="2" placeholder="e.g. Needs a trellis. Harvest 55 to 60 days after transplanting."></textarea></label>
      <fieldset class="stage-editor">
        <legend>Growth stages, in order. Each column picks one of these.</legend>
        ${stages}
        <button type="button" class="secondary" id="cropAddStage"${draft.stages.length >= MAX_STAGES ? " disabled" : ""}>Add a stage</button>
      </fieldset>
      <p class="zone-changes crop-problems" id="cropProblems" aria-live="polite" hidden></p>
      <div class="config-actions">
        <button type="submit" id="cropSave">Save crop</button>
        <button type="button" class="secondary" id="cropCancel">Cancel</button>
      </div>
    </form>`;
}

function fillCropForm() {
  const form = document.getElementById("cropForm");
  if (!form || !cropDraft) return;
  form.querySelector("#cropName").value = cropDraft.name;
  form.querySelector("#cropVariety").value = cropDraft.variety;
  form.querySelector("#cropNotes").value = cropDraft.notes;
  form.querySelectorAll(".stage-edit").forEach(row => {
    const stage = cropDraft.stages[Number(row.dataset.stageIndex)];
    row.querySelectorAll("[data-field]").forEach(input => { input.value = stage?.[input.dataset.field] ?? ""; });
  });
  renderCropProblems();
}

function readCropForm() {
  const form = document.getElementById("cropForm");
  if (!form || !cropDraft) return;
  cropDraft.name = form.querySelector("#cropName").value;
  cropDraft.variety = form.querySelector("#cropVariety").value;
  cropDraft.notes = form.querySelector("#cropNotes").value;
  form.querySelectorAll(".stage-edit").forEach(row => {
    const stage = cropDraft.stages[Number(row.dataset.stageIndex)];
    if (stage) row.querySelectorAll("[data-field]").forEach(input => { stage[input.dataset.field] = input.value; });
  });
}

// What stops the draft being saved, in words. The same limits as the rules on irrigation/config/crops.
function cropDraftProblems() {
  const draft = cropDraft;
  const problems = [];
  const name = draft.name.trim();
  if (!name) problems.push("Give the crop a name.");
  else if (cropLibrary().some(crop => crop.key !== draft.key && crop.name.trim().toLowerCase() === name.toLowerCase())) {
    problems.push(`There is already a crop called ${name}.`);
  }
  if (!draft.stages.length) problems.push("Add at least one stage.");
  const seen = new Set();
  draft.stages.forEach((stage, index) => {
    const stageName = stage.name.trim();
    const label = stageName || `Stage ${index + 1}`;
    if (!stageName) problems.push(`Stage ${index + 1} needs a name.`);
    else if (seen.has(stageName.toLowerCase())) problems.push(`Two stages are called ${stageName}.`);
    seen.add(stageName.toLowerCase());
    Object.entries(CROP_LIMITS).forEach(([field, [min, max]]) => {
      const raw = String(stage[field] ?? "").trim();
      if (field === "days" && raw === "") return;
      const value = Number(raw);
      if (raw === "" || !Number.isFinite(value) || value < min || value > max) {
        problems.push(`${label}: ${STAGE_FIELD_WORD[field]} must be a number from ${min} to ${max}.`);
      } else if (field === "days" && !Number.isInteger(value)) {
        problems.push(`${label}: Days must be a whole number.`);
      }
    });
  });
  // A stage a column uses stays until that column picks another one.
  if (!draft.isNew) {
    const kept = new Set(draft.stages.map(stage => stage.key));
    (findCrop(draft.key)?.stages || []).forEach(stage => {
      if (kept.has(stage.key)) return;
      const users = columnsUsing(draft.key, stage.key);
      if (users.length) problems.push(`${columnWord(users)} ${users.length > 1 ? "use" : "uses"} the ${stage.name} stage. Pick another stage for ${users.length > 1 ? "them" : "it"} on its card first, or keep the stage.`);
    });
  }
  return problems;
}

function renderCropProblems() {
  const box = document.getElementById("cropProblems");
  if (!box || !cropDraft) return;
  const problems = cropDraft.tried ? cropDraftProblems() : [];
  box.hidden = !problems.length;
  box.classList.toggle("error", problems.length > 0);
  box.textContent = problems.join(" ");
}

function renderCropLibrary() {
  const list = document.getElementById("cropList");
  if (!list) return;
  const canEdit = canEditCrops() && currentUserIsSignedIn();
  lastCropEditCapability = canEditCrops();
  if (!canEdit) cropDraft = null;
  // The crop being changed was removed on another device: its editor has nothing left to save into.
  if (cropDraft && !cropDraft.isNew && !findCrop(cropDraft.key)) {
    cropDraft = null;
    resultLine("cropLibraryResult")("The crop you were changing was removed on another device.", true);
  }
  const add = document.getElementById("cropAddBtn");
  if (add) { add.hidden = !canEdit; add.disabled = Boolean(cropDraft); }
  const note = document.getElementById("cropLibraryNote");
  if (note) note.hidden = canEdit || !currentUserIsSignedIn();
  const slot = document.getElementById("cropEditorNew");
  if (slot) slot.innerHTML = cropDraft?.isNew ? cropEditorHtml(cropDraft) : "";
  // A changed crop's editor takes its card's place. While any editor is open, the other cards have no buttons.
  list.innerHTML = cropLibrary().map(crop => cropDraft && !cropDraft.isNew && cropDraft.key === crop.key
    ? cropEditorHtml(cropDraft) : cropCardHtml(crop, canEdit && !cropDraft)).join("");
  const removed = removedBuiltIns();
  const box = document.getElementById("cropRemoved");
  if (box) {
    box.hidden = !removed.length;
    box.innerHTML = removed.length
      ? `<span>Removed built-in crops:</span> ${removed.map(crop => canEdit && !cropDraft
          ? `<button type="button" class="secondary small" data-crop-unremove="${escapeHtml(crop.key)}">Restore ${escapeHtml(crop.name)}</button>`
          : `<span class="crop-badge">${escapeHtml(crop.name)}</span>`).join(" ")}`
      : "";
  }
  if (!cropDraft) return;
  fillCropForm();
  const focus = cropDraftFocus;
  cropDraftFocus = "";
  if (focus === "name") document.getElementById("cropName")?.focus();
  else if (focus.startsWith("stage:")) document.querySelector(`.stage-edit[data-stage-index="${focus.slice(6)}"] [data-field="name"]`)?.focus();
}

function openCropEditor(key) {
  if (!canEditCrops() || cropDraft) return;
  const crop = key ? findCrop(key) : null;
  if (key && !crop) return;
  cropDraft = crop
    ? {
        key: crop.key, isNew: false, originalName: crop.name, name: crop.name, variety: crop.variety, notes: crop.notes, tried: false,
        stages: crop.stages.map(stage => ({
          key: stage.key, name: stage.name, n: cropNumber(stage.n), p: cropNumber(stage.p), k: cropNumber(stage.k), ph: cropNumber(stage.ph),
          ec: cropNumber(stage.ec), moisture: cropNumber(stage.moisture), days: cropNumber(stage.days), notes: stage.notes || ""
        }))
      }
    : { key: null, isNew: true, originalName: "", name: "", variety: "", notes: "", tried: false, stages: [newDraftStage("Seedling")] };
  cropDraftFocus = "name";
  resultLine("cropLibraryResult")("");
  renderCropLibrary();
}

function closeCropEditor() {
  const key = cropDraft?.key;
  cropDraft = null;
  renderCropLibrary();
  (key ? document.querySelector(`[data-crop-edit="${CSS.escape(key)}"]`) : document.getElementById("cropAddBtn"))?.focus();
}

function saveCropDraft() {
  readCropForm();
  if (!cropDraft) return;
  cropDraft.tried = true;
  renderCropProblems();
  if (cropDraftProblems().length) return;
  const show = resultLine("cropLibraryResult");
  if (!canEditCrops()) { show("Only operators and Sub-operators can add, change or remove crops.", true); return; }
  const draft = cropDraft;
  const key = draft.isNew ? db.ref("irrigation/config/crops").push().key : draft.key;
  const stages = {};
  draft.stages.forEach((stage, order) => {
    const saved = { name: stage.name.trim(), order };
    ["n", "p", "k", "ph", "ec", "moisture"].forEach(field => { saved[field] = Number(stage[field]); });
    if (String(stage.days).trim() !== "") saved.days = Number(stage.days);
    if (stage.notes.trim()) saved.notes = stage.notes.trim();
    stages[stage.key] = saved;
  });
  const entry = { name: draft.name.trim(), stages, updatedAt: firebase.database.ServerValue.TIMESTAMP };
  if (draft.variety.trim()) entry.variety = draft.variety.trim();
  if (draft.notes.trim()) entry.notes = draft.notes.trim();
  const by = myDisplayName();
  if (by) entry.updatedBy = by.slice(0, 80);
  const save = document.getElementById("cropSave");
  if (save) save.disabled = true;
  show(`Saving ${entry.name}…`);
  db.ref(`irrigation/config/crops/${key}`).set(entry)
    .then(() => {
      cropDraft = null;
      renderCropLibrary();
      renderZonesUI();
      show(draft.isNew ? `${entry.name} added. It is in every column's crop list now.` : `${entry.name} saved. Columns using it compare with its new numbers.`);
      logActivity(`${draft.isNew ? "Added" : "Changed"} crop profile ${entry.name}`);
    })
    .catch(error => {
      if (save) save.disabled = false;
      show(`Could not save ${entry.name}: ${error.code === "PERMISSION_DENIED" ? "only operators and Sub-operators can add, change or remove crops, and every value must be in range" : error.message}.`, true);
    });
}

function removeCrop(key) {
  const crop = findCrop(key);
  const show = resultLine("cropLibraryResult");
  if (!crop || !canEditCrops()) return;
  const users = columnsUsing(key);
  if (users.length) {
    show(`${columnWord(users)} ${users.length > 1 ? "use" : "uses"} ${crop.name}. Pick another crop for ${users.length > 1 ? "them" : "it"} on its card first, then remove it.`, true);
    return;
  }
  if (cropLibrary().length <= 1) { show("The crop list needs at least one crop.", true); return; }
  const question = crop.builtIn
    ? `Remove ${crop.name} from the crop list? It is a built-in crop, so it can be restored later${crop.changed ? ", as it was originally: your changes to it are lost" : ""}.`
    : `Remove ${crop.name}? This can't be undone.`;
  if (!confirm(question)) return;
  const ref = db.ref(`irrigation/config/crops/${key}`);
  const by = myDisplayName();
  const write = crop.builtIn
    ? ref.set({ removed: true, updatedAt: firebase.database.ServerValue.TIMESTAMP, ...(by ? { updatedBy: by.slice(0, 80) } : {}) })
    : ref.remove();
  write.then(() => { show(`${crop.name} removed.${crop.builtIn ? " Restore it under Removed built-in crops." : ""}`); logActivity(`Removed crop profile ${crop.name}`); })
    .catch(error => show(`Could not remove ${crop.name}: ${error.message}.`, true));
}

// Back to the website's own version of a built-in crop, whether it was changed or removed.
function restoreCrop(key) {
  const original = builtInCrop(key);
  const show = resultLine("cropLibraryResult");
  if (!original || !canEditCrops()) return;
  if (findCrop(key)) {
    const stranded = columnsUsing(key).filter(id => !original.stages.some(stage => stage.key === activeZones.find(zone => zone.id === id)?.defaultStage));
    if (stranded.length) {
      show(`${columnWord(stranded)} ${stranded.length > 1 ? "use" : "uses"} a stage the original ${original.name} doesn't have. Pick one of its original stages first: ${original.stages.map(stage => stage.name).join(", ")}.`, true);
      return;
    }
    if (!confirm(`Put ${original.name} back to the website's original stages and numbers? Your changes to it are lost.`)) return;
  } else if (cropLibrary().some(crop => crop.name.trim().toLowerCase() === original.name.toLowerCase())) {
    show(`Another crop is already called ${original.name}. Rename or remove it first, then restore this one.`, true);
    return;
  }
  db.ref(`irrigation/config/crops/${key}`).remove()
    .then(() => { show(`${original.name} restored.`); logActivity(`Restored crop profile ${original.name}`); })
    .catch(error => show(`Could not restore ${original.name}: ${error.message}.`, true));
}

document.getElementById("cropLibrary")?.addEventListener("click", event => {
  const button = event.target.closest("button");
  if (!button || button.classList.contains("info-icon")) return;
  const data = button.dataset;
  const moveStage = (from, to) => {
    readCropForm();
    const [stage] = cropDraft.stages.splice(from, 1);
    cropDraft.stages.splice(to, 0, stage);
    cropDraftFocus = `stage:${to}`;
    renderCropLibrary();
  };
  if (button.id === "cropAddBtn") openCropEditor(null);
  else if (data.cropEdit) openCropEditor(data.cropEdit);
  else if (data.cropRemove) removeCrop(data.cropRemove);
  else if (data.cropRestore) restoreCrop(data.cropRestore);
  else if (data.cropUnremove) restoreCrop(data.cropUnremove);
  else if (!cropDraft) return;
  else if (button.id === "cropCancel") closeCropEditor();
  else if (button.id === "cropAddStage") {
    readCropForm();
    if (cropDraft.stages.length >= MAX_STAGES) return;
    cropDraft.stages.push(newDraftStage());
    cropDraftFocus = `stage:${cropDraft.stages.length - 1}`;
    renderCropLibrary();
  }
  else if (data.stageUp !== undefined) moveStage(Number(data.stageUp), Number(data.stageUp) - 1);
  else if (data.stageDown !== undefined) moveStage(Number(data.stageDown), Number(data.stageDown) + 1);
  else if (data.stageRemove !== undefined) {
    readCropForm();
    if (cropDraft.stages.length > 1) cropDraft.stages.splice(Number(data.stageRemove), 1);
    renderCropLibrary();
  }
});
document.getElementById("cropLibrary")?.addEventListener("input", event => {
  if (!event.target.closest("#cropForm")) return;
  readCropForm();
  renderCropProblems();
});
document.getElementById("cropLibrary")?.addEventListener("submit", event => {
  if (event.target.id !== "cropForm") return;
  event.preventDefault();
  saveCropDraft();
});

/* ---- The rig drawing (2026-11-05) --------------------------------------------------------------------------------
 * The rig on the Live dashboard, drawn from ESP1's latest report: the reservoir and mixing tank with their water at the
 * measured level, the pumps, the nutrient bottles, the valves, and the three columns filled to their soil moisture.
 * The water's surface always moves a little, more while a pump fills or empties that tank and most while the mixer
 * stirs it, and it sloshes and settles when the level changes. Water runs along a pipe only while ESP1 reports that
 * pump running, and nothing is shown moving on a stale report. The waves are the only thing on a timer: they run only
 * while the drawing is on screen, and not at all when the phone asks for reduced motion. Two layouts, wide and narrow,
 * so a phone gets a drawing it can read rather than a shrunken one. */
const RIG_LAYOUTS = {
  wide: {
    viewBox: [1000, 340],
    tanks: { res: { x: 30, y: 60, w: 170, h: 230, label: [115, 320] }, mix: { x: 410, y: 90, w: 180, h: 200, label: [500, 320] } },
    pumps: { transfer: { cx: 290, cy: 270, r: 24, label: [290, 320] }, booster: { cx: 680, cy: 270, r: 24, label: [680, 320] } },
    pipes: {
      resOut: [[200, 270], [266, 270]],
      transferUp: [[314, 270], [350, 270], [350, 130], [410, 130]],
      mixOut: [[590, 270], [656, 270]],
      boosterOut: [[704, 270], [740, 270], [740, 60], [956, 60]]
    },
    bottles: { y: 22, h: 34, w: 34, xs: [438, 483, 528], tankTop: 90 },
    columns: { y: 110, h: 180, w: 56, xs: [776, 852, 928], busY: 60, valveY: 84, labelY: 320 }
  },
  narrow: {
    viewBox: [400, 446],
    tanks: { res: { x: 16, y: 56, w: 110, h: 140, label: [71, 220] }, mix: { x: 230, y: 56, w: 150, h: 140, label: [305, 220] } },
    pumps: { transfer: { cx: 172, cy: 176, r: 20, label: [172, 220] }, booster: { cx: 56, cy: 300, r: 20, label: [56, 344] } },
    pipes: {
      resOut: [[126, 176], [152, 176]],
      transferUp: [[192, 176], [206, 176], [206, 96], [230, 96]],
      mixOut: [[380, 186], [392, 186], [392, 248], [56, 248], [56, 280]],
      boosterOut: [[76, 300], [96, 300], [96, 272], [324, 272]]
    },
    bottles: { y: 8, h: 28, w: 26, xs: [250, 292, 334], tankTop: 56 },
    columns: { y: 310, h: 110, w: 56, xs: [120, 208, 296], busY: 272, valveY: 291, labelY: 440 }
  }
};
const RIG_NARROW_PX = 640;
const RIG_RES_LOW_PCT = 18;          // ESP1's RES_LOW_PCT (web v.4: = RES_FILTER_PCT, 18 %): no run starts below it
const rig = {
  layout: "", els: null, visible: false, frame: 0, lastTime: 0,
  tanks: {
    res: { level: 0, target: null, vel: 0, slosh: 0, phase: 0, activity: 0, amp: 0 },
    mix: { level: 0, target: null, vel: 0, slosh: 0, phase: 1.7, activity: 0, amp: 0 }
  }
};

function rigPathData(points) {
  return points.map(([x, y], i) => `${i ? "L" : "M"}${x} ${y}`).join(" ");
}
function rigBlades(cx, cy, r) {
  return [0, 120, 240].map(angle => `<ellipse cx="${cx}" cy="${cy - r / 2}" rx="${(r / 4).toFixed(1)}" ry="${(r / 2).toFixed(1)}" transform="rotate(${angle} ${cx} ${cy})"/>`).join("");
}

function buildRigDrawing(name) {
  const host = document.getElementById("rigDrawing");
  if (!host) return;
  const L = RIG_LAYOUTS[name];
  const [W, H] = L.viewBox;
  const c = L.columns;
  const ids = ["A", "B", "C"];
  const colCx = c.xs.map(x => x + c.w / 2);
  const defs = Object.entries(L.tanks).map(([key, t]) => `<clipPath id="rigClip-${name}-${key}"><rect x="${t.x + 4}" y="${t.y + 4}" width="${t.w - 8}" height="${t.h - 8}" rx="11"/></clipPath>`).join("")
    + c.xs.map((x, i) => `<clipPath id="rigClip-${name}-col${ids[i]}"><rect x="${x + 3}" y="${c.y + 3}" width="${c.w - 6}" height="${c.h - 6}" rx="8"/></clipPath>`).join("");
  const flows = {
    ...L.pipes,
    ...Object.fromEntries(ids.map((id, i) => [`drop${id}`, [[colCx[i], c.busY], [colCx[i], c.y]]])),
    ...Object.fromEntries(ids.map((id, i) => {
      const x = L.bottles.xs[i] + L.bottles.w / 2;
      return [`dose${id}`, [[x, L.bottles.y + L.bottles.h], [x, L.bottles.tankTop]]];
    }))
  };
  const pipes = Object.entries(flows).map(([key, points]) => {
    const thin = key.startsWith("dose") ? " thin" : "";
    return `<path class="rig-pipe${thin}" d="${rigPathData(points)}"/><path class="rig-flow${thin}" data-flow="${key}" d="${rigPathData(points)}"/>`;
  }).join("");
  const tank = (key, t, label) => {
    const low = key === "res" ? (() => {
      const y = (t.y + 4 + (t.h - 8) * (1 - RIG_RES_LOW_PCT / 100)).toFixed(1);
      return `<line class="rig-low" x1="${t.x + 6}" x2="${t.x + t.w - 6}" y1="${y}" y2="${y}"/><text class="rig-note" x="${t.x + t.w - 8}" y="${Number(y) - 5}" text-anchor="end">${RIG_RES_LOW_PCT} %</text>`;
    })() : "";
    return `<g class="rig-tank" data-tank="${key}">
      <rect class="rig-shell" x="${t.x}" y="${t.y}" width="${t.w}" height="${t.h}" rx="14"/>
      <g clip-path="url(#rigClip-${name}-${key})">
        <path class="rig-water back" data-water="${key}-back"/>
        <path class="rig-water" data-water="${key}"/>
      </g>
      ${low}
      <text class="rig-value" data-value="${key}" x="${t.x + t.w / 2}" y="${t.y + t.h / 2 + 9}" text-anchor="middle">--</text>
      <text class="rig-label" x="${t.label[0]}" y="${t.label[1]}" text-anchor="middle">${label}</text>
    </g>`;
  };
  // The mixer stands to one side, so its shaft doesn't run through the tank's reading.
  const mix = L.tanks.mix;
  const mixX = mix.x + mix.w * (name === "wide" ? 0.84 : 0.9);
  const mixer = `<g class="rig-mixer"><line class="rig-shaft" x1="${mixX}" y1="${mix.y + 4}" x2="${mixX}" y2="${mix.y + mix.h - 24}"/>
      <g class="rig-impeller">${rigBlades(mixX, mix.y + mix.h - 24, name === "wide" ? 15 : 12)}</g></g>`;
  const pump = (key, p, label) => `<g class="rig-pump" data-pump="${key}">
      <circle class="rig-pump-body" cx="${p.cx}" cy="${p.cy}" r="${p.r}"/>
      <g class="rig-impeller">${rigBlades(p.cx, p.cy, p.r * 0.62)}</g>
      <text class="rig-label" x="${p.label[0]}" y="${p.label[1]}" text-anchor="middle">${label}</text>
    </g>`;
  const bottles = L.bottles.xs.map((x, i) => `<g class="rig-bottle" data-bottle="${ids[i]}">
      <rect x="${x}" y="${L.bottles.y}" width="${L.bottles.w}" height="${L.bottles.h}" rx="7"/>
      <text x="${x + L.bottles.w / 2}" y="${L.bottles.y + L.bottles.h / 2 + 5}" text-anchor="middle">${ids[i]}</text>
    </g>`).join("");
  const valves = colCx.map((cx, i) => {
    const s = name === "wide" ? 9 : 8;
    return `<path class="rig-valve" data-valve="${ids[i]}" d="M${cx - s} ${c.valveY - s} H${cx + s} L${cx - s} ${c.valveY + s} H${cx + s} Z"/>`;
  }).join("");
  const columns = c.xs.map((x, i) => `<g class="rig-col" data-col="${ids[i]}">
      <rect class="rig-shell rig-soil" x="${x}" y="${c.y}" width="${c.w}" height="${c.h}" rx="10"/>
      <g clip-path="url(#rigClip-${name}-col${ids[i]})"><rect class="rig-moist" data-moist="${ids[i]}" x="${x}" y="${c.y}" width="${c.w}" height="${c.h}"/></g>
      <line class="rig-start" data-start="${ids[i]}" x1="${x + 5}" x2="${x + c.w - 5}" y1="${c.y + c.h}" y2="${c.y + c.h}"/>
      <text class="rig-label" x="${colCx[i]}" y="${c.labelY}" text-anchor="middle">${ids[i]} <tspan class="rig-col-value" data-colvalue="${ids[i]}">--</tspan></text>
    </g>`).join("");
  host.innerHTML = `<svg class="rig-svg ${name}" viewBox="0 0 ${W} ${H}" role="img" aria-labelledby="rigSvgTitle">
      <title id="rigSvgTitle">The rig</title>
      <defs>${defs}</defs>
      ${pipes}
      ${tank("res", L.tanks.res, "Reservoir")}
      ${tank("mix", L.tanks.mix, "Mixing tank")}
      ${mixer}
      ${bottles}
      ${pump("transfer", L.pumps.transfer, name === "wide" ? "Transfer pump" : "Transfer")}
      ${pump("booster", L.pumps.booster, name === "wide" ? "Booster pump" : "Booster")}
      ${valves}
      ${columns}
    </svg>`;
  const svg = host.querySelector("svg");
  const q = selector => svg.querySelector(selector);
  rig.layout = name;
  rig.els = {
    svg, title: q("#rigSvgTitle"),
    water: { res: [q('[data-water="res-back"]'), q('[data-water="res"]')], mix: [q('[data-water="mix-back"]'), q('[data-water="mix"]')] },
    value: { res: q('[data-value="res"]'), mix: q('[data-value="mix"]') }
  };
}

// The water's surface across one tank: two waves on top of each other, so it never looks like a plain sine.
function rigWaterPath(t, level, amp, phase, offset) {
  const inner = { x: t.x + 4, y: t.y + 4, w: t.w - 8, h: t.h - 8 };
  const surface = inner.y + inner.h * (1 - level);
  const calm = Math.min(1, level * 8);                      // a nearly empty tank can't make waves
  const k1 = (2 * Math.PI) / (inner.w * 0.95);
  const k2 = (2 * Math.PI) / (inner.w * 0.42);
  const parts = [];
  for (let x = 0; x <= inner.w + 0.1; x += 6) {
    const dx = Math.min(x, inner.w);
    const y = surface + amp * calm * (0.65 * Math.sin(k1 * dx + phase + offset) + 0.35 * Math.sin(k2 * dx - phase * 1.6 + offset * 2));
    parts.push(`${parts.length ? "L" : "M"}${(inner.x + dx).toFixed(1)} ${y.toFixed(1)}`);
  }
  return `${parts.join("")}L${inner.x + inner.w} ${inner.y + inner.h + 2}L${inner.x} ${inner.y + inner.h + 2}Z`;
}

function drawRigWater() {
  if (!rig.els) return;
  const L = RIG_LAYOUTS[rig.layout];
  ["res", "mix"].forEach(key => {
    const s = rig.tanks[key];
    const [back, front] = rig.els.water[key];
    if (s.target === null) { back.setAttribute("d", ""); front.setAttribute("d", ""); return; }
    back.setAttribute("d", rigWaterPath(L.tanks[key], s.level, s.amp * 0.8, s.phase * 0.85, 2.1));
    front.setAttribute("d", rigWaterPath(L.tanks[key], s.level, s.amp, s.phase, 0));
  });
}

// A damped spring brings the drawn level to the reported one; a change sets the water sloshing, which dies away.
function rigStep(dt) {
  Object.values(rig.tanks).forEach(s => {
    const target = s.target ?? 0;
    const stiffness = 9;
    s.vel += (stiffness * (target - s.level) - 2 * Math.sqrt(stiffness) * s.vel) * dt;
    s.level = Math.min(1, Math.max(0, s.level + s.vel * dt));
    s.slosh *= Math.exp(-dt / 1.4);
    const base = s.target === null ? 0 : s.activity === 2 ? 8 : s.activity === 1 ? 5 : 2.2;
    s.amp = base + s.slosh;
    s.phase += dt * (s.activity === 2 ? 3.2 : s.activity === 1 ? 2.1 : 1.1);
  });
}
function rigSettle() {
  Object.values(rig.tanks).forEach(s => { s.level = s.target ?? 0; s.vel = 0; s.slosh = 0; s.amp = s.target === null ? 0 : 2.2; });
}
function rigShouldAnimate() {
  return Boolean(rig.els) && rig.visible && !document.hidden && !reduceMotionQuery?.matches;
}
function rigLoop(time) {
  rig.frame = 0;
  const dt = rig.lastTime ? Math.min(0.05, (time - rig.lastTime) / 1000) : 0;
  rig.lastTime = time;
  rigStep(dt);
  drawRigWater();
  if (rigShouldAnimate()) rig.frame = requestAnimationFrame(rigLoop);
}
// Starts or stops the waves to match whether anyone can see them.
function rigWake() {
  if (rigShouldAnimate()) {
    if (!rig.frame) { rig.lastTime = 0; rig.frame = requestAnimationFrame(rigLoop); }
    return;
  }
  if (rig.frame) cancelAnimationFrame(rig.frame);
  rig.frame = 0;
  rigSettle();
  drawRigWater();
}

function setRigTank(key, reading, activity) {
  const s = rig.tanks[key];
  const target = hasValue(reading) ? Math.min(1, Math.max(0, Number(reading) / 100)) : null;
  if (target !== null && s.target !== null && Math.abs(target - s.target) > 0.004) s.slosh = Math.min(10, s.slosh + Math.abs(target - s.target) * 90);
  if (target !== null && s.target === null) s.level = target;      // the first reading: no long rise from empty
  s.target = target;
  s.activity = target === null ? 0 : activity;
  const value = rig.els?.value[key];
  if (value) value.textContent = target === null ? "No reading" : `${Math.round(Number(reading))} %`;
}

// Everything but the water's surface, from the latest report. Runs with every snapshot.
function updateRigDrawing() {
  if (!rig.els) return;
  const svg = rig.els.svg;
  const sensors = liveData.sensors || {};
  const actuators = liveData.actuators || {};
  const run = liveData.diagnostics?.runProgress || {};
  const fresh = deviceIsFresh();
  const transfer = fresh && Boolean(actuators.transferRunning);
  const booster = fresh && Boolean(actuators.boosterRunning);
  const mixer = fresh && Boolean(actuators.mixerRunning);
  const dosing = fresh && Boolean(run.active) && run.stage === "Pump Nutrients";
  const doseNow = dosing ? ["A", "B", "C"].find(k => Number(run.dosesMl?.[k]?.target) > Number(run.dosesMl?.[k]?.delivered || 0)) || null : null;
  const zone = String(run.zone || "").toUpperCase();
  setRigTank("res", sensors.reservoirLevel, transfer ? 1 : 0);
  setRigTank("mix", sensors.mixingLevel, mixer ? 2 : (transfer || booster || dosing) ? 1 : 0);
  const resLow = hasValue(sensors.reservoirLevel) && Number(sensors.reservoirLevel) < RIG_RES_LOW_PCT;
  svg.querySelector('[data-tank="res"]')?.classList.toggle("low", resLow);
  svg.querySelector('[data-tank="mix"]')?.classList.toggle("fed", Boolean(run.active) && run.operation === "fertigation");
  svg.querySelector('[data-pump="transfer"]')?.classList.toggle("on", transfer);
  svg.querySelector('[data-pump="booster"]')?.classList.toggle("on", booster);
  svg.querySelector(".rig-mixer")?.classList.toggle("on", mixer);
  const flowing = new Set();
  if (transfer) ["resOut", "transferUp"].forEach(key => flowing.add(key));
  if (booster) {
    ["mixOut", "boosterOut"].forEach(key => flowing.add(key));
    ["A", "B", "C"].filter(id => zone.includes(id)).forEach(id => flowing.add(`drop${id}`));
  }
  if (doseNow) flowing.add(`dose${doseNow}`);
  svg.querySelectorAll("[data-flow]").forEach(el => el.classList.toggle("on", flowing.has(el.dataset.flow)));
  svg.querySelectorAll("[data-valve]").forEach(el => el.classList.toggle("open", booster && zone.includes(el.dataset.valve)));
  svg.querySelectorAll("[data-bottle]").forEach(el => el.classList.toggle("on", el.dataset.bottle === doseNow));
  // The columns: filled to their soil moisture, with ESP1's Soil start % marked (it waters below that line).
  const L = RIG_LAYOUTS[rig.layout];
  const start = soilStartPercent();
  const words = [];
  ["A", "B", "C"].forEach(id => {
    const z = sensors.zones?.[id] || {};
    const off = z.enabled === false;
    const known = !off && hasValue(z.moisture);
    const pct = known ? Math.min(100, Math.max(0, Number(z.moisture))) : 0;
    const group = svg.querySelector(`[data-col="${id}"]`);
    group?.classList.toggle("off", off);
    group?.classList.toggle("dry", known && pct < start);
    const moist = svg.querySelector(`[data-moist="${id}"]`);
    if (moist) moist.style.transform = `scaleY(${(pct / 100).toFixed(3)})`;
    const line = svg.querySelector(`[data-start="${id}"]`);
    if (line) {
      const y = (L.columns.y + L.columns.h * (1 - start / 100)).toFixed(1);
      line.setAttribute("y1", y);
      line.setAttribute("y2", y);
    }
    const value = svg.querySelector(`[data-colvalue="${id}"]`);
    if (value) value.textContent = off ? "off" : known ? `${Math.round(pct)} %` : "--";
    words.push(`column ${id} ${off ? "off" : known ? `${Math.round(pct)} % moist` : "no reading"}`);
  });
  const status = !liveData.sensors ? "Waiting for ESP1's report."
    : !fresh ? "ESP1 isn't reporting, so this is its last report, and nothing is shown moving."
    : booster ? `Watering column${zone.length > 1 ? "s" : ""} ${columnList([...zone].filter(ch => "ABC".includes(ch))) || "?"} from the mixing tank.`
    : mixer ? "Stirring the batch in the mixing tank."
    : doseNow ? `Adding nutrient ${doseNow} to the mixing tank.`
    : transfer ? "Filling the mixing tank from the reservoir."
    : run.active ? `${run.operation === "fertigation" ? "Fertigation" : "Irrigation"} for column ${zone || "?"}: ${rawText(run.stage, "starting up")}.`
    : "Idle. No pump is running.";
  setText("rigStatus", status);
  const tank = key => hasValue(sensors[key]) ? `${Math.round(Number(sensors[key]))} %` : "no reading";
  rig.els.title.textContent = `The rig: reservoir ${tank("reservoirLevel")}, mixing tank ${tank("mixingLevel")}, ${words.join(", ")}. ${status}`;
  rigWake();
}

// ESP1 doesn't report its Soil start %, so this is the last value sent from this website (in the commands still
// listed), or the firmware's default.
function soilStartPercent() {
  const sent = commandData
    .filter(c => c.type === "SET_THRESH" && c.status === "completed" && hasValue(c.payload?.thStart))
    .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))[0];
  return sent ? Number(sent.payload.thStart) : 35;
}

function initRigDrawing() {
  const host = document.getElementById("rigDrawing");
  if (!host) return;
  const pick = () => {
    const name = host.clientWidth && host.clientWidth < RIG_NARROW_PX ? "narrow" : "wide";
    if (name === rig.layout) return;
    buildRigDrawing(name);
    updateRigDrawing();
  };
  pick();
  if (window.ResizeObserver) new ResizeObserver(pick).observe(host);
  if (window.IntersectionObserver) {
    new IntersectionObserver(entries => { rig.visible = entries.some(e => e.isIntersecting); rigWake(); }).observe(host);
  } else {
    rig.visible = true;
  }
  document.addEventListener("visibilitychange", rigWake);
  reduceMotionQuery?.addEventListener?.("change", rigWake);
}

// The Nano's pinger status letter (web v.4 firmware of 17 Nov 2026, sensorsRaw.tank.*Status): why a tank has no reading.
const TANK_STATUS_WORDS = { E: "no echo", S: "echo line stuck", R: "out of range", F: "too few good pings", X: "not pinging" };
function tankReadingText(pct, litres, status) {
  if (!hasValue(pct)) {
    const why = TANK_STATUS_WORDS[status];
    return why ? `No reading: ${why}` : "Unavailable";
  }
  const p = numberText(pct, 1, "%");
  return hasValue(litres) ? `${p} · ${numberText(litres, 1, "L")}` : p;
}

function updateDashboard() {
  const sensors = liveData.sensors || {};
  const system = liveData.system || {};
  const actuators = liveData.actuators || {};
  const diagnostics = liveData.diagnostics || {};
  // web v.4 firmware also reports the litres each tank holds (its tank calibration); older firmware only the %.
  // A tank may read over 100 %: water above the calibrated full mark.
  const rawTank = diagnostics.sensorsRaw?.tank || {};
  setText("reservoirLevel", tankReadingText(sensors.reservoirLevel, sensors.reservoirLiters, rawTank.reservoirStatus));
  setText("mixingLevel", tankReadingText(sensors.mixingLevel, sensors.mixingLiters, rawTank.mixingStatus));
  setText("flowRate", numberText(sensors.flowRate, 1, "L/min"));
  setText("temperature", numberText(sensors.temperature, 1, "°C"));
  setText("humidity", numberText(sensors.humidity, 1, "%"));
  setText("lightLevel", numberText(sensors.lightLevel, 0, "lux"));
  setText("waterPH", numberText(sensors.waterPH, 2));
  setText("waterEC", numberText(sensors.waterEC, 2, "mS/cm"));
  setText("batteryVoltage", numberText(sensors.batteryVoltage, 2, "V"));
  setText("batteryPercent", numberText(sensors.batteryPercent, 0, "%"));
  // batteryCurrent is a magnitude; the signed copy only exists while the main INA226 is the source.
  setText("batteryCurrent", signedCurrentText(sensors.batteryCurrent, sensors.batteryCurrentSigned));
  // Watts comes from the diagnostics tree, not sensors -- ESP1 publishes battP there alongside the
  // low/critical flags. Also shown under Diagnostics > Power; this is the front-page copy.
  setText("batteryPower", numberText(diagnostics.power?.batteryPower, 1, "W"));
  setText("powerSource", rawText(system.powerSource));
  setText("liveAge", snapshotAgeText());

  // Solar and secondary battery. One note instead of ten "Not reported" cards on older firmware. On
  // web v.3 each reading is only left out while its own sensor isn't answering, so it reads Unavailable.
  const oldFirmware = Boolean(liveData.system) && !isV3Firmware();
  const powerGrid = document.getElementById("powerGrid");
  if (powerGrid) powerGrid.hidden = oldFirmware;
  const powerGridNote = document.getElementById("powerGridNote");
  if (powerGridNote) powerGridNote.hidden = !oldFirmware;
  setText("solarVoltage", numberText(sensors.solarVoltage, 2, "V"));
  setText("solarCurrent", numberText(sensors.solarCurrent, 2, "A"));
  setText("solarPower", numberText(sensors.solarPower, 1, "W"));
  setText("solarEnergyToday", numberText(sensors.solarEnergyTodayWh, 0, "Wh"));
  setText("boardSupply", boardSupplyText());
  setText("secondaryBatteryVoltage", numberText(sensors.secondaryBatteryVoltage, 2, "V"));
  setText("secondaryBatteryPercent", numberText(sensors.secondaryBatteryPercent, 0, "%"));
  // ESP1 publishes this one signed only (+ = discharging), so the size comes from it as well. Passed as it
  // was, a charging pack read "-0.50 A charging".
  const secondaryAmps = sensors.secondaryBatteryCurrent;
  setText("secondaryBatteryCurrent", signedCurrentText(hasValue(secondaryAmps) ? Math.abs(Number(secondaryAmps)) : secondaryAmps, secondaryAmps));
  setText("secondaryBatteryPower", numberText(sensors.secondaryBatteryPower, 1, "W"));
  setText("secondaryBatteryStatus", booleanText(sensors.secondaryBatteryLow, "Low", "OK"));
  syncForceColumnOptions();
  syncPulseTargetOptions();

  const fresh = deviceIsFresh();
  setDeviceStatus("systemState", system.state || "WAITING FOR DATA", fresh ? "active" : "off");
  updateRunProgress(diagnostics.runProgress || {});

  // Pump lamps. A stale snapshot must never be drawn as a live "ON" -- if ESP1 has gone quiet we
  // do not know what the pumps are doing, and "OFF" is the only claim the page can defend.
  setPumpLamp("transferPumpStatus", fresh && Boolean(actuators.transferRunning));
  setPumpLamp("boosterPumpStatus", fresh && Boolean(actuators.boosterRunning));
  setPumpLamp("mixerPumpStatus", fresh && Boolean(actuators.mixerRunning));
  updateRigDrawing();
  // The column editor offers several watering times a day only to firmware that has them, and the first snapshot
  // usually lands after the cards are built.
  if (multiRunSupported() !== zoneCardsMultiRun) renderZonesUI();
  updateZoneReadings();

  renderDiagnostics();
  renderRawSensors();
  renderFlowMeters();
  renderExercise();
  renderManualHold();
  updateFaultBanner();
  updateForceArmed();
  renderUpNext();
  updateSystemTab();
  syncControlAvailability();
}

function setPumpLamp(id, on) {
  setDeviceStatus(id, on ? "ON" : "OFF", on ? "active" : "off");
}

function boardSupplyText() {
  const supply = liveData.system?.boardSupply;
  return supply === "secondary" ? "Secondary battery" : supply === "main" ? "Main battery" : "Unavailable";
}

// ESP1 only waters the enabled columns in a request: "AB" with B disabled sends the full volume to A
// without saying so. So a column ESP1 reports as disabled, and any combination that includes it, can't
// be chosen here. Only an explicit enabled:false counts, because older snapshots can leave it out.
function forceColumnsDisabled(columns) {
  return [...String(columns)].some(id => liveData.sensors?.zones?.[id]?.enabled === false);
}

function syncForceColumnOptions() {
  const select = document.getElementById("forceColumns");
  if (!select) return;
  [...select.options].forEach(option => {
    if (!option.value) return;
    const off = forceColumnsDisabled(option.value);
    const label = `${option.dataset.label}${off ? " (disabled on ESP1)" : ""}`;
    // Only touched on a real change, so a snapshot can't disturb a dropdown that is open.
    if (option.disabled !== off) option.disabled = off;
    if (option.textContent !== label) option.textContent = label;
  });
  if (select.selectedOptions[0]?.disabled) select.value = "";
}

// The Timed pulses list says the same about its push-to-column targets. The choice itself is left alone,
// because this list has no empty entry to fall back to; the Run pulse button refuses it instead.
function syncPulseTargetOptions() {
  const select = document.getElementById("pulseTarget");
  if (!select) return;
  [...select.options].forEach(option => {
    const column = /^col([ABC])$/.exec(option.value)?.[1];
    if (!column) return;
    if (option.dataset.label === undefined) option.dataset.label = option.textContent;
    const label = `${option.dataset.label}${forceColumnsDisabled(column) ? " (disabled on ESP1)" : ""}`;
    if (option.textContent !== label) option.textContent = label;
  });
}

function updateRunProgress(run) {
  const active = Boolean(run.active);
  const phase = rawText(run.phase, "IDLE");
  // A run in ESP1's last report may or may not still be going, so the page can't claim either.
  const unknown = active && !deviceIsFresh();
  setDeviceStatus("sideRunState", unknown ? "UNKNOWN" : active ? phase : "NO ACTIVE RUN", active && !unknown ? "active" : "off");
  setDeviceStatus("runStateBadge", unknown ? "UNKNOWN" : active ? phase : "IDLE", active && !unknown ? "active" : "off");
  if (!active) {
    setText("runSummary", "Nothing is running.");
    setText("runStage", "Unavailable");
    setText("runStageProgress", "Unavailable");
    setText("runWaterProgress", "Unavailable");
    setText("runDoseProgress", "Unavailable");
    return;
  }
  const stageOrder = hasValue(run.stageOrdinal) && hasValue(run.stageTotal) ? `Stage ${run.stageOrdinal} of ${run.stageTotal}` : "Stage order unavailable";
  const operation = run.operation ? run.operation[0].toUpperCase() + run.operation.slice(1) : "Run";
  setText("runSummary", unknown
    ? `ESP1 has stopped reporting. Its last report showed ${run.operation || "a run"} for column ${run.zone || "?"}, and this page can't tell whether that is still going.`
    : `${operation} for column ${run.zone || "?"}: ${stageOrder}.`);
  setText("runStage", rawText(run.stage));
  // Stages that move no water, such as Mixing, report a target of 0.
  setText("runStageProgress", Number(run.stageTargetLiters) > 0
    ? `${numberText(run.stageLiters, 1, "L")} of ${numberText(run.stageTargetLiters, 1, "L")}`
    : stageOrder);
  // ESP2 only adds a Release stage's water to its total when the stage ends, so count the one in progress too,
  // or this would sit still while water is moving. After the run, ESP1 fixed on 5 Nov 2026 takes the total from
  // ESP2's DONE and zeroes the stage's own litres, so nothing is counted twice; older firmware leaves the total
  // short by that last release, and adding it here is what makes the finished run read right.
  const releasing = run.stage === "Release" || run.stage === "Flush Release";
  const delivered = Number(run.waterDeliveredLiters || 0) + (releasing ? Number(run.stageLiters || 0) : 0);
  setText("runWaterProgress", hasValue(run.waterTargetLiters)
    ? `${numberText(delivered, 1, "L")} of ${numberText(run.waterTargetLiters, 1, "L")}`
    : "Unavailable");
  const doses = run.dosesMl || {};
  const doseText = ["A", "B", "C"].filter(key => hasValue(doses[key]?.target) || hasValue(doses[key]?.delivered))
    .map(key => `${key}: ${numberText(doses[key]?.delivered, 1, "mL")} / ${numberText(doses[key]?.target, 1, "mL")}`).join(" · ");
  setText("runDoseProgress", doseText || "Unavailable");
}

function diagnosticGroup(title, rows) {
  const card = document.createElement("article");
  card.className = "diagnostic-card";
  const heading = document.createElement("h3");
  heading.textContent = title;
  card.appendChild(heading);
  const list = document.createElement("dl");
  rows.forEach(([label, value]) => {
    const term = document.createElement("dt");
    const description = document.createElement("dd");
    term.textContent = label;
    description.textContent = rawText(value);
    list.append(term, description);
  });
  card.appendChild(list);
  return card;
}

// The modem's AT+CSQ scale. ESP1 sends -1 until it has a reading and 99 for no signal, and the LCD shows
// both as "--".
function gsmSignalText(rssi) {
  if (!hasValue(rssi)) return "Unavailable";
  return Number(rssi) < 0 || Number(rssi) === 99 ? "No signal" : `${rssi} of 31`;
}
// The modem's CREG/CEREG status. The firmware counts 1, 5, 6, 7, 9 and 10 as registered (gsmRegStatOk).
// The A7670C has been seen reporting 6.
const GSM_REGISTRATION_TEXT = {
  "-1": "No reply yet", 0: "Not searching", 1: "Registered, home network", 2: "Searching", 3: "Denied", 4: "Unknown",
  5: "Registered, roaming", 6: "Registered, home network (SMS only)", 7: "Registered, roaming (SMS only)",
  9: "Registered, home network", 10: "Registered, roaming"
};

/* Diagnostics summary (round 2, 2026-11-05): what ESP1 reports as wrong, in words, worst first, before the item-by-item
 * details. Only what ESP1 publishes: it can't see what ESP1 doesn't check. */
function healthItems() {
  const d = liveData.diagnostics || {};
  if (!Object.keys(d).length) return [];
  const items = [];
  const add = (tone, text) => items.push({ tone, text });
  const f = d.fault || {};
  const state = liveData.system?.state || "";
  if (!deviceIsFresh()) add("bad", `ESP1 isn't reporting: ${snapshotAgeText().toLowerCase()}. Everything below is from that report.`);
  if (f.held) add("bad", `A fault is held and waiting for a decision: ${rawText(heldFaultCode || f.code, "see the banner")}. Use the banner at the top of the page.`);
  else if (state === "EMERGENCY_STOP") add("bad", "The emergency stop is on. Use Return to normal in the banner when it is safe.");
  if (actuationsAreOff()) add("warn", "Pumps and valves are switched off (actuations disabled), so nothing runs, not even the schedule.");
  if (rigLocked()) add("warn", "The rig is locked: this website's controls and the rig's screen buttons are off. Automatic watering carries on.");
  if (d.esp2?.communicationLost) add("bad", "ESP1 has lost contact with ESP2, the pump and valve board. It is trying to bring it back.");
  const nanoStale = hasValue(d.nano?.lastSampleAgeMs) && ageFlag(d.nano.lastSampleAgeMs) !== "ok";
  if (nanoStale) add("bad", `No sensor readings from the Nano for ${formatAge(d.nano.lastSampleAgeMs)}. Watering decisions need them.`);
  else {
    const bad = [["environmentValid", "air"], ["tankValid", "tank level"], ["lightValid", "light"]].filter(([key]) => d.nano?.[key] === false).map(([, name]) => name);
    if (bad.length) add("warn", `The Nano's ${bad.join(", ")} reading${bad.length > 1 ? "s are" : " is"} not valid right now.`);
  }
  if (d.peripherals?.rtcOk === false) add("bad", "The clock (RTC) isn't working, so the watering times can't be trusted. Check it on the System tab.");
  if (d.peripherals?.sdOk === false) add("warn", "The SD card isn't working, so ESP1 can't keep its daily log.");
  if (d.peripherals?.batterySensorOk === false) add("warn", "The battery sensor isn't answering, so the battery readings are missing.");
  if (d.power?.batteryCritical) add("bad", "The battery is critically low.");
  else if (d.power?.batteryLow) add("warn", "The battery is low.");
  if (liveData.system?.battSwitchFault === true) add("warn", "A battery switch fault is latched, so the inverter can't be used. Clear it on the System tab after checking the hold-up.");
  if (d.gsm?.simReady === false) add("warn", "The SIM card isn't ready, so SMS alerts can't be sent.");
  else if (d.gsm?.networkRegistered === false) add("warn", "The SMS modem isn't on the mobile network, so SMS alerts can't be sent right now.");
  if (Number(d.firebase?.attempts || 0) > 0 && d.firebase?.lastUploadOk === false) add("warn", "ESP1's last upload to this website failed. It retries by itself.");
  if (d.thingspeak?.configured && d.thingspeak?.attempted && d.thingspeak?.lastUploadOk === false) add("warn", "ESP1's last upload to the ThingSpeak graphs failed.");
  if (!items.length) add("ok", "Everything ESP1 checks looks fine.");
  const lastFault = d.system?.lastFault;
  if (lastFault && lastFault !== "none" && !f.held) {
    add("", `The last fault ESP1 recorded was ${lastFault}${d.system?.lastFaultTime ? ` at ${d.system.lastFaultTime}` : ""}. It is over; it stays here as a record.`);
  }
  return items;
}

function renderHealthSummary() {
  const list = document.getElementById("healthSummary");
  if (!list) return;
  const items = healthItems();
  list.innerHTML = "";
  if (!items.length) {
    list.innerHTML = '<li class="muted">Waiting for ESP1\'s report.</li>';
    return;
  }
  items.forEach(({ tone, text }) => {
    const li = document.createElement("li");
    if (tone) li.className = tone;
    li.textContent = text;
    list.appendChild(li);
  });
}

function renderDiagnostics() {
  renderHealthSummary();
  const container = document.getElementById("diagnosticsGrid");
  if (!container) return;
  const d = liveData.diagnostics || {};
  container.innerHTML = "";
  const current = deviceIsFresh();
  const diagnosticsAvailable = Object.keys(d).length > 0;
  const groups = [
    ["ESP1's report", [["Report", current ? "Current" : "Stale or unavailable"], ["Received", snapshotAgeText()], ["ESP1 state", liveData.system?.state || "Unavailable"], ["Firmware", !liveData.system ? "Unavailable" : liveData.system.firmware ? String(liveData.system.firmware) : isV3Firmware() ? "web v.3 (or web v.4 before 17 Nov)" : "Older than web v.3"], ["Report size", liveData.meta?.docUsed ? `${liveData.meta.docUsed} / ${liveData.meta.docCapacity} B` : "Unavailable"]]],
    ["Wi-Fi", [["Wi-Fi enabled", booleanText(d.network?.wifiEnabled, "Enabled", "Disabled")], ["Wi-Fi link", booleanText(d.network?.wifiConnected, "Connected", "Disconnected")], ["Signal strength", hasValue(d.network?.wifiRssi) ? `${d.network.wifiRssi} dBm` : "Unavailable"]]],
    ["Link to this website (Firebase)", [["Enabled", booleanText(d.firebase?.enabled, "Enabled", "Disabled")], ["RTDB URL", booleanText(d.firebase?.urlConfigured, "Configured", "Not configured")], ["Device account", booleanText(d.firebase?.deviceCredentialsConfigured, "Configured", "Not configured")], ["Signed in", booleanText(d.firebase?.signedIn, "Yes", "No")], ["Last upload", Number(d.firebase?.attempts || 0) > 0 ? booleanText(d.firebase?.lastUploadOk, "OK", "Failed") : "Not attempted"], ["Last HTTP", Number(d.firebase?.attempts || 0) > 0 ? rawText(d.firebase?.lastHttp) : "Not attempted"], ["TLS validation", booleanText(d.firebase?.tlsValidationEnabled, "Enabled", "Disabled")], ["Last auth issue", rawText(d.firebase?.lastAuthError, "None")]]],
    ["ThingSpeak graphs", [["Configured", booleanText(d.thingspeak?.configured, "Configured", "Not configured")], ["Last upload", d.thingspeak?.attempted ? booleanText(d.thingspeak?.lastUploadOk, "OK", "Failed") : "Not attempted"]]],
    ["Log upload", [["Goes to", d.logUpload?.target === "firebase" ? "Firebase (SD card logs, below)" : d.logUpload?.target === "supabase" || (!d.logUpload && d.supabase?.configured) ? "Supabase" : "Not configured"], ["Upload", booleanText(d.logUpload ? d.logUpload.busy : d.supabase?.uploadBusy, "In progress", "Idle")], ["Last uploaded day", Number(d.logUpload?.lastUploadedDay || d.supabase?.lastUploadedDay || 0) > 0 ? rawText(d.logUpload?.lastUploadedDay || d.supabase?.lastUploadedDay) : "No completed upload"]]],
    ["Runs and faults", [["Work order", booleanText(d.system?.workOrderActive, "Active", "Inactive")], ["Run waiting to start", booleanText(d.system?.pendingRun, "Yes", "No")], ["Last fault", rawText(d.system?.lastFault === "none" ? "" : d.system?.lastFault, "None") + (explainFaultCode(d.system?.lastFault) ? ` — ${explainFaultCode(d.system?.lastFault)}` : "")], ["Fault time", rawText(d.system?.lastFaultTime, "None")], ["Rig lock", booleanText(d.system?.lcdLocked, "Locked", "Unlocked")]]],
    ["ESP2 (pumps and valves)", [["Available", booleanText(d.esp2?.available, "Available", "Unavailable")], ["Power", booleanText(d.esp2?.powered, "On", "Off")], ["Communication", booleanText(d.esp2?.communicationLost, "Lost", "OK")], ["Last reply", agoText(d.esp2?.lastResponseAgeMs)]]],
    ["Nano (sensors)", [["Last reading", agoText(d.nano?.lastSampleAgeMs)], ["Air", booleanText(d.nano?.environmentValid, "Valid", "Invalid")], ["Tank", booleanText(d.nano?.tankValid, "Valid", "Invalid")], ["Light", booleanText(d.nano?.lightValid, "Valid", "Invalid")]]],
    ["Clock, SD card and power sensors", [["RTC", booleanText(d.peripherals?.rtcOk, "OK", "Not OK")], ["SD card", booleanText(d.peripherals?.sdOk, "OK", "Not OK")], ["Battery sensor", booleanText(d.peripherals?.batterySensorOk, "OK", "Not OK")], ["Main battery INA226", v3Only(booleanText(d.peripherals?.mainInaOk, "In use", "Not in use"))], ["ACS758 fallback", v3Only(booleanText(d.peripherals?.batteryAdsOk, "OK", "Not OK"))], ["Solar INA226", v3Only(booleanText(d.peripherals?.solarSensorOk, "OK", "Not answering"))], ["Secondary INA226", v3Only(booleanText(d.peripherals?.secondaryBatterySensorOk, "OK", "Not answering"))]]],
    ["Battery switching", [["Boards powered from", v3Only(boardSupplyText())], ["Switching", v3Only(booleanText(liveData.system?.secondaryBattSwitching, "On", "Off in this firmware"))], ["Switch fault", v3Only(booleanText(liveData.system?.battSwitchFault, "Latched: inverter use blocked", "None"))], ["Reset strikes", v3Only(rawText(liveData.system?.battSwitchStrikes))], ["Backup mode", v3Only(booleanText(liveData.system?.backupEnabled, "On", "Off in this firmware"))], ["Backup state", v3Only(rawText(liveData.system?.backupState))]]],
    ["SMS modem (GSM)", [["SIM", booleanText(d.gsm?.simReady, "Ready", "Not ready")], ["Network", booleanText(d.gsm?.networkRegistered, "Registered", "Not registered")], ["Signal", gsmSignalText(d.gsm?.rssi)], ["Registration", GSM_REGISTRATION_TEXT[d.gsm?.creg] ?? rawText(d.gsm?.creg)], ["Last checked", agoText(d.gsm?.lastHealthAgeMs)]]],
    ["Battery", [["Battery low", booleanText(d.power?.batteryLow, "Yes", "No")], ["Battery critical", booleanText(d.power?.batteryCritical, "Yes", "No")], ["Current", numberText(d.power?.batteryCurrent, 2, "A")], ["Power", numberText(d.power?.batteryPower, 1, "W")]]],
    ["Pumps and tests", [["Relay feedback", d.actuator?.relayFeedback === "notReported" ? "Not reported by ESP1" : rawText(d.actuator?.relayFeedback)], ["Work order", booleanText(d.actuator?.workOrderActive, "Active", "Inactive")], ["Pump test", booleanText(d.actuator?.pumpTestActive, "Active", "Inactive")], ["Pump under test", rawText(d.actuator?.pumpUnderTest)]]]
  ];
  if (!diagnosticsAvailable) {
    const message = document.createElement("p");
    message.className = "muted";
    message.textContent = "No diagnostics have been published by ESP1 yet. Upload the current ESP1 firmware, then wait for its next Firebase snapshot.";
    container.appendChild(message);
  } else {
    groups.forEach(([title, rows]) => container.appendChild(diagnosticGroup(title, rows)));
  }

  const eventBox = document.getElementById("diagnosticEvents");
  if (!eventBox) return;
  eventBox.innerHTML = "";
  const events = Array.isArray(d.recentEvents) ? d.recentEvents : [];
  if (!events.length) {
    eventBox.innerHTML = '<p class="muted">No event data reported yet.</p>';
    return;
  }
  events.slice().reverse().forEach(event => {
    const row = document.createElement("article");
    row.className = `event-row ${String(event.type || "").toLowerCase()}`;
    const meta = document.createElement("strong");
    const detail = document.createElement("span");
    meta.textContent = `${rawText(event.at, "time unavailable")} · ${rawText(event.source)} · ${rawText(event.type)}`;
    detail.textContent = rawText(event.detail);
    row.append(meta, detail);
    eventBox.appendChild(row);
  });
}

function commandStatusTone(status) {
  const normalized = String(status || "").toLowerCase();
  if (normalized === "completed") return "completed";
  if (["failed", "rejected"].includes(normalized)) return "error";
  if (["accepted", "queued", "received"].includes(normalized)) return "pending";
  return "";
}

function formatTimestamp(timestamp) {
  const value = Number(timestamp);
  if (!value) return "Time pending";
  return new Date(value).toLocaleString();
}

// C-H1 workaround (audit): the CURRENTLY-FLASHED ESP1 firmware rejects FORCE_RUN with the single
// generic string "system is not safely idle" for any of 7 different internal conditions, with no
// way to tell which one fired. A reflash would fix this at the source (the dev-tree firmware already
// gives a specific reason); until then, cross-reference the diagnostics fields ESP1 already
// publishes to narrow it down as far as the data actually allows. Three of the seven conditions
// (uiMode, pendingExercise, a remote diag id) are not published anywhere and stay genuinely unknown
// -- this says so rather than guessing.
function explainNotSafelyIdle() {
  const d = liveData.diagnostics || {};
  const state = liveData.system?.state || "";
  if (Boolean(d.fault?.held) || state === "EMERGENCY_STOP") return "a fault is currently held — see the fault banner above and choose a recovery";
  if (Boolean(d.system?.workOrderActive) || Boolean(d.system?.pendingRun)) return "a run is already in progress";
  if (actuationsAreOff()) return "actuations are currently disabled";
  if (state && state !== "IDLE_STATE") return `the controller is not idle (reported state: ${state})`;
  return "a condition this dashboard cannot see directly (possibly a pump test running, the LCD menu open, or another session's Manual/Test hold) — check the Diagnostics tab or the LCD";
}

// New-F2 (audit): the 8 short codes forceTick() can pass to forceAbort() (ESP1/src/main.cpp, both
// trees) -- see the inline decode in renderCommandHistory() above.
const FORCE_ABORT_REASONS = {
  ACTUATIONS_OFF: "actuations were disabled",
  NOT_IDLE:       "the controller was no longer idle",
  IN_MENU:        "someone was using the LCD",
  WEB_MANUAL:     "the Manual/Test tab had taken control",
  BUSY:           "another run or test was in progress",
  HELD:           "a fault was held",
  RES_LOW:        "the reservoir was too low",
  COL_DISABLED:   "the target column was disabled",
  // Added by the web v.3 firmware: the rest of forceTick()'s codes, plus every other forceAbort() caller.
  TANK_CHECK:     "the mixing tank must be checked first (SMS TANKCHECK,OK or the LCD)",
  BATT2:          "the secondary battery blocked inverter use",
  // ESP1 from 17 Nov 2026 names which secondary-battery reason it was.
  BATT2_LOW:       "the second battery is at 30 % or less",
  BATT2_MONITOR:   "the second battery's monitor is not answering",
  BATT2_NOT_SEEN:  "the second battery is not seen (check its wiring and ground)",
  BATTSW_FAULT:    "a battery-switch fault is latched (clear it on the System tab)",
  BATTSW_RECHARGE: "the controllers had just restarted (a minute's wait)",
  MAIN_BATT_CRIT:  "the main battery is critical",
  REFUSED:        "ESP1 refused the work order",
  WEB:            "it was cancelled from this dashboard",
  OPERATOR:       "the operator cancelled it at the LCD",
  ACTUATIONS_DISABLED: "actuations were disabled",
  ESP2_TIMEOUT:   "ESP2 stopped responding",
  ESP2_NO_DONE:   "ESP2 never reported the run finished",
  ESP2_NO_READY:  "ESP2 never reported ready",
  ESP2_RESTARTED: "ESP2 restarted",
  COL_MISMATCH:   "ESP2 has that column disabled",
  WO_OVERSIZE:    "the work order was too long to send"
};

// ESP1's status detail is a 48-byte field, so any reply longer than 47 characters arrives cut off.
// These are the replies long enough to be affected, matched on a prefix that always survives the cut.
const COMMAND_DETAIL_FULL_TEXT = [
  ["batch too small to dose safely", `batch too small to dose safely: use at least ${FORCE_MIN_DOSING_LITERS} L of water, or remove the nutrients`],
  ["the secondary battery is not seen", "the secondary battery is not seen: check its wiring and ground"],
  ["the secondary battery monitor", "the secondary battery monitor (INA226) is not responding"],
  // With the colon: the success reply "battery-switch fault cleared" starts with the same words.
  ["battery-switch fault:", "battery-switch fault: a switch reset the controllers. Clear it on the System tab to retry"],
  ["the controllers just restarted", "the controllers just restarted: waiting for the hold-up to recharge"],
  ["the mixing tank must be checked first", "the mixing tank must be checked first. Confirm it with SMS TANKCHECK,OK or in the LCD Settings menu; this website cannot clear it"],
  // ESP1 from 5 Nov 2026 on refuses a cancel once the run has fired, instead of claiming to have cancelled it.
  ["run already started", "the run had already started, so nothing was cancelled. Use Emergency stop to stop it"]
];

function explainCommandDetail(detail) {
  const text = String(detail ?? "");
  const noFlow = text.match(/^(.*NO FLOW - 0 pulses)/);
  if (noFlow) return `${noFlow[1]}, check pump, line or sensor`;
  const known = COMMAND_DETAIL_FULL_TEXT.find(([start]) => text.startsWith(start));
  if (known) return known[1];
  // Cut off, with nothing to expand it from. "COL_A: mode=AUTO sched=AUTO N=140.0 P" otherwise reads as if P was
  // left unset. ESP1 applied the whole request: only its reply is short.
  return text.length === 47 ? `${text}…` : text;
}

// One place for turning ESP1's reply into words, for every panel that shows a command's outcome.
function commandDetailText(command, fallback) {
  let text = explainCommandDetail(rawText(command.detail, fallback));
  // ESP1 from before 5 Nov 2026 has no LCD_LOCK, so it gives every unknown command the same refusal.
  if (command.type === "LCD_LOCK" && /^not a remotely safe control/.test(text)) {
    return "this ESP1 firmware can't lock the rig from the website yet: it needs the update from 5 Nov 2026. Settings > Lock Screen on the LCD still works";
  }
  if (command.type === "LCD_LOCK" && /^someone is using the LCD menu/.test(text)) {
    return "someone at the rig is using the LCD's menus, so ESP1 won't lock it now. Try again once they have finished";
  }
  // AC voltage limits, added to ESP1 and ESP2 on 14 Nov 2026: older ESP1 gives every unknown command this refusal.
  if (command.type === "SET_AC_VOLT" && /^not a remotely safe control/.test(text)) {
    return "this ESP1 firmware doesn't have the AC voltage limits yet: ESP1 and ESP2 need the update from 14 Nov 2026";
  }
  if (command.type === "SET_AC_VOLT" && /^being edited on the LCD/.test(text)) {
    return "someone at the rig has Settings > AC Voltage open on the LCD, so ESP1 left it alone. Try again once they have finished";
  }
  // AC current limits from the website, added to ESP1 on 16 Nov 2026 (the LCD has had them since 15 Nov).
  if (command.type === "SET_AC_AMP" && /^not a remotely safe control/.test(text)) {
    return "this ESP1 firmware can't take the AC current limits from the website yet: ESP1 needs the update from 16 Nov 2026. Settings > AC Current on the LCD still works";
  }
  if (command.type === "SET_AC_AMP" && /^being edited on the LCD/.test(text)) {
    return "someone at the rig has Settings > AC Current open on the LCD, so ESP1 left it alone. Try again once they have finished";
  }
  // The pump selector (servoL / servoR). ESP1, ESP2 and the Nano have it from 15 Nov 2026. ESP1 of 15 Nov answers a
  // move in more than the 47 characters it can send, so the side's name arrives cut off: put it back from the request.
  const servoSide = command.type === "TEST_PULSE" ? SERVO_TARGETS[command.payload?.target] : undefined;
  if (servoSide && /^unknown pulse target/.test(text)) {
    return "this ESP1 firmware doesn't have the pump selector yet: ESP1, ESP2 and the Nano need the update from 15 Nov 2026";
  }
  if (servoSide && /^pulse complete; SERVO OK/.test(text)) return `pulse complete; SERVO OK: selector on the ${servoSide} side`;
  if (servoSide && /^pulse complete; SERVO NOT CONFIRMED/.test(text)) {
    return "pulse complete; SERVO NOT CONFIRMED: check the Nano link, servo power and the lever";
  }
  // New-F1 (audit): DIAG_SWEEP shares the same generic "not safely idle" wording as FORCE_RUN used
  // to (diagRemoteStart() has since been given specific reasons in the dev-tree firmware, but the
  // OLD flashed firmware's DIAG_SWEEP path -- and any other command that ever reuses this literal
  // text -- still benefits from the same best-guess decoding). explainNotSafelyIdle() doesn't
  // reference either command by name, so it's safe to reuse for both.
  if ((command.type === "FORCE_RUN" || command.type === "DIAG_SWEEP") && /not safely idle/i.test(text)) {
    text += ` — likely reason: ${explainNotSafelyIdle()}`;
  }
  if (command.type === "FORCE_RUN") {
    // New-F2 (audit): forceAbort() reports a short internal code verbatim (e.g. "cancelled before
    // start: RES_LOW"). Swap in the matching phrase when recognised.
    text = text.replace(/cancelled before start: (\S+)/,
      (full, c) => `cancelled before start: ${FORCE_ABORT_REASONS[c] || c}`);
  }
  return text;
}

// The newest command of one type, for a panel that shows that command's outcome inline.
function latestCommandOfType(type) {
  return commandData
    .filter(c => c.type === type)
    .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
}

// ESP1 picks up one queued row every 3 s, so one still sitting here after this long isn't being read.
const QUEUED_WARNING_MS = 30000;
// The stops can't be withdrawn, and the rules refuse it too, so one approved user cannot quietly withdraw
// another's emergency stop. They are only sent while ESP1 is reporting (stopOfflineReason(), 6 Nov 2026).
const NOT_WITHDRAWABLE = new Set(["EMERGENCY_STOP", "DISABLE_ACTUATIONS", "CANCEL_FORCE"]);

function renderCommandHistory() {
  const container = document.getElementById("commandHistory");
  if (!container) return;
  container.innerHTML = "";
  const commands = commandData.slice().sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0));
  if (!commands.length) {
    container.innerHTML = '<p class="muted">No command history received yet.</p>';
    return;
  }
  // ESP1's own window is the newest 10 rows, so a queued row can still run after it has scrolled past
  // the newest 8 shown here. It stays listed for as long as it is queued.
  commands.filter((command, index) => index < 8 || command.status === "queued").forEach(command => {
    const row = document.createElement("article");
    row.className = "command-row";
    const top = document.createElement("div");
    const name = document.createElement("strong");
    const badge = document.createElement("span");
    name.textContent = rawText(command.type, "Unknown command").replaceAll("_", " ");
    badge.className = `command-badge ${commandStatusTone(command.status)}`;
    badge.textContent = rawText(command.status, "unknown");
    const queued = command.status === "queued";
    const withdrawable = queued && !NOT_WITHDRAWABLE.has(command.type) && canWithdrawType(command.type);
    const right = document.createElement("span");
    right.className = "command-actions";
    if (withdrawable) {
      const withdraw = document.createElement("button");
      withdraw.type = "button";
      withdraw.className = "user-action";
      withdraw.dataset.withdraw = command.id;
      withdraw.textContent = "Withdraw";
      right.append(withdraw);
    }
    right.append(badge);
    top.append(name, right);
    const detail = document.createElement("p");
    let detailText = commandDetailText(command, "Awaiting ESP1 status");
    const queuedForMs = serverNow() - Number(command.requestedAt || 0);
    if (queued && queuedForMs > QUEUED_WARNING_MS) {
      detailText = `ESP1 hasn't picked this up for ${formatAge(queuedForMs)}. It will still run when ESP1 next reads it${withdrawable ? ", unless you withdraw it" : ""}.`;
    }
    detail.textContent = `${formatTimestamp(command.requestedAt)} — ${detailText}`;
    row.append(top, detail);
    container.appendChild(row);
  });
}

// Who may take a queued row back (2026-11-05, as the rules): Control for any row, Edit zones for a column's settings.
function canWithdrawType(type) {
  return hasControlAccess() || (type === "SET_COLUMN" && hasZoneEditAccess());
}

// Only for a row still "queued": once ESP1 has picked it up the rules refuse the write, and if ESP1's
// reply lands just after this one it overwrites it. So the row itself is the only honest result.
function withdrawCommand(id) {
  const command = commandData.find(c => c.id === id);
  if (!command || command.status !== "queued" || NOT_WITHDRAWABLE.has(command.type)) return;
  if (!currentUserIsSignedIn() || !canWithdrawType(command.type)) return;
  const label = rawText(command.type, "Command").replaceAll("_", " ");
  db.ref(`irrigation/commands/${id}`).update({
    status: "withdrawn",
    detail: "withdrawn from the dashboard",
    updatedAt: firebase.database.ServerValue.TIMESTAMP
  }).then(() => {
    setCommandStatus(`${label} withdrawal sent. Check the row in Command status: ESP1 may have picked it up first.`, "pending");
    logActivity(`WITHDREW ${command.type}`);
  }).catch(error => setCommandStatus(error.code === "PERMISSION_DENIED"
    ? `Too late to withdraw ${label}: ESP1 has already picked it up.`
    : `Could not withdraw ${label}: ${error.message}`, "error"));
}

document.getElementById("commandHistory")?.addEventListener("click", event => {
  const button = event.target.closest("[data-withdraw]");
  if (button) withdrawCommand(button.dataset.withdraw);
});

/* ---- Fault / recovery banner ------------------------------------------------------------------
 * Rendered from diagnostics.fault, so the choices offered here are exactly the ones the LCD recovery
 * menu offers. Recovery commands are sent with {emergency:true}: they bypass the freshness gate and
 * the cooldown on purpose, because a stale snapshot is often *why* you are trying to recover, and a
 * dashboard that locks you out at that moment is worse than useless. */
let faultAckLocalUntil = 0;          // client-side half of the "ask me again in 2 minutes" snooze
// The faults that hold a run (enterFaultHold() in the firmware). ESP1 reports everything in one text field, so
// after pressing Hold it says "HELD - waiting" there, and "Resume refused: ..." after a refused resume, and a
// minor fault can land there too. The banner must go on naming the fault that is actually being held.
const HELD_FAULT_CODE = /^(ESP2_ERROR|FLOW_FAIL|PWR_FAIL|SAFE_STOP|DOSE_TIMEOUT|LEVEL_BLIND|SERVO_FAIL)\b/;
let heldFaultCode = "";

// Tracks held||stopped SPECIFICALLY (not lockedOut) across snapshots so the moment it clears can be
// caught and explained -- otherwise "ESP2 confirmed the resume", "the auto-cancel circuit breaker
// gave up", "an operator cancelled it", and "ESP2 appears to have restarted" were all
// indistinguishable: the banner just disappeared in every case, with no way to tell a real recovery
// from the rig quietly giving up. Deliberately excludes lockedOut: lastClearReason is published only
// by the four esp2Held-clear sites, so a plain actuations-lockout toggle with no fault ever held
// would otherwise show a stale reason left over from a completely unrelated earlier fault episode.
let wasHeldOrStopped = false;
const HELD_CLEAR_TEXT = {
  resumed:         { text: "Confirmed: ESP2 resumed the paused run.", tone: "" },
  cancelled:       { text: "The run was cancelled.", tone: "" },
  // ESP1 records this on every emergency stop, held fault or not, and "held" may never have applied.
  estop:           { text: "The emergency stop has ended. ESP1 re-checks ESP2 before returning to normal.", tone: "" },
  auto_cancelled:  { text: "The rig gave up retrying and auto-cancelled the run after repeated identical faults — this was not a successful recovery.", tone: "error" },
  esp2_restarted:  { text: "ESP2 appears to have restarted rather than confirming the resume — the paused run may not have continued as expected.", tone: "error" }
};

// Diagnostic-detail decoder for fault codes that carry extra machine-readable detail after the
// location (ESP1's holdFault() "|pulses=" for FLOW_FAIL, and the equivalent enrichment on
// SOIL_MISSING). Returns "" for anything it doesn't recognise so callers can just append when
// non-empty, rather than guessing at codes this hasn't been told about.
function explainFaultCode(code) {
  const text = String(code || "");
  const flow = text.match(/^FLOW_FAIL\s+(\S+)\|pulses=(\d+)/);
  if (flow) {
    const stage = flow[1], pulses = Number(flow[2]);
    return pulses === 0
      ? `Zero flow pulses were counted during the ${stage} stage — consistent with a dead pump, a closed/stuck valve, or a disconnected flow sensor.`
      : `Flow started (${pulses} pulses counted) then stalled during the ${stage} stage — consistent with an air lock, a weakening pump, or a sensor only partially in the flow path.`;
  }
  // web v.4 firmware: water is metered by the tank ultrasonics, so a FLOW_FAIL carries what the levels saw
  // (moved=<litres>) and the rule that fired, instead of a pulse count. Both pumps sit below their tanks and are
  // always primed, so "nothing moved" is never a pump still priming.
  const lvl = text.match(/^FLOW_FAIL\s+(TRANSFER|MAIN|FLUSH)\|(NO_LEVEL_CHANGE|STALL|CAP)\b/);
  if (lvl) {
    const stage = lvl[1] === "TRANSFER" ? "the fill from the reservoir to the mixing tank" : "the delivery from the mixing tank to the column";
    const moved = text.match(/\|moved=(-?[\d.]+)/);
    const m = moved ? ` (${Number(moved[1]).toFixed(2)} L moved)` : "";
    if (lvl[2] === "NO_LEVEL_CHANGE") return `The tank levels did not change during ${stage}${m}: a failed pump, a shut valve, a blocked line or filter, or the pump selector on the wrong pump.`;
    const left = text.match(/\|left=([\d.]+)/);
    if (lvl[2] === "STALL" && left) return `The delivery stopped with ${Number(left[1]).toFixed(1)} L still in the mixing tank${m}: the column was not watered. Check the line, the column valve and the booster pump.`;
    if (lvl[2] === "STALL") return `Water moved, then stopped during ${stage}${m}: a blocked line or filter, a shut valve, or a failed pump.`;
    return `${stage[0].toUpperCase()}${stage.slice(1)} ran past its time limit${m}: the pump is much slower than expected. Check the line and filter, and the pump rates on the LCD (Calibration).`;
  }
  if (/^ESP2_DEGRADED\s+LEVEL_BLIND\|FILL\b/.test(text)) {
    return "The mixing tank's level was lost during the fill. The transfer carried on by its run time, cut short to keep a quarter of the tank's room free, and the tank is measured again after it settles (topped up if short). Check the mixing-tank ultrasonic: the log's NANO,LEVEL lines say why its readings were not used.";
  }
  if (/^LEVEL_BLIND\b/.test(text)) {
    return "The mixing tank's ultrasonic gave no usable reading, so ESP2 stopped rather than fill blind (it could overflow). Check the Nano and the sensor. Release or irrigate-only then run on a timer.";
  }
  if (/^SERVO_FAIL\b/.test(text)) {
    return /STUCK/.test(text)
      ? "The pump selector stayed on the wrong pump after it was re-seated. Check the servo and the selector lever before choosing."
      : "The pump selector could not be confirmed, so the inverter was never switched on. Check the Nano and the servo.";
  }
  const weak = text.match(/^ESP2_DEGRADED\s+PUMP_WEAK\|(\w+)\|levels=([\d.]+)\|by_rate=([\d.]+)/);
  if (weak) {
    return `The ${weak[1].toLowerCase()} pump moved ${weak[2]} L where its run time says ${weak[3]} L: under half. The run carried on (the check after the water settles tops the tank up). Check for a blocked filter, a half-shut valve or a worn pump, or the pump's rate on the LCD (Calibration).`;
  }
  // web v.4 from 17 Nov 2026: a fill is timed by the transfer pump's run time, then checked on the mixing tank once the
  // water has settled (about a minute) and topped up when more than 3 L short.
  const fillShort = text.match(/^ESP2_DEGRADED\s+FILL_SHORT\|goal=([\d.]+)\|measured=([\d.]+)\|topups=(\d+)/);
  if (fillShort) {
    return `After the transfer and ${fillShort[3]} top-up run${fillShort[3] === "1" ? "" : "s"}, the mixing tank held ${fillShort[2]} L of the ${fillShort[1]} L planned: still more than 3 L short. The run carried on with what was there. Check the reservoir, the transfer pump and its filter, or the Rate Transfer setting on the LCD.`;
  }
  if (/^ESP2_DEGRADED\s+FILL_NOT_STEADY\b/.test(text)) {
    return "After the transfer, the mixing tank's level did not settle within two and a half minutes; ESP2 measured it anyway. Check for a leak, a valve letting water through, or a noisy ultrasonic reading.";
  }
  if (/^ESP2_DEGRADED\s+FILL_SETTLE_BLIND\b/.test(text)) {
    return "The mixing tank's ultrasonic gave no reading after the transfer, so the fill could not be checked. The run carried on with the run-time estimate (the water total is marked as estimated). Check the Nano and the sensor.";
  }
  const jump = text.match(/^ESP2_DEGRADED\s+LEVEL_JUMP\|(FILL|DRAIN)\|moved=([\d.]+)\|s=([\d.]+)/);
  if (jump) {
    return `The mixing tank's level said the ${jump[1] === "FILL" ? "fill" : "delivery"} was done (${jump[2]} L in ${jump[3]} s), faster than the pump can move water. ESP2 treated it as a bad reading and kept going. Check the ultrasonic sensor and its cable.`;
  }
  if (/^ESP2_DEGRADED\s+FLOW_NOISE\b/.test(text)) {
    return "A nutrient flow meter counted a burst of pulses no pump can produce: electrical noise on its cable (a relay or the inverter switching nearby). ESP2 left the burst out of the dose. Check the meter cable's routing, shielding and ground.";
  }
  if (/^ESP2_DEGRADED\s+LEVEL_DISAGREE\b/.test(text)) {
    return "During a fill the mixing tank's rise and the reservoir's drop disagreed. The run carried on; check both ultrasonics and the tank calibrations.";
  }
  const soil = text.match(/^SOIL_MISSING\s+(COL_[ABC])\|(\S+)\|raw=(-?\d+),(-?\d+)/);
  if (soil) {
    const [, col, reason, r1, r2] = soil;
    return reason === "PROBE_DIVERGE"
      ? `${col}'s two soil probes disagree beyond tolerance (raw ${r1}/${r2}) — the NPK sensor is covering the reading for now, but one probe likely needs attention.`
      : `${col} has no usable soil reading (raw ${r1}/${r2}) — check that column's probe wiring/connector.`;
  }
  // New-F3/F4/F5 (audit): same decode pattern, for the three fault codes that gained measured-value
  // detail in this pass (ESP2/src/main.cpp -- PWR_FAIL/SAFE_STOP,MIXER_OC/DOSE_TIMEOUT).
  // ESP2 from 15 Nov 2026 adds the current limit in force (|max= / |min=), since the AC current limits are adjustable.
  const overcurrent = text.match(/^PWR_FAIL\s+OVERCURRENT\|i=([\d.]+)(?:\|max=([\d.]+))?/);
  if (overcurrent) {
    const limit = overcurrent[2] ? ` (the overcurrent limit is ${overcurrent[2]}A)` : "";
    return `Current draw hit ${overcurrent[1]}A during this stage${limit} — check for a jammed pump/valve or a wiring fault, or the AC current limits on the System tab.`;
  }
  // ESP2 from 14 Nov 2026 adds the limit in force (|min= / |max=), since the AC voltage limits are adjustable.
  const voltage = text.match(/^PWR_FAIL\s+VOLTAGE\|v=([\d.]+)\|(LOW|HIGH)(?:\|(?:min|max)=([\d.]+))?/);
  if (voltage) {
    const limit = voltage[3] ? ` (the ${voltage[2] === "LOW" ? "low" : "high"} limit is ${voltage[3]}V)` : "";
    return voltage[2] === "LOW"
      ? `AC supply sagged to ${voltage[1]}V${limit} — check the mains/inverter output and connections, or the AC voltage limits on the System tab.`
      : `AC supply spiked to ${voltage[1]}V${limit} — check the mains/inverter output, or the AC voltage limits on the System tab.`;
  }
  const noCurrent = text.match(/^PWR_FAIL\s+NO_CURRENT\|i=([\d.]+)(?:\|min=([\d.]+))?/);
  if (noCurrent) {
    const limit = noCurrent[2] ? `, under the no-current limit of ${noCurrent[2]}A,` : "";
    return `The pump was commanded on but drew almost no current (${noCurrent[1]}A)${limit} for too long — likely a dry run, a failed pump, a tripped breaker or loose wiring, or check the AC current limits on the System tab.`;
  }
  const mixerOc = text.match(/^SAFE_STOP\s+MIXER_OC\|i=([\d.]+)/);
  if (mixerOc) return `The mixer motor drew ${mixerOc[1]}A, over its safety limit — check for a jammed impeller or motor fault.`;
  const dose = text.match(/^DOSE_TIMEOUT\s+(NUT_[ABC])\|delivered=([\d.]+)/);
  if (dose) {
    const [, nut, ml] = dose;
    return Number(ml) === 0
      ? `${nut}'s dosing pump timed out with zero mL delivered — likely a dead pump or an unprimed line.`
      : `${nut}'s dosing pump timed out after only ${ml} mL — likely a weak pump or a partial blockage.`;
  }
  // New-F6/F7/F8 (audit): PH_FAIL/EC_FAIL now carry the measured value + which bound it crossed;
  // SENSOR_FAIL,PH|EC now carry which ADC rail was hit. The mixed batch was still delivered either
  // way (no drain) -- these explain why, not what to do about the run itself.
  const ph = text.match(/^PH_FAIL\s+(COL_[ABC])\|pH=([\d.]+)\|(LOW|HIGH)/);
  if (ph) {
    const [, col, val, dir] = ph;
    return `${col}'s mixed batch pH read ${val} — too ${dir === "LOW" ? "acidic" : "alkaline"} for the safe window. Delivery still went ahead (no drain); check the pH probe/dosing.`;
  }
  const ecFail = text.match(/^EC_FAIL\s+(COL_[ABC])\|EC=([\d.]+)\|(LOW|HIGH)/);
  if (ecFail) {
    const [, col, val, dir] = ecFail;
    return `${col}'s mixed batch EC read ${val} mS/cm — too ${dir === "LOW" ? "dilute" : "concentrated"} for the safe window. Delivery still went ahead (no drain); check dosing amounts/calibration.`;
  }
  const sensorFail = text.match(/^SENSOR_FAIL\s+(PH|EC)\|RAILED_(LOW|HIGH)/);
  if (sensorFail) {
    const [, which, rail] = sensorFail;
    return `The ${which === "PH" ? "pH" : "EC"} probe's raw reading is railed ${rail === "LOW" ? "low" : "high"} — consistent with a ${rail === "LOW" ? "shorted" : "disconnected/open"} probe or connector.`;
  }
  // New-F9/F10 (audit): same decode pattern for the two most recently enriched fault codes.
  const batchLow = text.match(/^DOSE_BATCH_LOW\s+(COL_[ABC])\|V=([\d.]+)\|min=([\d.]+)/);
  if (batchLow) {
    const [, col, v, min] = batchLow;
    return `${col}'s planned batch (${v} L) fell under the ${min} L safe minimum, so it watered without dosing this run — this is a WATER_BUDGET_L/FLUSH_PCT/MIXING_TANK_SAFE_MIN tuning question, not a hardware fault.`;
  }
  const npkFault = text.match(/^NPK_FAULT\s+(COL_[ABC])\|reason=(\S+)/);
  if (npkFault) {
    const [, col, reason] = npkFault;
    const why = { TIMEOUT: "the sensor never responded — check its power/wiring", BADADDR: "a different device answered — check the Modbus address", BADLEN: "the reply was the wrong length", BADCRC: "the reply failed its checksum — consistent with bus noise/EMI" }[reason] || reason;
    return `${col} fertigated as irrigation-only this cycle because its NPK probe reading was invalid (${why}).`;
  }
  return "";
}

function updateFaultBanner() {
  const banner = document.getElementById("faultBanner");
  if (!banner) return;
  const d = liveData.diagnostics || {};
  const f = d.fault || {};
  const state = String(f.state || liveData.system?.state || "");
  const held = Boolean(f.held);
  const stopped = state === "EMERGENCY_STOP";
  const lockedOut = actuationsAreOff();

  // Nothing wrong -> no banner. A lockout is not a fault, but it must still be visible and
  // reversible, so it raises the banner in a calmer form.
  if (!held && !stopped && !lockedOut) {
    if (wasHeldOrStopped) {
      const clear = HELD_CLEAR_TEXT[f.lastClearReason] || { text: "The hold cleared.", tone: "" };
      setCommandStatus(clear.text, clear.tone);
    }
    wasHeldOrStopped = false;
    banner.hidden = true; faultAckLocalUntil = 0; return;
  }
  wasHeldOrStopped = wasHeldOrStopped || held || stopped;

  // "Do nothing" snooze. Honour whichever of the device's countdown or our own is still running, so
  // the prompt reappears even if the snapshot is stale.
  const deviceAck = Number(f.ackSecondsLeft || 0);
  const snoozed = deviceAck > 0 || Date.now() < faultAckLocalUntil;
  const ackNote = document.getElementById("faultAck");
  if (ackNote) {
    const left = Math.max(deviceAck, Math.ceil((faultAckLocalUntil - Date.now()) / 1000));
    ackNote.hidden = !snoozed;
    if (snoozed) ackNote.textContent = `Acknowledged — this prompt will return in ${Math.max(0, left)}s. The system is still in this state.`;
  }
  banner.hidden = false;
  banner.classList.toggle("snoozed", snoozed);
  banner.classList.toggle("lockout-only", !held && !stopped && lockedOut);

  setText("faultKind", held ? "Held fault — awaiting your decision"
                     : stopped ? "Emergency stop active"
                     : "Actuations disabled");
  // Kept with the banner's own update, which runs every second, rather than only with the controls.
  const lockNote = document.getElementById("faultLockNote");
  if (lockNote) lockNote.hidden = !rigLocked() || (!held && !stopped && !lockedOut);
  const latestText = String(f.code || "");
  if (!held) heldFaultCode = "";
  else if (HELD_FAULT_CODE.test(latestText)) heldFaultCode = latestText;
  const heldName = heldFaultCode || latestText;
  setText("faultTitle", held ? rawText(heldName, "Fault reported by ESP2")
                       : stopped ? "The system is stopped"
                       : "Monitoring only — nothing will run");
  const faultExplain = held ? explainFaultCode(heldName) : "";
  const faultNews = held && latestText !== heldName ? ` ESP1's latest message: ${latestText}.` : "";
  setText("faultDetail", held
    ? `Reported ${rawText(f.at, "at an unknown time")}. ESP2 is paused with the actuator bank de-energised; pick how to continue.` + (faultExplain ? ` ${faultExplain}` : "") + faultNews
    : stopped
      ? "All actuators are off and ESP2 is unpowered. Returning to normal re-powers and re-validates ESP2 before anything runs."
      : "Pumps and valves are switched off. Sensors, logging and this website keep running, but scheduled runs, pump exercises and forced runs are all blocked until they are switched back on.");
  setDeviceStatus("faultState", state || "UNKNOWN", held || stopped ? "danger" : "off");

  // The re-hold guard, surfaced rather than hidden: after repeated identical holds ESP1 steers
  // toward Release and eventually self-cancels, so "Resume normal" is not an endless option.
  const steer = document.getElementById("faultSteer");
  if (steer) {
    const show = held && (f.steerRelease || Number(f.repeats || 0) > 1);
    steer.hidden = !show;
    if (show) {
      steer.textContent = f.steerRelease
        ? `This fault has held ${f.repeats} times. Resuming normally keeps re-holding — Release tank or Only irrigate run on a timer instead and will actually complete. The run self-cancels at ${rawText(f.autoCancelAt, "4")} holds.`
        : `This fault has held ${f.repeats} times.`;
    }
  }

  // Without Control (2026-11-05) the banner only informs: the choices are for accounts that can act on them.
  const control = hasControlAccess();
  const rec = document.getElementById("faultRecovery");
  if (rec) rec.hidden = !held || !control;
  const estopBtn = document.getElementById("estopRecoverBtn");
  if (estopBtn) estopBtn.hidden = !stopped || held || !control;
  const enableBtn = document.getElementById("enableActBtn");
  if (enableBtn) enableBtn.hidden = !lockedOut || !control;
  const ackBtn = document.getElementById("ackFaultBtn");
  if (ackBtn) ackBtn.hidden = !control;
  const viewerNote = document.getElementById("faultViewerNote");
  if (viewerNote) viewerNote.hidden = control;
}

/* ---- Armed forced-run countdown ---------------------------------------------------------------
 * Driven by diagnostics.forceArmed, NOT by whether this browser sent the request -- an LCD-armed run
 * must show here too. secondsLeft is re-seeded on every snapshot and ticked locally in between, so
 * the number stays smooth at a 20-60 s publish cadence without ever drifting past the truth.
 *
 * The snapshot alone is not enough, though. Arming does not trigger a publish, and ESP1 publishes every
 * 60 s when idle, so with the default 30 s delay the panel, and with it the only Cancel button, could
 * miss the whole countdown. The command row arrives within seconds of arming ("armed: starts in 30s"),
 * so a run armed from this page shows from that row. Timed against Firebase's clock, because the row can
 * be seen long after it was written, by a page loaded later. Runs every second, and from each snapshot
 * and command update. */
let armedSeed = null;                // { stamp, at: epoch ms, left: seconds } from the last armed snapshot
let forceArmedInfo = null;           // the armed run as the panel last showed it, for Up next (renderUpNext())
// A snapshot can carry data from several seconds before its own stamp (one second of telemetry age, plus
// a slow upload). One this much newer than the arming reply has definitely seen the countdown.
const ARMED_SNAPSHOT_SETTLE_MS = 8000;

// Has a reply written after this moment ended the countdown? ESP1 doesn't publish a snapshot when one
// ends, so its last snapshot can go on saying "armed" for up to a minute. An emergency stop doesn't update
// the run's own row at all, so that row can't be relied on by itself either.
function countdownEndedAfter(stamp) {
  return commandData.some(row => {
    if (Number(row.updatedAt || 0) <= stamp) return false;
    if (row.type === "CANCEL_FORCE" || row.type === "EMERGENCY_STOP" || row.type === "DISABLE_ACTUATIONS") return row.status === "completed";
    if (row.type !== "FORCE_RUN") return false;
    return row.status === "failed" || row.status === "completed" || String(row.detail || "").startsWith("force run started");
  });
}

function armedRowFromCommands() {
  // The newest row that armed a run, not simply the newest row: a second request refused with "another
  // forced run is already armed" must not hide the first one's Cancel.
  const row = commandData
    .filter(c => c.type === "FORCE_RUN" && c.status === "accepted" && /^armed: starts in \d+s/.test(String(c.detail || "")))
    .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))[0];
  const stamp = Number(row?.updatedAt || 0);
  if (!stamp || countdownEndedAfter(stamp)) return null;
  const snapshotStamp = Number(liveData.meta?.updatedAt || 0);
  if (snapshotStamp > stamp + ARMED_SNAPSHOT_SETTLE_MS && !liveData.diagnostics?.forceArmed?.armed) return null;
  const left = Number(/^armed: starts in (\d+)s/.exec(row.detail)[1]) - Math.floor((serverNow() - stamp) / 1000);
  return left > 0 ? { left, payload: row.payload || {} } : null;
}

function updateForceArmed() {
  const panel = document.getElementById("forceArmedPanel");
  if (!panel) return;
  const a = liveData.diagnostics?.forceArmed || {};
  const snapshotArmed = Boolean(a.armed) && deviceIsFresh() && !countdownEndedAfter(Number(liveData.meta?.updatedAt || 0));
  const row = armedRowFromCommands();
  if (!snapshotArmed) {
    armedSeed = null;
  } else if (armedSeed?.stamp !== liveData.meta?.updatedAt) {
    armedSeed = { stamp: liveData.meta?.updatedAt, at: Date.now(), left: Number(a.secondsLeft || 0) };
  }
  // The forced-run and hold-to-run buttons are off while a run is armed, so they follow the panel.
  const shown = snapshotArmed || Boolean(row);
  if (shown !== forceArmedShown) {
    forceArmedShown = shown;
    syncControlAvailability();
  }
  if (!shown) { panel.hidden = true; forceArmedInfo = null; return; }
  panel.hidden = false;
  const run = snapshotArmed ? a : row.payload;
  const source = snapshotArmed ? a.source : "web";
  const doses = run.doseMl || {};
  const fert = Number(doses.A || 0) + Number(doses.B || 0) + Number(doses.C || 0) > 0;
  setText("forceArmedDetail",
    `${fert ? "Fertigation" : "Irrigation"} · column ${rawText(run.columns, "?")} · ${numberText(run.liters, 1, "L")}`
    + (fert ? ` · A/B/C ${Number(doses.A || 0)}/${Number(doses.B || 0)}/${Number(doses.C || 0)} mL` : "")
    + ` · armed from the ${source === "web" ? "dashboard" : "LCD"}`);
  const left = row ? row.left : Math.max(0, armedSeed.left - Math.floor((Date.now() - armedSeed.at) / 1000));
  forceArmedInfo = { left, columns: run.columns, liters: run.liters, fert };
  setDeviceStatus("forceArmedCountdown", left > 0 ? `STARTS IN ${left}s` : "STARTING…", "active");
  // Past this point Cancel can't stop anything, though ESP1 would still reply "cancelled". Like every stop, it isn't
  // queued while ESP1 isn't reporting (stopOfflineReason()); BACK on the LCD still cancels the countdown.
  const offline = stopOfflineReason();
  const cancel = document.getElementById("cancelForceBtn");
  if (cancel) {
    cancel.disabled = left <= 0 || Boolean(offline);
    cancel.title = left <= 0 ? "Too late to cancel. Use Emergency stop to stop the run." : offline;
  }
  const cancelNote = document.getElementById("cancelForceOffline");
  if (cancelNote) {
    cancelNote.hidden = !offline || left <= 0;
    if (offline) cancelNote.textContent = `${!firebaseConnected ? "This browser is offline" : "ESP1 isn't reporting"}, so Cancel can't be sent from here. Press BACK on the LCD before the countdown ends, or send SMS STOP,ALL.`;
  }
}

/* ---- Up next (2026-11-06) ----------------------------------------------------------------------
 * Beside Current run: what ESP1 will do next, by its own clock. Each switched-on column's watering times still to come
 * today and all of tomorrow's, a forced run counting down, and when each pump's exercise is due. Worked out from ESP1's
 * clock at its last report (system.deviceTime, moved on by the report's age), each column's windows and how many of
 * today's it has done (runsToday), and pumpExercise.nextDueMin (ESP1 from 6 Nov 2026). A watering time only waters if
 * the soil is drier than Soil start % then, so each says so. ESP1 doesn't say WHICH of today's times are done, only how
 * many: a time open now counts as done once as many are done as have started. Redrawn only when its text changes. */
const EXERCISE_PUMPS = ["Transfer pump", "Booster pump", "Tank mixer"];   // the order ESP1 publishes them in (EX_NAME)
const EXERCISE_IDS = ["TRANSFER", "BOOSTER", "MIXER"];                    // ESP1's EX_NAME, as in actuator.pumpUnderTest
// A no-break space, made from its code so the source stays plain ASCII (the linter refuses the character itself).
const NBSP = String.fromCharCode(160);
let upNextKey = "";

// ESP1's own time now: its clock at its last report plus the report's age. Null when its clock can't be trusted.
function rigNow() {
  const parts = /^(\d{4})-(\d{1,2})-(\d{1,2}) (\d{1,2}):(\d{2})(?::(\d{2}))?/.exec(String(liveData.system?.deviceTime || ""));
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (!parts || !stamp || liveData.system?.rtcOk === false) return null;
  const [y, mo, d, h, mi, s] = parts.slice(1).map(v => Number(v || 0));
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) return null;
  return new Date(new Date(y, mo - 1, d, h, mi, s).getTime() + Math.max(0, serverNow() - stamp));
}

function dayStart(date) {
  return new Date(date.getFullYear(), date.getMonth(), date.getDate());
}

function clockText(date) {
  return minutesToHhmm(date.getHours() * 60 + date.getMinutes());
}

function upNextItems(now) {
  const items = [];
  const nowMin = now.getHours() * 60 + now.getMinutes();
  const today = dayStart(now);
  const tomorrow = dayStart(new Date(today.getTime() + 36 * 3600000));
  const ifDry = `Waters if drier than ${soilStartPercent()}${NBSP}%`;   // kept together: a lone "%." wrapped onto its own line
  ["A", "B", "C"].forEach(id => {
    const s = zoneSettings(id);
    if (!s.known || !s.enabled || s.sched === null || !s.windows?.length) return;
    const span = ([a, b]) => `${minutesToHhmm(a)}–${minutesToHhmm(b)}`;
    const started = s.windows.filter(([a]) => a <= nowMin).length;
    const done = s.runsToday === null ? 0 : s.runsToday;
    s.windows.forEach(w => {
      const [a, b] = w;
      // ESP1 counts both ends as inside a window (windowAt() in its main.cpp), so 12:00–13:00 is still open at 13:00.
      if (done >= s.windows.length || b < nowMin) return;           // the day is done, or this time is over
      const open = a <= nowMin;
      if (open && done >= started) return;                          // it has watered in this one already
      items.push({ day: today, at: open ? nowMin : a, time: span(w), what: `Column ${id}`, note: open ? `Open now. ${ifDry}.` : `${ifDry}.`, open });
    });
    s.windows.forEach(w => items.push({ day: tomorrow, at: w[0], time: span(w), what: `Column ${id}`, note: `${ifDry}.` }));
  });
  if (forceArmedInfo) {
    const start = new Date(now.getTime() + Math.max(0, forceArmedInfo.left) * 1000);
    items.push({
      day: dayStart(start), at: start.getHours() * 60 + start.getMinutes(), time: clockText(start), forced: true,
      what: `Forced run, column ${rawText(forceArmedInfo.columns, "?")}`,
      note: `${numberText(forceArmedInfo.liters, 1, "L")}, ${forceArmedInfo.fert ? "with nutrients" : "plain water"}. Counting down.`
    });
  }
  const ex = liveData.diagnostics?.pumpExercise;
  if (ex?.enabled && Array.isArray(ex.nextDueMin)) {
    const ageMin = Math.max(0, serverNow() - Number(liveData.meta?.updatedAt || 0)) / 60000;
    // The pump ESP1 is exercising (or warming ESP2 up for) right now: its clock only restarts once that finishes.
    const actuator = liveData.diagnostics?.actuator || {};
    const busy = deviceIsFresh() && actuator.pumpTestActive ? String(actuator.pumpUnderTest || "") : "";
    ex.nextDueMin.slice(0, EXERCISE_PUMPS.length).forEach((minutes, k) => {
      if (!hasValue(minutes)) return;
      const seconds = hasValue(ex.seconds) ? `${Number(ex.seconds)} s` : "A few seconds";
      if (busy === EXERCISE_IDS[k]) {
        items.push({ day: dayStart(now), at: nowMin, time: "Now", what: `${EXERCISE_PUMPS[k]} exercise`, note: `${seconds}, under way now.`, open: true });
        return;
      }
      const left = Number(minutes) - ageMin;
      const due = new Date(Math.round((now.getTime() + Math.max(0, left) * 60000) / 60000) * 60000);
      items.push({
        day: dayStart(due), at: left <= 0 ? nowMin : due.getHours() * 60 + due.getMinutes(),
        time: left <= 0 ? "Due now" : `About ${clockText(due)}`, what: `${EXERCISE_PUMPS[k]} exercise`,
        note: `${seconds}, ${left <= 0 ? "once the rig is idle" : "if the rig is idle then"}.`
      });
    });
  }
  return items.sort((x, y) => x.day - y.day || x.at - y.at);
}

// Why nothing on the list may happen, in words: the standing reasons ESP1 publishes.
function upNextBlockText() {
  const d = liveData.diagnostics || {};
  const state = liveData.system?.state;
  const reasons = [];
  if (!deviceIsFresh()) reasons.push("ESP1 isn't reporting, so this is from its last report.");
  if (state === "EMERGENCY_STOP" && !d.fault?.held) reasons.push("The emergency stop is on: none of this runs until Return to normal.");
  else if (d.fault?.held) reasons.push("A fault is held: none of this runs until someone chooses how to continue.");
  if (actuationsAreOff()) reasons.push("Pumps and valves are off: none of this runs until they are switched back on.");
  if (d.webManual?.held === true) reasons.push("Manual/Test is open: nothing starts by itself until it is closed.");
  const reservoir = Number(liveData.sensors?.reservoirLevel);
  if (hasValue(liveData.sensors?.reservoirLevel) && reservoir < RIG_RES_LOW_PCT) reasons.push(`The reservoir is under ${RIG_RES_LOW_PCT} %: no column is watered until it is refilled.`);
  return reasons.join(" ");
}

function renderUpNext() {
  const list = document.getElementById("upNextList");
  const status = document.getElementById("upNextStatus");
  if (!list || !status) return;
  let note = "";
  let html;
  const now = liveData.system ? rigNow() : null;
  if (!liveData.system) {
    html = `<p class="muted">Waiting for ESP1's report.</p>`;
  } else if (!now) {
    note = "ESP1's clock can't be read, so it isn't keeping its watering times. Operators can set the clock on the System tab.";
    html = "";
  } else {
    note = upNextBlockText();
    const items = upNextItems(now);
    const today = dayStart(now).getTime();
    const days = [...new Set([today, ...items.map(item => item.day.getTime())])].sort((x, y) => x - y);
    html = days.map(day => {
      const rows = items.filter(item => item.day.getTime() === day);
      const date = new Date(day);
      // "Fri 6 Nov" in any locale: some write it "Fri, Nov 6", which reads badly after "Today, ".
      const when = `${date.toLocaleDateString([], { weekday: "short" })} ${date.getDate()} ${date.toLocaleDateString([], { month: "short" })}`;
      const offset = Math.round((day - today) / 86400000);
      const title = offset === 0 ? `Today, ${when}` : offset === 1 ? `Tomorrow, ${when}` : when;
      return `<section class="next-day"><h4>${escapeHtml(title)}</h4>${rows.length
        ? `<ul>${rows.map(item => `<li class="${item.open ? "is-open" : item.forced ? "is-forced" : ""}"><span class="next-time">${escapeHtml(item.time)}</span><span class="next-what">${escapeHtml(item.what)}<span class="next-note">${escapeHtml(item.note)}</span></span></li>`).join("")}</ul>`
        : `<p class="next-none">${offset === 0 ? "Nothing more today." : "Nothing planned."}</p>`}</section>`;
    }).join("");
    const ex = liveData.diagnostics?.pumpExercise;
    if (ex && !ex.enabled) html += `<p class="next-foot">Pump exercise is switched off.</p>`;
    else if (ex?.enabled && !Array.isArray(ex.nextDueMin)) html += `<p class="next-foot">Each pump is exercised every 2 days. This firmware doesn't say when each is due.</p>`;
    const on = ["A", "B", "C"].map(zoneSettings).filter(s => s.known && s.enabled);
    if (!on.length) html += `<p class="next-foot">No column is switched on, so ESP1 has nothing to water.</p>`;
    else if (!on.some(s => s.sched !== null && s.windows?.length)) html += `<p class="next-foot">ESP1 hasn't reported the columns' watering times yet.</p>`;
  }
  const key = `${note}\u0000${html}`;
  if (key === upNextKey) return;
  upNextKey = key;
  status.hidden = !note;
  status.textContent = note;
  list.innerHTML = html;
}

/* ---- Raw sensor diagnostics -------------------------------------------------------------------- */
function ageFlag(ms) {
  const v = Number(ms);
  if (!Number.isFinite(v) || v === 0xFFFFFFFF) return "never";
  // The firmware's NANO_STALE_MS. web v.3 raised it from 90 s to 5 minutes, because the Nano only reports
  // every 2 minutes at night.
  return v > (isV3Firmware() ? 300000 : 90000) ? "STALE" : "ok";
}

function renderRawSensors() {
  const nano = document.getElementById("rawNanoGrid");
  const r = liveData.diagnostics?.sensorsRaw;
  if (nano) {
    nano.innerHTML = "";
    if (!r) {
      nano.innerHTML = '<p class="muted">ESP1 has not published raw sensor values yet.</p>';
    } else {
      nano.appendChild(diagnosticGroup("Environment (raw)", [
        ["Temperature", numberText(r.env?.tempC, 1, "C")],
        ["Humidity", numberText(r.env?.humidity, 1, "%")],
        ["Reading age", `${formatAge(r.env?.ageMs)} (${ageFlag(r.env?.ageMs)})`]
      ]));
      nano.appendChild(diagnosticGroup("Light (raw)", [
        ["Lux", numberText(r.light?.lux, 0)],
        ["Reading age", `${formatAge(r.light?.ageMs)} (${ageFlag(r.light?.ageMs)})`]
      ]));
      nano.appendChild(diagnosticGroup("Tank (raw)", [
        ["Reservoir distance", numberText(r.tank?.reservoirCm, 1, "cm")],
        ["Mixing distance", numberText(r.tank?.mixingCm, 1, "cm")],
        ["Reservoir refill flow", numberText(r.tank?.flowLpm, 2, "L/min")],
        ["Sensor status", r.tank?.reservoirStatus || r.tank?.mixingStatus
          ? `reservoir ${TANK_STATUS_WORDS[r.tank?.reservoirStatus] || (r.tank?.reservoirStatus === "K" ? "OK" : "not reported")}, mixing tank ${TANK_STATUS_WORDS[r.tank?.mixingStatus] || (r.tank?.mixingStatus === "K" ? "OK" : "not reported")}`
          : "Not reported by this firmware"],
        ["Reading age", `${formatAge(r.tank?.ageMs)} (${ageFlag(r.tank?.ageMs)})`]
      ]));
      const soilRows = ["A", "B", "C"]
        .filter(id => Array.isArray(r.soil?.[id]))
        .map(id => [`Column ${id} probes`, `${r.soil[id][0]} / ${r.soil[id][1]}`]);
      soilRows.push(["Reading age", `${formatAge(r.soil?.ageMs)} (${ageFlag(r.soil?.ageMs)})`]);
      nano.appendChild(diagnosticGroup("Soil ADC (raw)", soilRows));
      // Modbus register order is fixed by the sensor: moisture, temp, EC, pH, N, P, K.
      const NPK_LABEL = ["Moisture", "Temp", "EC", "pH", "N", "P", "K"];
      ["A", "B", "C"].forEach(id => {
        const regs = r.npk?.[id]?.regs;
        if (!Array.isArray(regs)) return;
        const rows = regs.map((v, i) => [NPK_LABEL[i] || `reg${i}`, numberText(v, 2)]);
        rows.push(["Reading age", `${formatAge(r.npk[id].ageMs)} (${ageFlag(r.npk[id].ageMs)})`]);
        nano.appendChild(diagnosticGroup(`NPK column ${id} (raw registers)`, rows));
      });
    }
  }

  const sweeping = Boolean(r?.esp2?.sweepActive);
  const sweepLeft = Number(r?.esp2?.sweepSecondsLeft || 0);
  setDeviceStatus("rawEsp2Sweep", sweeping ? (sweepLeft ? `SWEEPING ${sweepLeft}s` : "SWEEPING") : "ESP2 IDLE",
                  sweeping ? "active" : "off");

  const box = document.getElementById("rawEsp2Grid");
  if (!box) return;
  const vals = r?.esp2?.values;
  box.innerHTML = "";
  if (!vals || !Object.keys(vals).length) {
    box.innerHTML = '<p class="muted">No ESP2 values yet. ESP2 is powered down between runs — run a sweep to read them.</p>';
    return;
  }
  const rows = Object.entries(vals).map(([id, v]) =>
    [id, `${numberText(v.raw, 2)} · ${v.valid ? "ok" : "BAD"} · ${formatAge(v.ageMs)}`]);
  box.appendChild(diagnosticGroup("ESP2 raw sensors", rows));
}

/* ---- Per-column firmware configuration --------------------------------------------------------- */
function hhmmToMinutes(value) {
  if (!value) return null;
  const [h, m] = String(value).split(":").map(Number);
  if (!Number.isFinite(h) || !Number.isFinite(m)) return null;
  return h * 60 + m;
}
function minutesToHhmm(mins) {
  if (!hasValue(mins)) return "--:--";
  const m = ((Number(mins) % 1440) + 1440) % 1440;
  return `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
}

// The same for the panels that send one command type. Without this they froze at "Sending to ESP1…",
// and the real outcome only appeared in Command status on another tab.
const PANEL_COMMAND_RESULTS = [
  ["FORCE_RUN", "forceRunResult"],
  ["SET_EXERCISE", "exResult"],
  ["RUN_PUMP_TEST", "mtExerciseResult"],
  ["DIAG_SWEEP", "mtSweepResult"],
  ["DIAG_SWEEP", "sweepResult"],
  ["SET_CLOCK", "sysClockResult"],
  ["SET_THRESH", "sysThreshResult"],
  ["SET_AC_VOLT", "sysAcVoltResult"],
  ["SET_AC_AMP", "sysAcAmpResult"],
  ["RESTORE_DEFAULTS", "sysRestoreResult"],
  ["CLEAR_BATT_SWITCH", "sysBattSwitchResult"],
  ["REBOOT", "rebootResult"],
  ["LCD_LOCK", "sysLockResult"],
  [["ENABLE_ACTUATIONS", "DISABLE_ACTUATIONS"], "actResult"]
];
function renderPanelCommandResults() {
  PANEL_COMMAND_RESULTS.forEach(([types, id]) => {
    const result = document.getElementById(id);
    const latest = [types].flat().map(latestCommandOfType).filter(Boolean)
      .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
    if (!result || !latest) return;
    result.textContent = `${rawText(latest.status, "pending")} — ${commandDetailText(latest, "waiting for ESP1")}`;
    result.className = `control-result${commandStatusTone(latest.status) === "error" ? " error" : ""}`;
  });
}

// Forced run. Payload keys and bounds are the firmware's, verified against firebaseCommandTick():
// columns / liters / doseMl{A,B,C}. Any non-zero dose makes ESP1 build a fertigation work order.
function submitForceRun(event) {
  event.preventDefault();
  const result = document.getElementById("forceRunResult");
  const show = (text, error = true) => {
    if (!result) return;
    result.textContent = text;
    result.className = `control-result${error ? " error" : ""}`;
  };

  const columns = document.getElementById("forceColumns")?.value || "";
  if (!["A", "B", "C", "AB"].includes(columns)) { show("Choose a destination column. Nothing was sent."); return; }
  if (forceColumnsDisabled(columns)) { show("That column is switched off on ESP1 (for example with \"Turn OFF column\" in the LCD's cancel menu). Switch it on in its column settings first. Nothing was sent."); return; }

  const liters = Number(document.getElementById("forceLiters")?.value);
  if (!Number.isFinite(liters) || liters <= 0 || liters > FORCE_MAX_LITERS) {
    show(`Water must be greater than 0 and at most ${FORCE_MAX_LITERS} L — ESP1 rejects anything outside that. Nothing was sent.`);
    return;
  }

  const doseMl = {};
  for (const key of ["A", "B", "C"]) {
    const value = Number(document.getElementById(`forceDose${key}`)?.value);
    if (!Number.isFinite(value) || value < 0 || value > FORCE_MAX_DOSE_ML) {
      show(`Nutrient ${key} must be between 0 and ${FORCE_MAX_DOSE_ML} mL. Nothing was sent.`);
      return;
    }
    doseMl[key] = value;
  }

  const delayRaw = document.getElementById("forceDelay")?.value;
  const delaySeconds = delayRaw === "" || delayRaw === undefined ? 30 : Number(delayRaw);
  if (!Number.isInteger(delaySeconds) || delaySeconds < 0 || delaySeconds > 300) {
    show("Start delay must be a whole number from 0 to 300 seconds. Nothing was sent."); return;
  }

  const fertigation = doseMl.A > 0 || doseMl.B > 0 || doseMl.C > 0;
  if (fertigation && liters < FORCE_MIN_DOSING_LITERS) {
    show(`A run with nutrients needs at least ${FORCE_MIN_DOSING_LITERS} L of water, or ESP1 refuses it as too small to dose safely. Nothing was sent.`);
    return;
  }
  // The countdown is the only chance to catch a mistyped amount once the request has been sent.
  if (delaySeconds === 0 && !confirm(`Start this run with no countdown? ${liters} L goes to ${columns}, and there will be no time to cancel it.`)) {
    show("Nothing was sent.", false);
    return;
  }
  show(`Sending ${fertigation ? "fertigation" : "irrigation"}: ${liters} L to ${columns}`
     + `${fertigation ? ` with A/B/C ${doseMl.A}/${doseMl.B}/${doseMl.C} mL` : ""}`
     + `, starting ${delaySeconds}s after ESP1 accepts it. Cancel appears below once it does.`, false);
  queueCommand("FORCE_RUN", { columns, liters, doseMl, delaySeconds }).then(key => {
    // queueCommand() already says why in the command status when it refuses to send.
    if (!key) show("Not sent. See the status message for the reason.");
  });
}

const loginForm = document.getElementById("loginForm");
loginForm?.addEventListener("submit", async event => {
  event.preventDefault();
  const errorBox = document.getElementById("loginError");
  if (!auth) {
    if (errorBox) errorBox.textContent = "Firebase is not configured.";
    return;
  }
  if (errorBox) errorBox.textContent = "";
  try {
    const credential = await auth.signInWithEmailAndPassword(document.getElementById("loginEmail").value.trim(), document.getElementById("loginPassword").value);
    loginForm.reset();
    if (credential.user.uid === ESP1_DEVICE_UID) return;   // onAuthStateChanged() refuses it and says why
    // Delete in User Management now removes the Auth account too (via the deleteUserAccount Cloud
    // Function), so this specific gap no longer applies to that path -- but a signed-in account with
    // no /users/{uid} record can still happen for other reasons (created outside this app's sign-up
    // form, or a record lost some other way). Recreate a fresh pending record here rather than leave
    // it stuck: the same shape and the same rule (!data.exists() && status:"pending") the sign-up
    // form itself relies on.
    const uid = credential.user.uid;
    const existing = await db.ref(`users/${uid}`).once("value");
    if (!existing.exists()) {
      db.ref(`users/${uid}`).set({
        name: credential.user.email,
        email: credential.user.email,
        status: "pending",
        createdAt: firebase.database.ServerValue.TIMESTAMP
      }).catch(error => console.warn("Could not re-create account record after sign-in", error));
    }
    logActivity("Signed in");
  } catch (error) {
    if (errorBox) errorBox.textContent = "Sign-in failed. Check your email and password.";
    console.error(error);
  }
});

document.getElementById("showSignupBtn")?.addEventListener("click", () => showAuthForm("signup"));
document.getElementById("showLoginBtn")?.addEventListener("click", () => showAuthForm("login"));
document.getElementById("showResetBtn")?.addEventListener("click", () => showAuthForm("reset"));
document.getElementById("showLoginFromResetBtn")?.addEventListener("click", () => showAuthForm("login"));

const resetForm = document.getElementById("resetForm");
resetForm?.addEventListener("submit", async event => {
  event.preventDefault();
  const errorBox = document.getElementById("resetError");
  const successBox = document.getElementById("resetSuccess");
  const show = text => { if (errorBox) errorBox.textContent = text; if (successBox) successBox.hidden = true; };
  if (!auth) { show("Firebase is not configured."); return; }
  const email = document.getElementById("resetEmail")?.value.trim() || "";
  show("");
  if (!email) { show("Enter your email address."); return; }
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) { show("Enter a valid email address."); return; }
  const succeed = () => {
    if (errorBox) errorBox.textContent = "";
    if (successBox) successBox.hidden = false;
    resetForm.reset();
  };
  try {
    await auth.sendPasswordResetEmail(email);
    succeed();
  } catch (error) {
    // Deliberately does NOT reveal whether the email is registered -- auth/user-not-found gets the
    // same success wording a real send would, matching a normal password-reset flow's standard
    // practice of never confirming or denying an account's existence to an unauthenticated caller.
    // Only genuinely non-identity-revealing problems (bad format, network) show as errors.
    if (error.code === "auth/user-not-found") { succeed(); return; }
    show(FIREBASE_AUTH_ERROR_TEXT[error.code] || `Could not send reset email: ${error.message}`);
    console.error(error);
  }
});

const FIREBASE_AUTH_ERROR_TEXT = {
  "auth/email-already-in-use": "That email is already registered. Try signing in instead.",
  "auth/invalid-email": "That doesn't look like a valid email address.",
  "auth/weak-password": "Password must be at least 6 characters.",
  "auth/network-request-failed": "Network error -- check your connection and try again."
};

const signupForm = document.getElementById("signupForm");
signupForm?.addEventListener("submit", async event => {
  event.preventDefault();
  const errorBox = document.getElementById("signupError");
  const show = text => { if (errorBox) errorBox.textContent = text; };
  if (!auth || !db) { show("Firebase is not configured."); return; }
  const name = document.getElementById("signupName")?.value.trim() || "";
  const email = document.getElementById("signupEmail")?.value.trim() || "";
  const password = document.getElementById("signupPassword")?.value || "";
  const confirm = document.getElementById("signupConfirm")?.value || "";
  show("");
  if (!name) { show("Enter your name."); return; }
  // The rules refuse a longer name. Checked first, because the login is created before the record, and a
  // refused record left the new account stuck on "Setting up your account".
  if (name.length > 80) { show("Name must be 80 characters or fewer."); return; }
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) { show("Enter a valid email address."); return; }
  if (password.length < 6) { show("Password must be at least 6 characters."); return; }
  if (password !== confirm) { show("Passwords do not match."); return; }
  try {
    const credential = await auth.createUserWithEmailAndPassword(email, password);
    // New account: forced to status "pending" both here and by the rules -- see /users/{uid} in
    // firebase-rules.json, which refuses any OTHER status on a self-created record. Approval is a
    // separate, operator-only write from this point on.
    await db.ref(`users/${credential.user.uid}`).set({
      name,
      email,
      status: "pending",
      createdAt: firebase.database.ServerValue.TIMESTAMP
    });
    signupForm.reset();
  } catch (error) {
    show(FIREBASE_AUTH_ERROR_TEXT[error.code] || `Could not create account: ${error.message}`);
    console.error(error);
  }
});

document.getElementById("logoutBtn")?.addEventListener("click", () => {
  if (auth) releaseHoldsThenSignOut().catch(error => setCommandStatus(`Sign out failed: ${error.message}`, "error"));
});
// A closed tab (pagehide -- actually going away: close/navigate/BFCache) hands the rig back at once.
// A merely BACKGROUNDED tab (visibilitychange) gets a short grace period instead of an instant release
// -- confirmed live that switching to check something for a few seconds and coming straight back was
// dropping an active hold every time, forcing a fresh request-and-wait cycle for no operational reason.
window.addEventListener("pagehide", () => { releaseManualHold(); mtHoldStop(true); });
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    if (mtHoldTimer && !mtHideTimer) {
      mtHideTimer = setTimeout(() => { mtHideTimer = null; if (document.hidden) releaseManualHold(); }, MT_HIDE_GRACE_MS);
    }
    // Unlike the 60 s manual-hold lease above (which tolerates a brief background), an ACTIVE
    // hold-to-run press has no "grace period" equivalent -- a hidden tab means whatever pointer/touch
    // was driving it is no longer reaching this page (e.g. switching browser tabs mid-press delivers
    // no mouseup here at all), so stop it the instant the tab goes out of view, not on a timer.
    mtHoldStop(true);
  } else if (mtHideTimer) {
    clearTimeout(mtHideTimer);
    mtHideTimer = null;
  } else if (!mtHoldTimer && document.getElementById("manualtest")?.classList.contains("active")) {
    // The grace period ran out while hidden and the hold went back to the rig. Still on this tab, so ask
    // again, or the page would sit on "Waiting for the rig to grant manual mode" with nothing asking.
    requestManualHold();
  }
});
document.getElementById("transferPumpBtn")?.addEventListener("click", () => sendFromPanel(resultLine("mtExerciseResult"), "RUN_PUMP_TEST", { pump: "transfer" }, { operatorOnly: true }));
document.getElementById("boosterPumpBtn")?.addEventListener("click", () => sendFromPanel(resultLine("mtExerciseResult"), "RUN_PUMP_TEST", { pump: "booster" }, { operatorOnly: true }));
document.getElementById("mixerBtn")?.addEventListener("click", () => sendFromPanel(resultLine("mtExerciseResult"), "RUN_PUMP_TEST", { pump: "mixer" }, { operatorOnly: true }));
bindAll(["emergencyStop", "emergencyStopTop"], () => queueCommand("EMERGENCY_STOP", {}, { emergency: true }));
document.getElementById("forceRunForm")?.addEventListener("submit", submitForceRun);

// Recovery controls all pass {emergency:true}. They must work when the snapshot is stale or the
// cooldown is armed -- being unable to recover the rig because the page thinks it is offline is the
// exact failure this whole feature exists to remove.
document.querySelectorAll("#faultRecovery button[data-recover]").forEach(btn =>
  btn.addEventListener("click", () => queueCommand("RECOVER", { action: btn.dataset.recover }, { emergency: true })));
document.getElementById("estopRecoverBtn")?.addEventListener("click", () => queueCommand("ESTOP_RECOVER", {}, { emergency: true }));

// One handler for every copy of a control, so the copies cannot drift.
function bindAll(ids, handler) {
  ids.forEach(id => document.getElementById(id)?.addEventListener("click", handler));
}
// Pumps and valves: the switch on Controls, and the banner's way back on during a lockout. Both send the same
// commands. (Round 2, 2026-11-05: the banner's copies of Disable and the three restarts were dropped. They aren't
// ways out of what the banner reports, and Controls and the always-visible Emergency stop cover them.)
document.getElementById("actSwitch")?.addEventListener("click", toggleActuations);
document.getElementById("enableActBtn")?.addEventListener("click", () => queueCommand("ENABLE_ACTUATIONS", {}, { emergency: true }));
// Restarts are not freshness-gated (emergency: true): a board that stopped answering is why they're used.
document.getElementById("rebootNanoBtn2")?.addEventListener("click", () => sendFromPanel(resultLine("rebootResult"), "REBOOT", { target: "nano" }, { emergency: true }));
document.getElementById("rebootEsp2Btn2")?.addEventListener("click", () => sendFromPanel(resultLine("rebootResult"), "REBOOT", { target: "esp2" }, { emergency: true }));
document.getElementById("rebootEsp1Btn2")?.addEventListener("click", () => {
  // ESP1 owns the Firebase link, so this one goes quiet for ~15 s before it comes back.
  if (!confirm("Restart ESP1? This website loses contact for about 15 seconds while it restarts.")) return;
  sendFromPanel(resultLine("rebootResult"), "REBOOT", { target: "esp1" }, { emergency: true });
});
["lockSwitch", "sysLockSwitch", "unlockBtn"].forEach(id => document.getElementById(id)?.addEventListener("click", toggleRigLock));
document.getElementById("ackFaultBtn")?.addEventListener("click", () => {
  faultAckLocalUntil = Date.now() + 120000;
  queueCommand("ACK_FAULT", {}, { emergency: true });
  updateFaultBanner();
});
document.getElementById("cancelForceBtn")?.addEventListener("click", () => queueCommand("CANCEL_FORCE", {}, { emergency: true }));

/* ---- System tab: Set Clock, Thresholds, Restore Defaults, Lock Screen status ------------------
 * Remote equivalents of the ESP1 LCD Settings-menu items that don't already live elsewhere on this
 * site. Each one is a thin wrapper around queueCommand() -- same approval gate, same validated
 * command path as everything else -- not a second, competing implementation. */
document.getElementById("sysClockSendBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysClockResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  const dateVal = document.getElementById("sysClockDate")?.value;
  const timeVal = document.getElementById("sysClockTime")?.value;
  if (!dateVal || !timeVal) { show("Pick both a date and a time first. Nothing was sent."); return; }
  const [y, mo, d] = dateVal.split("-").map(Number);
  const [h, mi] = timeVal.split(":").map(Number);
  if (![y, mo, d, h, mi].every(Number.isFinite)) { show("Date/time could not be read. Nothing was sent."); return; }
  // ESP1's RTC_YEAR_MIN/MAX. Outside them it only answers "clock fields out of range", which doesn't say why.
  if (y < RTC_YEAR_MIN || y > RTC_YEAR_MAX) { show(`ESP1 only accepts a year from ${RTC_YEAR_MIN} to ${RTC_YEAR_MAX}. Nothing was sent.`); return; }
  if (!confirm(`Set ESP1's clock to ${dateVal} ${timeVal}? This affects every column's irrigation schedule.`)) return;
  sendFromPanel(show, "SET_CLOCK", { clkY: y, clkMo: mo, clkD: d, clkH: h, clkMi: mi }, { operatorOnly: true });
});

// Fills the date and time with the current time, from Firebase's clock rather than this device's (a PC clock 18 hours
// out has happened on this project), shown in this device's time zone, which is the rig's.
document.getElementById("sysClockNowBtn")?.addEventListener("click", () => {
  const now = new Date(serverNow());
  const pad = value => String(value).padStart(2, "0");
  const date = document.getElementById("sysClockDate");
  const time = document.getElementById("sysClockTime");
  if (date) date.value = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
  if (time) time.value = `${pad(now.getHours())}:${pad(now.getMinutes())}`;
  resultLine("sysClockResult")("Filled in the time now. Check it, then press Set ESP1's clock.");
});

document.getElementById("sysThreshSendBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysThreshResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  // A blank field must never go out as 0: start 0 means no column is ever dry enough, which silently
  // stops all scheduled watering, and gap 0 fertigates on any shortfall. ESP1 doesn't publish its
  // current values, so these fields always start blank. thStart must be whole: ESP1 reads a decimal as
  // absent. Only older firmware needs a stop value, and rejects the request without one.
  const needsStop = !isV3Firmware();
  const start = numberFromInput("sysThreshStart");
  const stop  = needsStop ? numberFromInput("sysThreshStop") : null;
  const gap   = numberFromInput("sysThreshGap");
  if (![start, gap, ...(needsStop ? [stop] : [])].every(Number.isFinite)) { show("Fill in every field with a number. Nothing was sent."); return; }
  if (!Number.isInteger(start) || start < 0 || start > 100) { show("Soil start must be a whole number from 0 to 100. Nothing was sent."); return; }
  if (needsStop && (!Number.isInteger(stop) || stop < 0 || stop > 100)) { show("Soil stop must be a whole number from 0 to 100. Nothing was sent."); return; }
  if (gap < 0 || gap > 500) { show("Gap must be 0-500. Nothing was sent."); return; }
  if (start === 0 && !confirm("Soil start 0% means no column will ever be dry enough, so scheduled watering stops. Send it anyway?")) { show("Nothing was sent.", false); return; }
  const payload = { thStart: start, thGap: gap };
  if (needsStop) payload.thStop = stop;
  sendFromPanel(show, "SET_THRESH", payload, { operatorOnly: true });
});

// AC voltage limits (2026-11-14): the range ESP2's PZEM check stops a pump on the inverter outside of. The same
// checks as the LCD's Settings > AC Voltage and ESP1's acVoltWindowOk(); firebase-rules.json repeats the range
// and the gap. Whole volts only: ESP1 reads a decimal as absent.
const AC_VOLT_LIMIT_LOW = 80, AC_VOLT_LIMIT_HIGH = 260, AC_VOLT_MIN_GAP = 5;
// Outside this, ask once more: the pumps would keep running on a supply that far from a normal 220-230 V.
const AC_VOLT_USUAL_LOW = 200, AC_VOLT_USUAL_HIGH = 250;
document.getElementById("sysAcVoltSendBtn")?.addEventListener("click", () => {
  const show = resultLine("sysAcVoltResult");
  const low = numberFromInput("sysAcVoltMin");
  const high = numberFromInput("sysAcVoltMax");
  if (!Number.isFinite(low) || !Number.isFinite(high)) { show("Fill in both limits. Nothing was sent.", true); return; }
  if (!Number.isInteger(low) || !Number.isInteger(high)) { show("Use whole volts. Nothing was sent.", true); return; }
  if (low < AC_VOLT_LIMIT_LOW || high > AC_VOLT_LIMIT_HIGH) {
    show(`Both limits must be from ${AC_VOLT_LIMIT_LOW} to ${AC_VOLT_LIMIT_HIGH} V. Nothing was sent.`, true); return;
  }
  if (high - low < AC_VOLT_MIN_GAP) {
    show(`The low limit must be at least ${AC_VOLT_MIN_GAP} V under the high limit. Nothing was sent.`, true); return;
  }
  const system = liveData.system || {};
  const now = hasValue(system.acVoltMin) && hasValue(system.acVoltMax) ? ` They are ${system.acVoltMin}–${system.acVoltMax} V now.` : "";
  const wide = low < AC_VOLT_USUAL_LOW || high > AC_VOLT_USUAL_HIGH
    ? `\n\nThat is a wide range: pumps will keep running on a supply as low as ${low} V or as high as ${high} V, which can damage them.`
    : "";
  if (!confirm(`Set the AC voltage limits to ${low}–${high} V?${now} A pump on the inverter stops when the voltage leaves this range.${wide}`)) {
    show("Nothing was sent.");
    return;
  }
  sendFromPanel(show, "SET_AC_VOLT", { vMin: low, vMax: high }, { operatorOnly: true });
});

// AC current limits (2026-11-16): the no-current (min) and overcurrent (max) limits ESP2's PZEM check stops a pump on
// the inverter outside of. The same checks as the LCD's Settings > AC Current and ESP1's acAmpWindowOk(), in the same
// unit ESP1 keeps (tenths of an amp); firebase-rules.json repeats the range and the gap.
const AC_AMP_LIMIT_LOW10 = 1, AC_AMP_LIMIT_HIGH10 = 100, AC_AMP_MIN_GAP10 = 2;
document.getElementById("sysAcAmpSendBtn")?.addEventListener("click", () => {
  const show = resultLine("sysAcAmpResult");
  const low = numberFromInput("sysAcAmpMin");
  const high = numberFromInput("sysAcAmpMax");
  if (!Number.isFinite(low) || !Number.isFinite(high)) { show("Fill in both limits. Nothing was sent.", true); return; }
  const low10 = Math.round(low * 10), high10 = Math.round(high * 10);
  // The LCD steps by 0.1 A and ESP1 keeps tenths: anything finer would be rounded here, so it is refused instead.
  if (Math.abs(low * 10 - low10) > 1e-6 || Math.abs(high * 10 - high10) > 1e-6) {
    show("Use steps of 0.1 A, for example 0.5 or 3.0. Nothing was sent.", true); return;
  }
  if (low10 < AC_AMP_LIMIT_LOW10 || high10 > AC_AMP_LIMIT_HIGH10) {
    show(`Both limits must be from ${ampsFromTenthsText(AC_AMP_LIMIT_LOW10)} to ${ampsFromTenthsText(AC_AMP_LIMIT_HIGH10)} A. Nothing was sent.`, true); return;
  }
  if (high10 - low10 < AC_AMP_MIN_GAP10) {
    show(`The no-current limit must be at least ${ampsFromTenthsText(AC_AMP_MIN_GAP10)} A under the overcurrent limit. Nothing was sent.`, true); return;
  }
  const system = liveData.system || {};
  const now = hasValue(system.acAmpMin10) && hasValue(system.acAmpMax10)
    ? ` They are ${ampsFromTenthsText(system.acAmpMin10)} A and ${ampsFromTenthsText(system.acAmpMax10)} A now.` : "";
  if (!confirm(`Set the AC current limits to no-current ${ampsFromTenthsText(low10)} A and overcurrent ${ampsFromTenthsText(high10)} A?${now} A pump on the inverter stops at once above the overcurrent limit, and after 30 seconds below the no-current limit.`)) {
    show("Nothing was sent.");
    return;
  }
  sendFromPanel(show, "SET_AC_AMP", { aMin10: low10, aMax10: high10 }, { operatorOnly: true });
});

// Dormant while the firmware builds with SECONDARY_BATT_ENABLED 0: the button only enables when ESP1
// reports the fault as latched. Same effect as the LCD row and SMS BATTSW,CLEAR.
document.getElementById("sysClearBattSwitchBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysBattSwitchResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  if (!confirm("Clear the battery-switch fault? Check the hold-up first: this fault means a battery switch reset the controllers, and clearing it allows the next inverter use to switch again.")) return;
  sendFromPanel(show, "CLEAR_BATT_SWITCH", {}, { operatorOnly: true });
});

document.getElementById("sysRestoreDefaultsBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysRestoreResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  if (!confirm("Restore Defaults? This resets every column's mode, targets, name, and schedule window, " +
               "plus the global thresholds, to factory values. This CANNOT be undone. Calibration, " +
               "column-enabled wiring, and WiFi/ThingSpeak setup are kept.")) return;
  sendFromPanel(show, "RESTORE_DEFAULTS", {}, { operatorOnly: true });
});

// ESP1's own time and clock health, for Set Clock. The rig lock panel is drawn by renderRigLock().
function updateSystemTab() {
  setText("sysClockCurrent", deviceTimeText(liveData.system?.deviceTime));
  setText("sysClockRtcOk", booleanText(liveData.system?.rtcOk, "OK", "Not OK"));
  const system = liveData.system || {};
  setText("sysBattSwitching", v3Only(booleanText(system.secondaryBattSwitching, "On", "Off in this firmware")));
  setText("sysBattSwitchFault", v3Only(booleanText(system.battSwitchFault, "Latched: all inverter use is blocked", "None")));
  setText("sysBattSwitchStrikes", v3Only(rawText(system.battSwitchStrikes)));
  const stopLabel = document.getElementById("sysThreshStopLabel");
  if (stopLabel) stopLabel.hidden = isV3Firmware();
  updateAcVoltPanel(system);
  updateAcAmpPanel(system);
}

// AC voltage limits. ESP1 publishes the window (acVoltMin/acVoltMax) and, once a pump has run since it started,
// the last AC voltage measured with its age at that snapshot (acVoltLast/acVoltLastAgeMs).
function updateAcVoltPanel(system) {
  const known = hasValue(system.acVoltMin) && hasValue(system.acVoltMax);
  setText("sysAcVoltCurrent", known ? `${system.acVoltMin}–${system.acVoltMax} V`
    : liveData.system ? "Not reported by this firmware: update ESP1 and ESP2" : "Unavailable");
  // The age was taken when ESP1 built the snapshot; add how old the snapshot is, in Firebase's time.
  const snapshotAge = hasValue(liveData.meta?.updatedAt) ? Math.max(0, serverNow() - Number(liveData.meta.updatedAt)) : 0;
  setText("sysAcVoltLast", hasValue(system.acVoltLast)
    ? `${numberText(system.acVoltLast, 1, "V")}, ${agoText(Number(system.acVoltLastAgeMs) + snapshotAge)}`
    : known ? "None yet: it comes from the next pump run" : "Unavailable");
  // Start the fields at the current values, without ever overwriting what someone is typing.
  const minInput = document.getElementById("sysAcVoltMin");
  const maxInput = document.getElementById("sysAcVoltMax");
  const editing = [minInput, maxInput].includes(document.activeElement);
  if (known && minInput && maxInput && !editing && !minInput.value && !maxInput.value) {
    minInput.value = system.acVoltMin;
    maxInput.value = system.acVoltMax;
  }
}

// AC current limits (2026-11-16). ESP1 publishes them in tenths of an amp (acAmpMin10/acAmpMax10) and, once a pump has
// run since it started, the last pump current measured, in amps, with its age at that snapshot (acAmpLast/AgeMs).
function updateAcAmpPanel(system) {
  const known = hasValue(system.acAmpMin10) && hasValue(system.acAmpMax10);
  setText("sysAcAmpCurrent", known
    ? `No-current ${ampsFromTenthsText(system.acAmpMin10)} A, overcurrent ${ampsFromTenthsText(system.acAmpMax10)} A`
    : liveData.system ? "Not reported by this firmware: update ESP1 (16 Nov 2026)" : "Unavailable");
  const snapshotAge = hasValue(liveData.meta?.updatedAt) ? Math.max(0, serverNow() - Number(liveData.meta.updatedAt)) : 0;
  setText("sysAcAmpLast", hasValue(system.acAmpLast)
    ? `${numberText(system.acAmpLast, 2, "A")}, ${agoText(Number(system.acAmpLastAgeMs) + snapshotAge)}`
    : known ? "None yet: it comes from the next pump run" : "Unavailable");
  // Start the fields at the current values, without ever overwriting what someone is typing.
  const minInput = document.getElementById("sysAcAmpMin");
  const maxInput = document.getElementById("sysAcAmpMax");
  const editing = [minInput, maxInput].includes(document.activeElement);
  if (known && minInput && maxInput && !editing && !minInput.value && !maxInput.value) {
    minInput.value = ampsFromTenthsText(system.acAmpMin10);
    maxInput.value = ampsFromTenthsText(system.acAmpMax10);
  }
}

/* ---- Manual/Test tab -------------------------------------------------------------------------
 * Everything here energises a relay immediately, so the tab opens behind a gate that re-arms every
 * time you leave it. Emergency stop is deliberately NOT here -- it lives on Controls, because a stop
 * must never sit behind a warning that has to be dismissed first. */
let mtArmed = false;

/* Manual-mode hold. Opening Manual/Test takes the rig out of automatic so a scheduled run cannot
 * start under the operator's hands. Written to a SINGLE node with set() -- never the command queue,
 * which a 20 s keep-alive would grow without bound and fill with noise.
 *
 * ESP1 owns the deadline and only refreshes its lease when `seq` CHANGES, so a tab left open on a
 * dead machine stops refreshing and the rig frees itself. Releasing on pagehide/tab-switch just makes
 * that happen sooner than the 60 s lease. A merely backgrounded tab (visibilitychange) gets a short
 * grace period instead -- see MT_HIDE_GRACE_MS below -- rather than dropping the hold on every glance
 * away. */
let mtHoldTimer = null;
let mtHideTimer = null;
let lastManualSeq = 0;          // the last seq this page wrote to irrigation/manual (see writeManualHold())
const MT_HIDE_GRACE_MS = 15000; // < the 20 s keep-alive, so a genuinely abandoned tab still gives up the hold promptly

function writeManualHold(want) {
  // The one other write path toward hardware outside queueCommand() -- same approval gate, both
  // client-side (here) and server-side (the rules on /irrigation/manual).
  // Deep-scan finding (2026-09-10, HIGH): this used to gate on isApprovedUser() UNCONDITIONALLY,
  // directly contradicting the very comment below it -- an operator restricting/blocking this account
  // while it held Manual/Test silently dropped its OWN release (want=false) write too, leaving the
  // relay held until ESP1's own 60s lease timed it out instead of handing control back immediately.
  // Only signed-in is required to release; approval is checked below, only for want=true.
  if (!currentUserIsSignedIn()) return Promise.resolve(false);
  // Access-control revision (2026-09-09): Manual/Test is privileged-operator-only (or temporarily
  // granted, see canAccessPrivilegedTabs()), so only that tier may ACQUIRE the hold (want=true) --
  // the manualtest tab that normally triggers this is already hidden otherwise, this is the
  // function-level backstop per the access-control spec's "a JS function invoked manually must
  // still be rejected" requirement. Releasing (want=false) deliberately stays available regardless
  // of role: it only ever hands control back to automation, which must never be blocked by a
  // permission check.
  if (want && (!isApprovedUser() || !canAccessPrivilegedTabs())) return Promise.resolve(false);
  // The rules refuse asking for the hold while the rig is locked; releasing it is always allowed.
  if (want && rigLocked()) return Promise.resolve(false);
  // C-H4/R workaround (audit): the CURRENTLY-FLASHED ESP1 firmware parses this into a 32-bit `long`
  // (`long seq = doc["seq"] | -1`). A raw Date.now() (~1.7e12) doesn't fit a 32-bit long, so
  // ArduinoJson's `|` silently falls back to -1 on EVERY poll -- seq != webManualSeq then never
  // becomes newly true, and Manual/Test's "Proceed" gate can never re-arm on that hardware. Sending
  // whole seconds instead keeps a monotonically increasing value that fits a 32-bit long until 2038,
  // fixing this without a reflash. 1-second resolution is coarser than the 20 s keep-alive interval
  // this is used with, so it cannot collide in normal use. The unflashed dev-tree firmware already
  // reads this as int64_t and accepts either form, so this is compatible with both trees.
  // ESP1 only re-decides when seq CHANGES, so it must never repeat: a release and a fresh request inside
  // the same second (leaving the tab and coming straight back) used to send the same number, and the new
  // request then went unanswered until the next keep-alive, 20 s later.
  const seq = Math.max(Math.floor(Date.now() / 1000), lastManualSeq + 1);
  lastManualSeq = seq;
  return db.ref("irrigation/manual").set({ seq, want })
    .catch(error => { setCommandStatus(`Could not reach the rig: ${error.message}`, "error"); return false; });
}

/* Hold-to-run (2026-09-10): mirrors writeManualHold()'s own gate/seq pattern, but writes
 * irrigation/testHold = {seq, bit, want} -- ESP1's firebasePollTestHold() arms/holds/releases the
 * SAME ESP2 TEST,HOLD/TEST,RELEASE dead-man protocol the LCD Testing screen and the Timed-pulses
 * feature above already use, just driven by a held button instead of a physical one or a timer. */
function writeTestHold(want, bit) {
  // Only signed-in is required to release, exactly like writeManualHold() and the rule on this node:
  // gating want:false on approval dropped the release for an account restricted mid-press.
  if (!currentUserIsSignedIn()) return Promise.resolve(false);
  // Same acquire-only gate as writeManualHold(): only privileged-tier accounts may START a hold;
  // releasing always goes through regardless of tier, so a lost grant (e.g. an expiring temp-access
  // window) can never leave a relay stuck energised because the release write itself got refused.
  if (want && (!isApprovedUser() || !canAccessPrivilegedTabs() || rigLocked())) return Promise.resolve(false);
  const seq = Date.now();
  return db.ref("irrigation/testHold").set({ seq, bit, want })
    .catch(error => { setCommandStatus(`Could not reach the rig: ${error.message}`, "error"); return false; });
}

let mtHoldActiveBtn = null;   // the button currently mid-hold, or null
let mtHoldKeepAliveTimer = null;
let mtHoldCapTimer = null;
let mtHoldLastStopAt = 0;     // when this page last stopped a hold, for MT_HOLD_RESTART_GAP_MS
let mtHoldArmedTarget = null; // a corrosive (pH) target the operator just confirmed, awaiting the hold press
let mtHoldArmedTimer = null;

// Mirrors the firmware's TEST_HOLD_MAX_MS. ESP1 stops the relay at this limit and then ignores every
// refresh until it sees want:false, so this page must stop too or it would go on showing a hold that
// has already ended. Counted from the press, which is earlier than ESP1 starts counting, so this one
// always fires first and its want:false clears ESP1's latch.
const MT_HOLD_MAX_MS = 15000;
// ESP1 only ever sees the newest testHold write, polling every 1-3 s. Releasing and pressing another
// button inside that gap hides the release from it: the first relay keeps running on the new press's
// keep-alives while this page shows the new target. Comfortably clears that poll (and matches ESP1's
// own 4 s browser timeout).
const MT_HOLD_RESTART_GAP_MS = 4000;

function mtHoldStop(clearedByRelease, stoppedAtCap = false) {
  const btn = mtHoldActiveBtn;
  mtHoldActiveBtn = null;
  if (mtHoldKeepAliveTimer) { clearInterval(mtHoldKeepAliveTimer); mtHoldKeepAliveTimer = null; }
  if (mtHoldCapTimer) { clearTimeout(mtHoldCapTimer); mtHoldCapTimer = null; }
  if (!btn) return Promise.resolve(false);
  btn.classList.remove("holding");
  mtHoldLastStopAt = Date.now();
  // This stop writes its own want:false below, so the browser-died fallback from mtHoldStart() is done.
  db?.ref("irrigation/testHold").onDisconnect().cancel()
    .catch(error => console.warn("Could not cancel the hold's disconnect release", error));
  const target = btn.dataset.holdTarget;
  const released = clearedByRelease ? writeTestHold(false, target) : Promise.resolve(false);
  const startedMs = Number(btn.dataset.holdStartMs || 0);
  if (startedMs) reportHoldResult(target, Date.now() - startedMs, stoppedAtCap);
  delete btn.dataset.holdStartMs;
  return released;
}

function mtHoldStart(btn) {
  const target = btn.dataset.holdTarget;
  // If this browser dies mid-press (crash, power, lost network), Firebase itself writes the release.
  // Otherwise want:true stays in the database, and ESP1 would act on it the next time it boots.
  db.ref("irrigation/testHold").onDisconnect()
    .set({ seq: firebase.database.ServerValue.TIMESTAMP, bit: target, want: false })
    .catch(error => console.warn("Could not register the hold's disconnect release", error));
  writeTestHold(true, target);
  logActivity(`Hold-to-run test: ${btn.textContent.trim() || target}`);
  mtHoldActiveBtn = btn;
  btn.classList.add("holding");
  btn.dataset.holdStartMs = String(Date.now());
  mtHoldCapTimer = setTimeout(() => { if (mtHoldActiveBtn === btn) mtHoldStop(true, true); }, MT_HOLD_MAX_MS);
  // A confirmed pH arm is consumed by the press it was meant for -- releasing and pressing again
  // needs a fresh confirm(), same one-shot spirit as the Timed-pulses confirm above.
  if (mtHoldArmedTarget === target) {
    mtHoldArmedTarget = null;
    if (mtHoldArmedTimer) { clearTimeout(mtHoldArmedTimer); mtHoldArmedTimer = null; }
    btn.textContent = btn.dataset.holdLabel || btn.textContent;
  }
  // Refresh well inside ESP1's own TEST_HOLD_BROWSER_TIMEOUT_MS (4 s). ESP1 polls every 1 s during a
  // hold, so this tolerates one slow poll or write round-trip.
  mtHoldKeepAliveTimer = setInterval(() => writeTestHold(true, target), 1200);
}

document.querySelectorAll(".hold-btn").forEach(btn => {
  const isCorrosive = btn.classList.contains("danger");
  // Did this mouse press start a hold? Releasing it then fires a click, which must not be taken for a new
  // request to dispense. A touch press fires no click, because its preventDefault() below cancels it.
  let mousePressBeganHold = false;
  const start = ev => {
    if (ev.type === "mousedown") mousePressBeganHold = false;
    const target = btn.dataset.holdTarget;
    // Needs the confirm click first, below. Checked before preventDefault(): on a touchscreen that would
    // also cancel the click, so a pH button could never be armed from a phone.
    if (isCorrosive && mtHoldArmedTarget !== target) return;
    ev.preventDefault();
    if (btn.disabled || mtHoldActiveBtn) return;
    const wait = MT_HOLD_RESTART_GAP_MS - (Date.now() - mtHoldLastStopAt);
    if (wait > 0) {
      const note = document.getElementById("mtHoldResult");
      if (note) { note.textContent = `Wait ${Math.ceil(wait / 1000)} s before the next press, so ESP1 has seen the last release.`; note.className = "control-result error"; }
      return;
    }
    mtHoldStart(btn);
    if (ev.type === "mousedown") mousePressBeganHold = true;
  };
  const stop = () => { if (mtHoldActiveBtn === btn) mtHoldStop(true); };
  btn.addEventListener("mousedown", start);
  btn.addEventListener("touchstart", start, { passive: false });
  btn.addEventListener("mouseup", stop);
  btn.addEventListener("mouseleave", stop);   // dragging off the button must stop it, not leave it stuck
  btn.addEventListener("touchend", stop);
  btn.addEventListener("touchcancel", stop);
  window.addEventListener("blur", stop);      // alt-tabbing away with the mouse still down must stop it
  // Corrosive targets need an explicit confirm before a hold does anything -- a native confirm()
  // blocks JS, which is incompatible with mousedown/mouseup timing (the physical press would be long
  // over by the time the dialog closes), so confirming ARMS the button for a few seconds instead of
  // immediately starting the pump. A plain click (press+release without ever arming) does nothing.
  if (isCorrosive) {
    btn.addEventListener("click", () => {
      if (mousePressBeganHold) { mousePressBeganHold = false; return; }
      if (btn.disabled || mtHoldActiveBtn) return;
      const target = btn.dataset.holdTarget;
      if (mtHoldArmedTarget === target) return;   // already armed from a previous click
      if (!confirm(`Hold this button to dispense pH adjuster (${target === "phUp" ? "UP" : "DOWN"})? This is corrosive and goes into the mixing tank. It stops about 1-4 seconds after you release it.`)) return;
      mtHoldArmedTarget = target;
      btn.textContent = `${btn.dataset.holdLabel || btn.textContent} — press & hold now`;
      if (mtHoldArmedTimer) clearTimeout(mtHoldArmedTimer);
      mtHoldArmedTimer = setTimeout(() => { mtHoldArmedTarget = null; btn.textContent = btn.dataset.holdLabel || btn.textContent; }, 8000);
    });
    btn.dataset.holdLabel = btn.textContent;
  }
});

// The pulse verdict comes back as the command's own status detail (see renderPulseResult() above);
// hold-to-run has no command entry to read back (testHold isn't a queued command, just a live node).
// So this can only report how long the button was held, not that a relay ran: ESP1 may have refused the
// hold silently, and it needs time to power ESP2 up first. Its own TESTHOLD| log lines reach Recent ESP1
// events, and ESP2's TEST,FLOW reply surfaces through the LCD/logs for the metered volume.
function reportHoldResult(target, ms, stoppedAtCap) {
  const el = document.getElementById("mtHoldResult");
  if (!el) return;
  el.textContent = stoppedAtCap
    ? `Stopped "${target}" at the ${MT_HOLD_MAX_MS / 1000} s limit. Release the button and wait a few seconds before pressing again.`
    : `Held "${target}" for about ${(ms / 1000).toFixed(1)} s. ESP1 doesn't confirm holds on this page: check Recent ESP1 events on the Diagnostics tab for TESTHOLD entries.`;
  el.className = "control-result";
}

function requestManualHold() {
  if (rigLocked()) return;                    // the rules refuse it; renderManualHold() says why
  if (!mtHoldTimer) logActivity("Opened Manual/Test (the rig leaves automatic)");
  const answer = liveData.diagnostics?.webManual?.state;
  mtStaleAnswer = answer === "refused" || answer === "revoked" ? answer : null;
  mtAskedAt = serverNow();
  writeManualHold(true);
  if (mtHoldTimer) clearInterval(mtHoldTimer);
  // 20 s against a 60 s lease: two keep-alives may be lost before the rig takes the hold back.
  mtHoldTimer = setInterval(() => writeManualHold(true), 20000);
}

function releaseManualHold() {
  if (mtHideTimer) { clearTimeout(mtHideTimer); mtHideTimer = null; }
  // Only a session that asked for the hold gives it back. irrigation/manual is one node shared by every
  // browser, and this runs on every tab change and page close: writing want:false regardless let any
  // other signed-in user end an operator's hold just by browsing, putting the rig back in automatic
  // while they were testing, until their next 20 s keep-alive. A session that dies without releasing is
  // covered by ESP1's own 60 s lease.
  if (!mtHoldTimer) return Promise.resolve(false);
  clearInterval(mtHoldTimer);
  mtHoldTimer = null;
  return writeManualHold(false);
}

// ESP1 leaves "refused" and "revoked" published after the page stops asking, until it next grants a hold.
// So they aren't an answer to this page unless they arrive after it asked, and the same word as before
// isn't one until ESP1 has had time to read the request: it polls every 3 s, and an upload may already be
// under way. It re-decides on each 20 s keep-alive, so a real refusal still comes through soon after.
let mtAskedAt = 0;            // Firebase's time when this page last started asking
let mtStaleAnswer = null;     // the "refused" or "revoked" already published when it did
const MT_HOLD_ANSWER_GRACE_MS = 6000;

function manualHoldState() {
  const state = String(liveData.diagnostics?.webManual?.state || "idle");
  if (state !== "refused" && state !== "revoked") return state;
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const leftOver = stamp < mtAskedAt || (state === mtStaleAnswer && stamp < mtAskedAt + MT_HOLD_ANSWER_GRACE_MS);
  return leftOver ? "idle" : state;
}

// Reflect what ESP1 decided. The controls stay hidden until it actually says "held" -- the gate's
// Proceed button reveals them, but only once the rig has agreed to hand over control.
function renderManualHold() {
  const wm = liveData.diagnostics?.webManual || {};
  const state = manualHoldState();
  const note = document.getElementById("mtHoldNote");
  const gate = document.getElementById("mtGate");
  const proceed = document.getElementById("mtProceed");
  const onTab = document.querySelector('#manualtest')?.classList.contains("active");

  if (note) {
    if (!onTab) note.textContent = "";
    // Checked ahead of every other branch, including freshness/"held" -- a non-approved account's
    // request is silently never granted (writeManualHold() no-ops), so without this it was
    // indistinguishable from "the page is broken" or "ESP1 is offline."
    else if (!isApprovedUser()) note.textContent = "Your account is awaiting operator approval before it can use this control.";
    else if (rigLocked()) note.textContent = "The rig is locked, so nothing on this tab can be used. Unlock it first: operators, and accounts with 1-hour access or Sub-operator, can.";
    // Checked ahead of every state branch, including "held" -- a stale snapshot can't be trusted to
    // mean the rig is still there, and telling the operator "Manual mode active" from stale data would
    // be actively misleading.
    else if (!deviceIsFresh()) note.textContent = `ESP1 appears offline — ${snapshotAgeText()}. Your request is queued but cannot be granted until it reconnects.`;
    else if (state === "held")    note.textContent = `Manual mode active — the rig is out of automatic${wm.secondsLeft ? ` (renews, ${wm.secondsLeft}s left)` : ""}.`;
    else if (state === "refused") note.textContent = `The rig refused manual mode: ${rawText(wm.reason, "not idle")}. Wait for it to finish, then press Proceed to ask again.`;
    else if (state === "revoked") note.textContent = "The rig operator took control at the LCD.";
    else note.textContent = "Requesting manual control from the rig…";
    // Asking is the normal first step, not an error.
    const refusedOrUnavailable = !isApprovedUser() || rigLocked() || !deviceIsFresh() || state === "refused" || state === "revoked";
    note.className = refusedOrUnavailable ? "control-result error" : state === "held" ? "field-note" : "control-result";
  }
  // Proceed reveals the screen regardless of hold state (2026-09-10 revision) -- previously this
  // stayed disabled until ESP1 actually granted the hold, which meant a stale/offline snapshot
  // (state stuck at "idle" since ESP1 never gets the chance to grant anything) left the operator
  // permanently unable to even LOOK at the tab. Individual test buttons are the real gate now, via
  // syncControlAvailability()'s normalAllowed/deviceIsFresh() check -- they alone stay disabled
  // until the rig is genuinely connected and has granted the hold.
  // Concern raised (2026-09-10): could a non-privileged approved account reach Manual/Test at all?
  // Traced end to end -- this was the one real gap. Proceed used to disable only on !isApprovedUser(),
  // so a plain approved (non-operator, no temp/sub-operator grant) account's Proceed button was never
  // actually disabled, even though the nav tab itself stays correctly hidden for them. That let a
  // console `document.getElementById('mtProceed').click()` reveal #mtControls regardless of tier --
  // cosmetic only (every individual button/command underneath is still independently gated by
  // canAccessPrivilegedTabs()/queueCommand()'s operatorOnly check, both client and server-side), but
  // still a real defense-in-depth miss for the one thing Proceed itself controls: revealing the panel
  // at all. Now requires the same privileged tier as the tab itself (an operator, an active 1h temp grant, or a
  // persistent Sub-operator grant).
  if (proceed) {
    const privileged = canAccessPrivilegedTabs();
    proceed.disabled = !privileged || rigLocked();
    proceed.title = !privileged ? (!isApprovedUser()
      ? "Your account is awaiting operator approval before it can use this control."
      : "This is for operators, and accounts with 1-hour access or Sub-operator.")
      : rigLocked() ? LOCKED_TITLE : "";
  }
  // Locked while in here (by anyone, here or at the rig): the rules refuse the keep-alives, so hand the rig back to
  // automatic now rather than when the lease runs out, and go back to the gate, which says why.
  if (rigLocked() && (mtArmed || mtHoldTimer)) {
    if (mtArmed) {
      setManualTestArmed(false);
      if (onTab) setCommandStatus("The rig was locked, so Manual/Test closed and the rig went back to automatic.", "error");
    }
    releaseManualHold();
  }
  // Revoked or refused while already inside: drop the controls and stop re-requesting. Deliberately
  // NOT "state !== held" any more (2026-09-10) -- Proceed can now reveal #mtControls with state still
  // "idle"/"requesting" (stale snapshot, or ESP1 mid-reconnect), and that must NOT immediately bounce
  // the operator back out; only an actual revoke/refuse -- a hold that WAS granted and then taken
  // back -- should do that.
  if (onTab && mtArmed && (state === "revoked" || state === "refused")) {
    setManualTestArmed(false);
    if (state === "revoked") {
      setCommandStatus("Manual mode was revoked at the rig — the operator there took control.", "error");
      document.querySelector('.tab[data-view="dashboard"]')?.click();
    } else if (state === "refused") {
      // Previously only "revoked" stopped the 20s keep-alive (via the dashboard bounce triggering
      // releaseManualHold()) -- a refusal mid-session left it re-requesting a hold ESP1 had just
      // said no to, indefinitely, until the operator manually navigated away.
      releaseManualHold();
    }
  }
  if (gate) gate.hidden = mtArmed;

  // Stale/disconnected/lost-hold banner + reminder, INSIDE #mtControls (only visible once armed --
  // the gate above carries its own version of this messaging for before Proceed is pressed). Buttons
  // themselves are separately gated by syncControlAvailability()'s normalAllowed/deviceIsFresh() check
  // -- this note only explains WHY they're disabled, or reminds the operator what this tab does once
  // they're re-enabled after a reconnect.
  const controlsNote = document.getElementById("mtControlsNote");
  if (controlsNote && mtArmed) {
    if (!deviceIsFresh()) {
      controlsNote.textContent = `ESP1 appears offline — ${snapshotAgeText()}. Requests are queued but nothing runs until it reconnects.`;
      controlsNote.className = "control-result error";
    } else if (state === "held") {
      controlsNote.textContent = "Direct hardware control — this tab drives the hardware directly.";
      controlsNote.className = "control-result";
    } else {
      // A genuinely transient state (idle/requesting): device is fresh but ESP1 hasn't granted the
      // hold yet. Revoked/refused are handled above (they flip mtArmed off before this note runs), so
      // this branch cannot see them -- nothing here needs to stop an in-progress hold-to-run press.
      // The grant forces a snapshot, but that one can miss it, and then the next is up to a minute away.
      controlsNote.textContent = "Waiting for the rig to grant manual mode. This can take up to a minute to show here.";
      controlsNote.className = "control-result";
    }
  }
}

function setManualTestArmed(on) {
  // Deep-scan finding (2026-09-10, low): this function itself had no privilege check of its own --
  // every OTHER guard on this reveal (the tab-click handler, mtProceed's own disabled state) could be
  // bypassed by calling this directly from a console (script.js's top-level functions are all
  // implicitly on window). Impact was cosmetic only (every real write underneath -- queueCommand()'s
  // operatorOnly, writeTestHold()'s own check -- independently re-verifies privilege client- AND
  // server-side regardless), but closing it here matches the same "a function invoked manually must
  // still be rejected" standard already applied to Proceed and the tab-click handler.
  if (on && (!canAccessPrivilegedTabs() || rigLocked())) return;
  mtArmed = on;
  const gate = document.getElementById("mtGate");
  const body = document.getElementById("mtControls");
  if (gate) gate.hidden = on;
  if (body) body.hidden = !on;
  if (!on) mtHoldStop(true);
}

// Dosing pumps run at roughly PUMP_FLOWRATE_MLPM (50 mL/min) in the firmware, so a pulse dispenses a
// real, if small, volume. Say so before it is run -- especially for the pH pumps, which dispense
// corrosive adjuster into the tank.
const MT_DOSING = { nutA: 1, nutB: 1, nutC: 1, phUp: 1, phDn: 1 };   // no nutD -- see index.html
function updatePulseNote() {
  const t = document.getElementById("pulseTarget")?.value || "";
  const s = Number(document.getElementById("pulseSeconds")?.value || 0);
  const note = document.getElementById("pulseVolumeNote");
  if (!note) return;
  if (MT_DOSING[t] && s > 0) {
    const ml = (50 * s / 60).toFixed(1);
    const corrosive = (t === "phUp" || t === "phDn");
    note.textContent = `This dispenses roughly ${ml} mL of ${corrosive ? "pH adjuster (corrosive)" : "nutrient concentrate"} into the mixing tank.`;
    note.className = corrosive ? "control-result error" : "field-note";
  } else if (t === "servoL" || t === "servoR") {
    note.textContent = "Moves only the pump-selector servo on the Nano, with the inverter OFF, so no pump runs; the Pump selector switch above does the same in one press. Give it 10 to 15 seconds: ESP2 first waits for the inverter to be dead, and ESP1 from 16 Nov 2026 ends the test as soon as the move is settled. The result says whether the Nano confirmed the move.";
    note.className = "field-note";
  } else if (t === "mixer") {
    note.textContent = "The mixer is a 12 V DC motor on its own relay (no inverter). It has no flow meter, so this reports no flow reading — only that the relay ran.";
    note.className = "field-note";
  } else {
    note.textContent = "";
    note.className = "field-note";
  }
}

// The pulse verdict comes back as the command's own status detail, which ESP1 fills in with the
// meter count. Read the newest TEST_PULSE out of the command feed rather than inventing a second
// telemetry path for it.
function renderPulseResult() {
  const el = document.getElementById("pulseResult");
  if (!el) return;
  const latest = latestCommandOfType("TEST_PULSE");
  if (!latest) return;
  const detail = commandDetailText(latest, "");
  el.textContent = `${rawText(latest.status, "pending")} — ${detail || "waiting for ESP1"}`;
  // ESP1 of 15 Nov answers an unconfirmed servo move "completed"; from 16 Nov it is "failed".
  el.className = "control-result"
    + (/NO FLOW|NO LEVEL CHANGE|SERVO (NOT CONFIRMED|FAIL)/i.test(detail) || latest.status === "failed" || latest.status === "rejected" ? " error" : "");
}

/* ---- Pump selector switch (Manual/Test, 2026-11-16) -----------------------------------------------
 * Which pump the Nano's servo has selected, and two buttons that move it. A move is a TEST_PULSE on a servo row
 * (servoL = TRANSFER, servoR = BOOSTER): ESP1 powers ESP2 up, ESP2 keeps the inverter OFF and checks its AC is dead,
 * then ESP1 has the Nano turn the servo, so no pump runs. ESP1 from 16 Nov 2026 ends the pulse as soon as the move is
 * settled, so SERVO_MOVE_SECONDS is only its upper bound (covering ESP2's own 10 s servo limit), and publishes where
 * the Nano last confirmed the servo as system.servoSide. A move in flight is read from the command feed, not kept on
 * this page, so every open page shows it, and a reload in the middle of one still does. */
const SERVO_MOVE_SECONDS = 15;
// ESP1 picks a row up within 3 s, ESP2 takes up to 14 s to enter Testing, the move up to 15 s, and the answer
// takes a few seconds more. A row still unsettled after this is no longer treated as moving.
const SERVO_MOVE_WAIT_MAX_MS = 90000;
const SERVO_PUMP_TEXT = { TRANSFER: "Transfer pump", BOOSTER: "Booster pump" };
let servoResultShown = "";      // which row's outcome the result line shows, so a local "Not sent" isn't overwritten

function latestServoMove() {
  return commandData
    .filter(c => c.type === "TEST_PULSE" && SERVO_TARGETS[c.payload?.target])
    .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
}

function servoMoveInFlight() {
  const row = latestServoMove();
  if (!row || COMMAND_END_STATUSES.has(row.status)) return null;
  return serverNow() - Number(row.updatedAt || row.requestedAt || 0) < SERVO_MOVE_WAIT_MAX_MS ? row : null;
}

// ESP1's answer: "SERVO OK: selector on the TRANSFER side" once the Nano confirmed it. ESP1 of 15 Nov also answers
// "completed" when it wasn't confirmed, so only those words count as a confirmed move.
function servoMoveConfirmed(row) {
  return row?.status === "completed" && /SERVO OK/.test(commandDetailText(row, ""));
}

function renderServoSwitch(normalAllowed, normalTitle) {
  const panel = document.getElementById("mtServoPanel");
  if (!panel) return;
  const system = liveData.system || {};
  const side = SERVO_PUMP_TEXT[system.servoSide] ? system.servoSide : "";
  // ESP1 from 16 Nov 2026 publishes the AC current limits unconditionally, and servoSide whenever it knows it.
  const sideReported = hasValue(system.acAmpMin10);
  const moving = servoMoveInFlight();
  const movingTo = moving ? SERVO_TARGETS[moving.payload.target] : "";
  const snapshotAge = hasValue(liveData.meta?.updatedAt) ? Math.max(0, serverNow() - Number(liveData.meta.updatedAt)) : 0;
  setText("mtServoNow", !liveData.system ? "Unavailable"
    : !sideReported ? "Not reported by this ESP1 firmware (it needs the update from 16 Nov 2026). The buttons still move it."
    : !side ? "Not known yet: no move has been confirmed. Press Transfer or Booster to set it."
    : `${SERVO_PUMP_TEXT[side]} (${side} side), confirmed by the Nano ${hasValue(system.servoSideAgeMs)
        ? agoText(Number(system.servoSideAgeMs) + snapshotAge) : "before ESP1's last restart"}`);
  setDeviceStatus("mtServoState", moving ? `MOVING TO ${movingTo}` : side || "UNKNOWN", moving ? "warn" : side ? "active" : "off");

  const block = !normalAllowed ? normalTitle : moving ? "Wait for the move in progress to finish." : "";
  panel.querySelectorAll(".servo-option").forEach(button => {
    const target = SERVO_TARGETS[button.dataset.servoTarget];
    button.setAttribute("aria-pressed", String(!moving && side === target));
    button.classList.toggle("is-moving", movingTo === target);
    button.disabled = Boolean(block);
    button.title = block || (side === target ? `ESP1 reports it on the ${target} side already. Press to move it there again.`
      : `Move the pump selector to the ${target} side.`);
  });

  // The result line: a move in flight says how far it has got; otherwise the newest move's answer, shown once per change
  // so that this page's own "Not sent" (from sendFromPanel()) stays until a newer move appears.
  const row = latestServoMove();
  if (!row) return;
  const signature = `${row.id}|${row.status}|${row.detail || ""}|${moving ? "moving" : ""}`;
  if (signature === servoResultShown) return;
  servoResultShown = signature;
  const show = resultLine("mtServoResult");
  const to = SERVO_TARGETS[row.payload.target];
  if (moving) {
    show(row.status === "queued" ? `Moving to the ${to} side: waiting for ESP1 to pick it up (it checks every 3 seconds)…`
      : row.status === "received" ? `Moving to the ${to} side: ESP1 is checking the request…`
      : `Moving to the ${to} side: ESP1 is starting ESP2, then the Nano turns the servo…`);
    return;
  }
  const failed = row.status === "failed" || row.status === "rejected" || (row.status === "completed" && !servoMoveConfirmed(row));
  show(`${rawText(row.status, "pending")} — ${commandDetailText(row, "waiting for ESP1")}`, failed);
}

function sendServoMove(target) {
  const side = SERVO_TARGETS[target];
  if (!side || servoMoveInFlight()) return;
  const show = resultLine("mtServoResult");
  if (liveData.system?.servoSide === side &&
      !confirm(`ESP1 reports the pump selector on the ${side} side already. Move it there again? That re-seats the lever.`)) {
    show("Nothing was sent.");
    return;
  }
  sendFromPanel(show, "TEST_PULSE", { target, seconds: SERVO_MOVE_SECONDS }, { operatorOnly: true })
    .then(key => { if (key) syncControlAvailability(); });
}
document.querySelectorAll("#mtServoPanel .servo-option").forEach(button =>
  button.addEventListener("click", () => sendServoMove(button.dataset.servoTarget)));

// Flow-meter table. Shares the sweep with the Diagnostics tab -- one DIAG_SWEEP fills both.
const MT_FLOW_LABEL = {
  // web v.4: the reservoir → mix and mix → column meters are gone (the tank ultrasonics meter that water).
  FLOW_NUTA: "Nutrient A", FLOW_NUTB: "Nutrient B", FLOW_NUTC: "Nutrient C",
  FLOW_NUTD: "Nutrient D", FLOW_PHUP: "pH up", FLOW_PHDN: "pH down"
};
function renderFlowMeters() {
  const box = document.getElementById("mtFlowGrid");
  const r = liveData.diagnostics?.sensorsRaw?.esp2;
  const sweeping = Boolean(r?.sweepActive);
  const left = Number(r?.sweepSecondsLeft || 0);
  setDeviceStatus("mtSweepState", sweeping ? (left ? `SWEEPING ${left}s` : "SWEEPING") : "ESP2 IDLE",
                  sweeping ? "active" : "off");
  if (!box) return;
  box.innerHTML = "";
  const vals = r?.values;
  const rows = Object.keys(MT_FLOW_LABEL)
    .filter(id => vals && vals[id])
    .map(id => [MT_FLOW_LABEL[id], `${numberText(vals[id].raw, 0)} · ${vals[id].valid ? "ok" : "BAD"} · ${formatAge(vals[id].ageMs)}`]);
  if (!rows.length) {
    box.innerHTML = '<p class="muted">No flow readings yet. ESP2 is powered down between runs — run a sweep.</p>';
    return;
  }
  box.appendChild(diagnosticGroup("Flow meters (raw pulse counts)", rows));
}

/* Preventive pump exercise. The controls mirror ESP1's current setting rather than assuming a
 * default, so opening the tab shows what the rig is actually doing. Only re-seeded when the operator
 * is NOT mid-edit, otherwise a snapshot landing mid-keystroke would fight them for the field. */
let exTouched = false;
function renderExercise() {
  const ex = liveData.diagnostics?.pumpExercise;
  if (!ex) { setDeviceStatus("exState", "UNKNOWN", "off"); return; }
  const on = Boolean(ex.enabled);
  const hrs = Number(ex.intervalHours || 0);
  setDeviceStatus("exState",
    on ? `ON ${Number(ex.seconds || 0)}s / ${hrs ? hrs / 24 : "?"}d` : "OFF", on ? "active" : "off");
  if (exTouched) return;
  const sel = document.getElementById("exEnabled");
  const secs = document.getElementById("exSeconds");
  if (sel)  sel.value  = on ? "1" : "0";
  if (secs) secs.value = String(Number(ex.seconds || 5));
}
["exEnabled", "exSeconds"].forEach(id =>
  document.getElementById(id)?.addEventListener("input", () => { exTouched = true; }));
document.getElementById("exSaveBtn")?.addEventListener("click", () => {
  const on = document.getElementById("exEnabled")?.value === "1";
  const seconds = numberFromInput("exSeconds");
  const out = document.getElementById("exResult");
  const show = (t, err = true) => { if (out) { out.textContent = t; out.className = `control-result${err ? " error" : ""}`; } };
  // Whole seconds only: ESP1 reads a decimal as absent, so it would keep its old length and still report success.
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 10) { show("Duration must be a whole number from 1 to 10 seconds. Nothing was sent."); return; }
  exTouched = false;                       // let the next snapshot confirm what ESP1 actually stored
  sendFromPanel(show, "SET_EXERCISE", { exerciseEnabled: on, exerciseSeconds: seconds }, { operatorOnly: true });
});

document.getElementById("mtProceed")?.addEventListener("click", () => {
  // A refusal stops the asking once inside, so proceeding asks again. Ask again as well when the gate is
  // showing a refusal while still asking: otherwise the controls opened, and the next report, up to a minute
  // later, carried the same refusal and threw the operator straight back out to press Proceed a second time.
  const state = manualHoldState();
  if (!mtHoldTimer || state === "refused" || state === "revoked") requestManualHold();
  setManualTestArmed(true);
  renderManualHold();        // say what is happening now, not at ESP1's next report
});
document.getElementById("mtBack")?.addEventListener("click", () => {
  setManualTestArmed(false);
  document.querySelector('.tab[data-view="dashboard"]')?.click();
});
document.getElementById("pulseTarget")?.addEventListener("change", updatePulseNote);
document.getElementById("pulseSeconds")?.addEventListener("input", updatePulseNote);
document.getElementById("pulseBtn")?.addEventListener("click", () => {
  const target = document.getElementById("pulseTarget")?.value || "transfer";
  const seconds = numberFromInput("pulseSeconds");
  // ESP1 refuses a push to a column it has disabled ("that column is disabled"), so say so before sending.
  const column = /^col([ABC])$/.exec(target)?.[1];
  if (column && liveData.sensors?.zones?.[column]?.enabled === false) {
    setCommandStatus(`Pulse not sent: ESP1 reports column ${column} as disabled.`, "error"); return;
  }
  // Whole seconds only: ESP1 reads a decimal as 0, then runs its default 5 s, even on a pH pump.
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 15) {
    setCommandStatus("Pulse not sent: duration must be a whole number from 1 to 15 seconds.", "error"); return;
  }
  if ((target === "phUp" || target === "phDn") &&
      !confirm(`Dispense pH adjuster for ${seconds} s? This is corrosive and goes into the mixing tank.`)) return;
  queueCommand("TEST_PULSE", { target, seconds }, { operatorOnly: true });
});
// The Diagnostics tab has its own sweep button; both queue the same DIAG_SWEEP and fill both tables.
// Both are plain buttons (not form submits), so the HTML min/max on the paired input never fires --
// same reason every other actuating numeric field on this page validates in JS before sending.
function sweepSecondsOrReject(inputId) {
  const seconds = numberFromInput(inputId);
  if (!Number.isInteger(seconds) || seconds < 10 || seconds > 120) {
    setCommandStatus("Sweep not sent: length must be a whole number from 10 to 120 seconds.", "error");
    return null;
  }
  return seconds;
}
document.getElementById("sweepBtn")?.addEventListener("click", () => {
  const seconds = sweepSecondsOrReject("sweepSeconds");
  if (seconds !== null) sendFromPanel(resultLine("sweepResult"), "DIAG_SWEEP", { seconds });
});
document.getElementById("mtSweepBtn")?.addEventListener("click", () => {
  const seconds = sweepSecondsOrReject("mtSweepSeconds");
  if (seconds !== null) sendFromPanel(resultLine("mtSweepResult"), "DIAG_SWEEP", { seconds }, { operatorOnly: true });
});
document.querySelectorAll(".tab").forEach(tab => tab.addEventListener("click", () => {
  // Defense-in-depth (2026-09-10, concern raised about non-privileged Manual/Test access): a tab
  // button being [hidden] for an unprivileged/non-operator account is a UI convenience -- it does
  // NOT stop a raw element.click() from a browser console from still firing this SAME listener and
  // activating the view underneath, regardless of visibility. Refuse here too, at the one place all
  // three privileged views are switched to, so "the tab looks reachable" and "the tab is actually
  // reachable" can never diverge. (The actual hardware commands inside were always independently
  // gated by canAccessPrivilegedTabs()/isOperator(), both client and server-side -- this closes the
  // one remaining gap, which was only ever revealing the panel itself, never operating anything.)
  const view = tab.dataset.view;
  if ((view === "manualtest" || view === "system") && !canAccessPrivilegedTabs()) return;
  if (view === "users" && !isOperator()) return;
  if (view === "diagnostics" && !hasControlAccess()) return;
  // Leaving Manual/Test re-arms its gate, so you can never land back on live hardware controls
  // already unlocked from a previous visit.
  // Entering Manual/Test asks the rig for the hold; leaving gives it straight back rather than
  // waiting out the 60 s lease.
  if (view === "manualtest") requestManualHold();
  else { setManualTestArmed(false); releaseManualHold(); mtHoldStop(true); }
  document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
  document.querySelectorAll(".view").forEach(v => v.classList.toggle("active", v.id === view));
  // Don't leave the gate blank until the next snapshot, which can be a minute away.
  if (view === "manualtest") renderManualHold();
}));

const themeToggle = document.getElementById("theme-toggle");
const savedTheme = localStorage.getItem("theme") || "dark";
document.documentElement.dataset.theme = savedTheme;
function refreshThemeButton() { if (themeToggle) themeToggle.textContent = document.documentElement.dataset.theme === "dark" ? "Light theme" : "Dark theme"; }
refreshThemeButton();
themeToggle?.addEventListener("click", () => {
  document.documentElement.dataset.theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  localStorage.setItem("theme", document.documentElement.dataset.theme);
  refreshThemeButton();
});

const contributorsDialog = document.getElementById("contributorsDialog");
document.getElementById("contributorsBtn")?.addEventListener("click", () => contributorsDialog?.showModal());
contributorsDialog?.querySelector(".closeDialog")?.addEventListener("click", () => contributorsDialog.close());

/* ---- Live viewing and Refresh (2026-11-17, user: "a very realtime website, and a refresh button") --------------------
 * While this page is open and on screen it tells ESP1 someone is watching (irrigation/viewer/at, every VIEWER_TOUCH_MS);
 * ESP1 then reports every 2 s during a run and every 4 s otherwise, instead of every 20 / 60 s, and goes back to that
 * pace about 45 s after the last touch. Refresh asks for a report now (irrigation/viewer/refreshAt) and says how long
 * ESP1 took to answer. An older ESP1 ignores the node: the page then simply keeps the old pace. */
const VIEWER_TOUCH_MS = 15000;
const REFRESH_WAIT_MS = 12000;
let viewerTouchTimer = null;
let refreshAskedAt = 0, refreshBaseStamp = 0, refreshTimeout = null, refreshClearTimer = null;

function viewerTouch(extra) {
  if (!db || !auth?.currentUser || !isApprovedUser() || databaseAccessDenied) return Promise.resolve(false);
  const update = Object.assign({ at: firebase.database.ServerValue.TIMESTAMP }, extra || {});
  return db.ref("irrigation/viewer").update(update).then(() => true).catch(error => {
    console.warn("irrigation/viewer", error);
    return false;
  });
}

function syncViewerTouch() {
  const allowed = Boolean(auth?.currentUser) && isApprovedUser() && !databaseAccessDenied;
  const refreshButton = document.getElementById("refreshBtn");
  if (refreshButton) refreshButton.hidden = !allowed;
  const want = allowed && document.visibilityState === "visible";
  if (want && !viewerTouchTimer) {
    viewerTouch();
    viewerTouchTimer = setInterval(() => viewerTouch(), VIEWER_TOUCH_MS);
  } else if (!want && viewerTouchTimer) {
    clearInterval(viewerTouchTimer);
    viewerTouchTimer = null;
  }
}
document.addEventListener("visibilitychange", syncViewerTouch);

function refreshStatusText(text, clearAfterMs) {
  const status = document.getElementById("refreshStatus");
  if (!status) return;
  status.textContent = text;
  if (refreshClearTimer) clearTimeout(refreshClearTimer);
  refreshClearTimer = clearAfterMs ? setTimeout(() => { status.textContent = ""; }, clearAfterMs) : null;
}

function requestRefresh() {
  if (refreshAskedAt || !db || !isApprovedUser()) return;
  const button = document.getElementById("refreshBtn");
  refreshAskedAt = serverNow();
  refreshBaseStamp = Number(liveData.meta?.updatedAt || 0);
  if (button) { button.disabled = true; button.textContent = "Refreshing…"; }
  refreshStatusText("Asking ESP1 for a fresh report…");
  viewerTouch({ refreshAt: firebase.database.ServerValue.TIMESTAMP }).then(ok => {
    if (!ok) finishRefresh(false, "Couldn't ask ESP1: this account may not be allowed, or the connection dropped.");
  });
  // Re-read what Firebase already holds, so the page is current even before ESP1 answers.
  liveRef?.once("value").then(snapshot => {
    liveData = snapshot.val() || {};
    lastKnownFresh = deviceIsFresh();
    updateDashboard();
    setText("liveAge", liveAgeText());
    syncConnection();
    syncControlAvailability();
  }).catch(() => {});
  if (refreshTimeout) clearTimeout(refreshTimeout);
  refreshTimeout = setTimeout(() => finishRefresh(false), REFRESH_WAIT_MS);
}

function finishRefresh(answered, message) {
  if (!refreshAskedAt) return;
  const took = Math.max(0, serverNow() - refreshAskedAt);
  refreshAskedAt = 0;
  if (refreshTimeout) { clearTimeout(refreshTimeout); refreshTimeout = null; }
  const button = document.getElementById("refreshBtn");
  if (button) { button.disabled = false; button.textContent = "Refresh"; }
  if (answered) refreshStatusText(`Fresh report from ESP1 in ${(took / 1000).toFixed(1)} s`, 8000);
  else refreshStatusText(message || "No new report from ESP1 yet: it may be offline or busy, or its firmware is older than 17 Nov.", 15000);
}
document.getElementById("refreshBtn")?.addEventListener("click", requestRefresh);

/* ---- SD card logs (2026-11-17: the Supabase upload expired; the daily CSV now comes through Firebase) ---------------
 * ESP1 puts each day under logs/files/<yyyymmdd>/<nnnn> (pieces of ~6 KB; 12 KB before 17 Nov evening) with logs/index/<yyyymmdd> saying how far it
 * got. The list follows logs/index live; Download joins the pieces into the CSV; "Upload from the rig" writes
 * irrigation/logRequest, which ESP1 polls every 10 s. Operators and the privileged tier only (the rules agree). */
let logsRef = null, logsIndex = {}, logsDenied = false;

function logDayLabel(day) {
  const s = String(day);
  return `${s.slice(0, 4)}-${s.slice(4, 6)}-${s.slice(6, 8)}`;
}

function syncLogsListener() {
  const want = Boolean(db && auth?.currentUser) && canAccessPrivilegedTabs();
  const panel = document.getElementById("logsPanel");
  if (panel) panel.hidden = !want;
  const dayInput = document.getElementById("logDay");
  if (dayInput && !dayInput.value) {
    const now = new Date(serverNow());
    dayInput.value = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}`;
  }
  if (want && !logsRef && !logsDenied) {
    logsRef = db.ref("logs/index");
    logsRef.on("value", snapshot => {
      logsIndex = snapshot.val() || {};
      renderLogs();
      renderMtLog();
    }, error => {
      logsDenied = true;
      logsRef = null;
      setText("logsNote", error.code === "PERMISSION_DENIED"
        ? "This account can't read the logs: they need an operator, a Sub-operator or 1-hour access."
        : `Couldn't read the list: ${error.code || "unknown error"}.`);
    });
  } else if (!want && logsRef) {
    logsRef.off(); logsRef = null; logsIndex = {};
  }
}

function logStateText(entry) {
  const at = Number(entry?.at || 0);
  const when = at ? `, ${agoText(Math.max(0, serverNow() - at))}` : "";
  switch (entry?.state) {
    case "done":
      return `${(Number(entry.size || 0) / 1024).toFixed(0)} KB${entry.partial ? " (the day so far)" : ""}, sent${when}`;
    case "uploading": return `Being sent from the rig${when}…`;
    case "missing":   return `Not on the rig's SD card${when}`;
    case "failed":    return `Sending failed${when}; ESP1 tries again`;
    default:          return rawText(entry?.state, "Unknown");
  }
}

function renderLogs() {
  const list = document.getElementById("logsList");
  if (!list) return;
  const days = Object.keys(logsIndex).filter(day => /^\d{8}$/.test(day)).sort().reverse();
  if (!days.length) {
    list.innerHTML = `<li class="muted">No logs here yet. Finished days arrive after midnight; use Upload from the rig for one now.</li>`;
    return;
  }
  list.innerHTML = days.map(day => {
    const entry = logsIndex[day] || {};
    const ready = entry.state === "done";
    return `<li class="log-row"><span><strong>${escapeHtml(logDayLabel(day))}</strong> · ${escapeHtml(logStateText(entry))}</span>` +
           `<button type="button" data-log-day="${escapeHtml(day)}"${ready ? "" : " disabled"}>Download</button></li>`;
  }).join("");
}

async function downloadLog(day, button) {
  if (!db) return;
  const label = button ? button.textContent : "Download";
  if (button) { button.disabled = true; button.textContent = "Downloading…"; }
  try {
    const snapshot = await db.ref(`logs/files/${day}`).once("value");
    const pieces = snapshot.val() || {};
    const keys = Object.keys(pieces).sort();
    if (!keys.length) { setText("logsNote", `${logDayLabel(day)}: no data in Firebase. Use Upload from the rig.`); return; }
    const text = keys.map(key => String(pieces[key] ?? "")).join("");
    const url = URL.createObjectURL(new Blob([text], { type: "text/csv" }));
    const link = document.createElement("a");
    link.href = url; link.download = `${day}.CSV`;
    document.body.appendChild(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    setText("logsNote", `${logDayLabel(day)}: downloaded (${(text.length / 1024).toFixed(0)} KB).`);
  } catch (error) {
    setText("logsNote", `${logDayLabel(day)}: download failed (${error.code || error.message || "unknown error"}).`);
  } finally {
    if (button) { button.disabled = false; button.textContent = label; }
    if (button?.id === "mtLogDownloadBtn") setText("mtLogResult", document.getElementById("logsNote")?.textContent || "");
  }
}

// Ask ESP1 to send one day's log (irrigation/logRequest). noteId = where to say what happened.
function sendLogRequest(day, noteId) {
  if (!db) return Promise.resolve(false);
  return db.ref("irrigation/logRequest").set({ day, at: firebase.database.ServerValue.TIMESTAMP })
    .then(() => {
      setText(noteId, `Asked the rig for ${logDayLabel(day)}. It is sent as it goes (about 2–3 minutes per megabyte); ESP1 checks every 10 seconds.`);
      return true;
    })
    .catch(error => {
      setText(noteId, error.code === "PERMISSION_DENIED"
        ? "Not sent: this account may not ask (operators, Sub-operators and 1-hour access can), or you asked less than 5 seconds ago."
        : `Not sent: ${error.code || error.message || "unknown error"}.`);
      return false;
    });
}

function requestLogUpload() {
  const value = document.getElementById("logDay")?.value || "";
  const parts = value.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  if (!db || !parts) { setText("logsNote", "Pick a day first."); return; }
  sendLogRequest(Number(parts[1] + parts[2] + parts[3]), "logsNote");
}

/* Manual/Test > SD card log (2026-11-17, user: "a button to update the CSV, especially if I want the logs any time in the
 * morning ... when there is no current / the system is idle"). Today's log, on demand, only while ESP1 is reporting, idle,
 * with no run and no pump, valve or mixer on. ESP1 sends it as far as it is written (partial); Download joins it. */
let mtLogAskedAt = 0;

function todayLogDay() {
  const now = new Date(serverNow());
  return Number(`${now.getFullYear()}${String(now.getMonth() + 1).padStart(2, "0")}${String(now.getDate()).padStart(2, "0")}`);
}

function mtLogBlockReason() {
  if (!deviceIsFresh()) return "ESP1 isn't reporting, so it can't be asked.";
  const state = liveData.system?.state || "";
  if (!["IDLE_STATE", "SAFE_MODE"].includes(state)) return `Only while the rig is idle (it reports ${state || "nothing"}).`;
  if (liveData.system?.masterWorkOrderActive || liveData.diagnostics?.system?.workOrderActive) return "Only while no run is in progress.";
  const a = liveData.actuators || {};
  if (a.transferRunning || a.boosterRunning || a.mixerRunning) return "Only while no pump is running.";
  if (liveData.diagnostics?.actuator?.pumpTestActive) return "Only while no pump test is running.";
  return "";
}

function renderMtLog() {
  const update = document.getElementById("mtLogUpdateBtn");
  const download = document.getElementById("mtLogDownloadBtn");
  if (!update || !download) return;
  const day = todayLogDay();
  const entry = logsIndex[String(day)];
  const state = entry?.state || "";
  const block = mtLogBlockReason();
  const sending = state === "uploading" || (mtLogAskedAt && serverNow() - mtLogAskedAt < 30000 && state !== "done");
  update.disabled = Boolean(block) || sending || !canAccessPrivilegedTabs();
  update.title = block || (sending ? "Being sent now" : "Ask ESP1 to send today's log so far");
  download.disabled = state !== "done";
  setDeviceStatus("mtLogState",
    state === "done" ? (entry.partial ? "SENT (SO FAR)" : "SENT") : state === "uploading" ? "SENDING" : state === "failed" ? "FAILED"
      : state === "missing" ? "NOT ON CARD" : sending ? "ASKED" : "NOT SENT",
    state === "done" ? "active" : state === "uploading" || sending ? "warn" : state === "failed" ? "danger" : "off");
  if (block && !sending) setText("mtLogResult", block);
  else if (entry && !sending) setText("mtLogResult", `Today's log: ${logStateText(entry)}.`);
}

document.getElementById("mtLogUpdateBtn")?.addEventListener("click", () => {
  const block = mtLogBlockReason();
  if (block) { setText("mtLogResult", block); return; }
  sendLogRequest(todayLogDay(), "mtLogResult").then(ok => { if (ok) { mtLogAskedAt = serverNow(); renderMtLog(); } });
});
document.getElementById("mtLogDownloadBtn")?.addEventListener("click", event => {
  downloadLog(String(todayLogDay()), event.currentTarget);
});
document.getElementById("logRequestBtn")?.addEventListener("click", requestLogUpload);
document.getElementById("logsList")?.addEventListener("click", event => {
  const button = event.target.closest("button[data-log-day]");
  if (button) downloadLog(button.dataset.logDay, button);
});

// The snapshot ages between pushes, so the freshness gate has to be re-evaluated on a timer as well
// as on each update -- otherwise a device that goes silent leaves the controls enabled indefinitely.
setInterval(() => {
  setText("liveAge", liveAgeText());
  syncControlAvailability();
  // A temporary Manual/Test + System grant (see canAccessPrivilegedTabs()) expires by clock alone --
  // no Firebase write happens at the exact expiry moment, so nothing else would re-check this. 15 s
  // granularity matches the existing staleness-recheck cadence this tick already runs.
  refreshOperatorUI();
  // A queued row's age warning has to appear without waiting for a command update that may never come.
  if (commandData.some(command => command.status === "queued")) renderCommandHistory();
}, 15000);

// 1 s tick: the armed-run countdown has to move between snapshots (ESP1 publishes every 20-60 s),
// and the fault banner has to re-raise itself the moment a "do nothing" snooze expires -- that
// re-prompt is the whole point of the option.
setInterval(() => {
  syncFreshness();
  setText("liveAge", liveAgeText());   // the age moves every second, now that reports come every few seconds
  syncViewerTouch();                   // start / stop telling ESP1 someone is watching (sign-in, approval, tab hidden)
  syncLogsListener();                  // the SD card logs list, for the accounts that may see it
  renderMtLog();                       // Manual/Test > SD card log: idle-only button, today's state
  updateForceArmed();
  renderUpNext();             // the clock moves on: a watering time opens or closes, the day turns over
  if (faultAckLocalUntil && Date.now() >= faultAckLocalUntil) faultAckLocalUntil = 0;
  updateFaultBanner();
}, 1000);

/* Info icons ------------------------------------------------------------------------------------
 * Small "i" buttons beside features across every tab, each carrying its own explanation in
 * data-info-title / data-info-text. One shared popover is repositioned per click rather than one
 * hidden panel per icon -- with ~50 of these on the page, a per-icon panel would add real markup
 * weight and could drift out of sync with the feature it sits beside; a single popover cannot.
 * Pure UI: reads no live data, sends no command, and is never nested inside an actuating button's
 * own clickable area, so it cannot itself trigger a pump/valve/relay action. */
let infoOpenerEl = null;

function closeInfoPopover() {
  const pop = document.getElementById("infoPopover");
  if (!pop || pop.hidden) return;
  pop.hidden = true;
  if (infoOpenerEl) infoOpenerEl.setAttribute("aria-expanded", "false");
  const opener = infoOpenerEl;
  infoOpenerEl = null;
  opener?.focus();
}

function openInfoPopoverFor(icon) {
  const pop = document.getElementById("infoPopover");
  if (!pop) return;
  // A different icon was already open: reset its own expanded state before this one claims it, or
  // both end up marked aria-expanded="true" at once even though only one popover instance exists.
  if (infoOpenerEl && infoOpenerEl !== icon) infoOpenerEl.setAttribute("aria-expanded", "false");
  pop.querySelector(".info-popover-title").textContent = icon.dataset.infoTitle || "";
  pop.querySelector(".info-popover-text").textContent = icon.dataset.infoText || "";
  // The icon's few words, and a way to the Help entry that explains it in full, when there is one.
  const more = pop.querySelector(".info-popover-more");
  if (more) {
    // Only to an entry this account can see (Help is split by account type).
    more.hidden = !icon.dataset.help || !helpEntryAllowed(icon.dataset.help);
    more.dataset.help = icon.dataset.help || "";
  }
  pop.hidden = false;
  icon.setAttribute("aria-expanded", "true");
  infoOpenerEl = icon;

  // Position after it is visible, so its real size is known, then clamp inside the viewport --
  // a card near the right/bottom edge must not push the popover off-screen.
  const iconRect = icon.getBoundingClientRect();
  const popRect = pop.getBoundingClientRect();
  const margin = 12;
  let left = iconRect.left;
  let top = iconRect.bottom + 8;
  if (left + popRect.width > window.innerWidth - margin) left = window.innerWidth - popRect.width - margin;
  if (left < margin) left = margin;
  if (top + popRect.height > window.innerHeight - margin) top = iconRect.top - popRect.height - 8;
  if (top < margin) top = margin;
  pop.style.left = `${left}px`;
  pop.style.top = `${top}px`;

  pop.querySelector(".info-popover-close")?.focus();
}

function initInfoIcons() {
  document.addEventListener("click", event => {
    const icon = event.target.closest(".info-icon");
    if (icon) {
      event.preventDefault();
      event.stopPropagation();
      if (infoOpenerEl === icon) closeInfoPopover();
      else openInfoPopoverFor(icon);
      return;
    }
    const pop = document.getElementById("infoPopover");
    if (pop && !pop.hidden && !pop.contains(event.target)) closeInfoPopover();
  });
  document.addEventListener("keydown", event => { if (event.key === "Escape") closeInfoPopover(); });
  window.addEventListener("resize", closeInfoPopover);
  document.querySelector("#infoPopover .info-popover-close")?.addEventListener("click", closeInfoPopover);
  document.querySelector("#infoPopover .info-popover-more")?.addEventListener("click", event => {
    const entry = event.currentTarget.dataset.help;
    closeInfoPopover();
    if (entry) openHelpEntry(entry);
  });
}

/* Help tab -------------------------------------------------------------------------------------------
 * Static text in index.html: how the rig decides, how to do each task, and what every word means. Each
 * entry is a <details> with an id, so an info icon's "Read more in Help" can open the right one.
 * Each entry also says who it is for (data-for, 2026-11-05), and an account sees only its own: "all" (every approved
 * account), "asker" (an approved account still without some access), "zones" (Edit zones), "control" (Control),
 * "privileged" (Sub-operator, 1-hour access, operators) and "operator". */
function helpAudienceAllowed(audience) {
  switch (audience) {
    case "zones":      return hasZoneEditAccess();
    case "control":    return hasControlAccess();
    case "privileged": return canAccessPrivilegedTabs();
    case "operator":   return isOperator();
    case "asker":      return isApprovedUser() && !canAccessPrivilegedTabs() && (!myControlAccess || !myZoneEditAccess);
    default:           return isApprovedUser();
  }
}
function helpEntryAllowed(entryId) {
  const entry = document.getElementById(entryId);
  return Boolean(entry) && helpAudienceAllowed(entry.dataset.for);
}
function filterHelp(query) {
  const words = String(query || "").trim().toLowerCase().split(/\s+/).filter(Boolean);
  let shown = 0;
  document.querySelectorAll("#help .help-item").forEach(item => {
    const allowed = helpAudienceAllowed(item.dataset.for);
    const match = allowed && words.every(word => item.textContent.toLowerCase().includes(word));
    item.hidden = !match;
    if (match) shown++;
    item.open = words.length > 0 && match;     // a search opens what it found; clearing it folds them back
  });
  document.querySelectorAll("#help .help-group").forEach(group => {
    group.hidden = !group.querySelector(".help-item:not([hidden])");
  });
  const none = document.getElementById("helpNoMatch");
  if (none) none.hidden = !words.length || shown > 0;
}
document.getElementById("helpSearch")?.addEventListener("input", event => filterHelp(event.target.value));

function openHelpEntry(entryId) {
  const entry = document.getElementById(entryId);
  if (!entry || !helpEntryAllowed(entryId)) return;
  const search = document.getElementById("helpSearch");
  if (search?.value) { search.value = ""; filterHelp(""); }   // a leftover search could be hiding it
  document.querySelector('.tab[data-view="help"]')?.click();
  entry.open = true;
  const reduceMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
  entry.scrollIntoView({ block: "start", behavior: reduceMotion ? "auto" : "smooth" });
  entry.querySelector("summary")?.focus({ preventScroll: true });
}

initRigDrawing();
renderZonesUI();
renderCropLibrary();
updateDashboard();
renderCommandHistory();
syncControlAvailability();
initInfoIcons();
initializeFirebase();
