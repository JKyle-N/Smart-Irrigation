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
 *   TANK_CHECK_OK      {}   (ESP1 from 18 Nov 2026) confirms the mixing tank was checked, as SMS TANKCHECK,OK does;
 *                        only while idle. ESP1 publishes the check as diagnostics.tankCheck {needed, why}.
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
let lastKnownDelayed = false;       // ...and whether it drew its reports as late (deviceDelayMs(), 18 Nov 2026)
let firebaseDownSince = 0;          // when this browser lost Firebase (0 = connected), for nudgeFirebaseReconnect()
let lastReconnectNudge = 0;
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
  // .s-roll: Simple mode's big numbers (18 Nov 2026) roll the same way.
  if ((element.classList.contains("reading-value") || element.classList.contains("s-roll")) && rollNumber(element, String(text))) return;
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
  return valid ? rigTimeText(text) : `Invalid reading (${text}): the clock can't be trusted`;
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

// The age at the top of the dashboard, with the pace ESP1 is reporting at. Both come from ESP1 (meta.refreshMs, and
// meta.linkMode on web v.4 of 18 Nov 2026 e), so a firmware pace change needs no change here.
function liveAgeText() {
  const base = snapshotAgeText();
  if (!liveData.meta?.updatedAt || !deviceIsFresh()) return base;
  return `${base} · ${linkPaceText()}`;
}

// "every 30 s", plus why it is slower while the rig works (18 Nov 2026 e, the owner: "during the operation or an upcoming
// schedule, the system should focus on the operation than the firebase").
function linkPaceText() {
  const every = Math.round(Number(liveData.meta?.refreshMs || 60000) / 1000);
  const pace = every >= 60 && every % 60 === 0 ? `every ${every / 60} min` : `every ${every} s`;
  return liveData.meta?.linkMode === "busy" ? `${pace} while the rig works on a watering` : pace;
}

function deviceIsFresh() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const refreshMs = Number(liveData.meta?.refreshMs || 60000);
  return Boolean(stamp) && serverNow() - stamp < Math.max(refreshMs * 2 + 15000, 90000);
}

// How late ESP1's report is (18 Nov 2026): 0 while it is on time, else its age. ESP1 reports every meta.refreshMs (web v.4
// of 18 Nov g: once a minute), so a report older than one more interval, and at least 5 s more, is late. Only while
// deviceIsFresh(): past that the page says ESP1 has stopped.
function deviceDelayMs() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (!stamp || !deviceIsFresh()) return 0;
  const refreshMs = Number(liveData.meta?.refreshMs || 60000);
  const age = Math.max(0, serverNow() - stamp);
  return age > refreshMs + Math.max(refreshMs, 5000) ? age : 0;
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

// connected: true, false, or "delayed" (18 Nov 2026: connected, but ESP1's reports are late; see deviceDelayMs()).
function setConnection(connected, label) {
  const delayed = connected === "delayed";
  const dot = document.getElementById("connectionDot");
  if (dot) dot.style.backgroundColor = delayed ? "var(--warning)" : connected ? "var(--success)" : "var(--danger)";
  setText("connectionStatus", label);
  setDeviceStatus("sideConnection", delayed ? "DELAYED" : connected ? "LIVE SNAPSHOT" : "NOT LIVE", delayed ? "warn" : connected ? "active" : "off");
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
  return today ? `at ${timeOfDayText(when)}` : `on ${dateTimeText(when)}`;
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
        syncAccountBadge(null);
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
      syncAccountBadge(user);            // the account chip in the top bar (18 Nov 2026)
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
    firebaseDownSince = up ? 0 : (firebaseDownSince || Date.now());
    syncConnection();
    syncControlAvailability();
  });
  liveRef = db.ref("irrigation/live");
  commandsRef = db.ref("irrigation/commands").limitToLast(25);

  liveRef.on("value", snapshot => {
    liveData = snapshot.val() || {};
    lastKnownFresh = deviceIsFresh();
    lastKnownDelayed = deviceDelayMs() > 0;
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
  // No running age here: the top bar's own line (liveAge) already counts the seconds.
  if (fresh && deviceDelayMs()) { setConnection("delayed", "Connected, but ESP1's reports are late"); return; }
  setConnection(fresh, fresh ? "Live system connected" : "Waiting for a current ESP1 snapshot");
}

// A rig that stops reporting sends nothing to react to, so nothing else would redraw the page: the top bar
// would go on saying it's connected, and a pump last reported running would stay lit, for as long as the page
// stayed open.
function syncFreshness() {
  const fresh = deviceIsFresh();
  const delayed = deviceDelayMs() > 0;
  if (fresh === lastKnownFresh && delayed === lastKnownDelayed) return;
  const freshChanged = fresh !== lastKnownFresh;
  lastKnownFresh = fresh;
  lastKnownDelayed = delayed;
  if (freshChanged) updateDashboard();   // late but current: the numbers stand, only the connection line changes
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
    syncAccountBadge();                  // the name in the top bar (18 Nov 2026)
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
    const created = u?.createdAt ? dateTimeText(u.createdAt) : "Unknown";
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
    if (tempActive) access.push(`Can use Manual/Test, System and the rig lock until ${timeOfDayText(new Date(u.tempAccessUntil))} (1-hour access).`);
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
const SHOWN_IN_NEXT_REPORT = new Set(["EMERGENCY_STOP", "ESTOP_RECOVER", "ENABLE_ACTUATIONS", "DISABLE_ACTUATIONS", "SET_COLUMN", "SET_EXERCISE", "SET_CLOCK", "RESTORE_DEFAULTS", "CLEAR_BATT_SWITCH", "TANK_CHECK_OK"]);

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
    case "SET_THRESH":         return `Watering thresholds: start ${rawText(p.thStart, "?")} %, gap ${rawText(p.thGap, "?")}` +
                                 (hasValue(p.thWet) ? `, too wet ${p.thWet} %` : "") + (hasValue(p.thRes) ? `, reservoir pause ${p.thRes} %` : "");
    case "SET_AC_VOLT":        return `AC voltage limits: ${rawText(p.vMin, "?")}–${rawText(p.vMax, "?")} V`;
    case "SET_AC_AMP":         return `AC current limits: ${ampsFromTenthsText(p.aMin10)}–${ampsFromTenthsText(p.aMax10)} A`;
    case "RESTORE_DEFAULTS":   return "Restored factory settings";
    case "CLEAR_BATT_SWITCH":  return "Cleared the battery switch fault";
    case "TANK_CHECK_OK":      return "Confirmed the mixing tank was checked";
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

/* ---- Operation and day plan (2026-11-18, user request) -----------------------------------------------------------------
 * Automatic: a column waters on its watering times, as before. Manual: readings only. ESP1 starts no scheduled run for the
 * column, and the operator waters or fertigates it with "Water a column now" (forced runs still work). Each column has a
 * default Operation plus a day plan of single days that override it: in Edit settings a month calendar, where a click
 * cycles a day default -> Manual -> Automatic -> default. ESP1 keeps both (run_plan.h, firmware of 18 Nov 2026) and follows
 * them by its own clock, so the plan works with this page closed. It publishes, per column: operation (the default),
 * operationToday, and plan, one character a day from planFrom (ESP1's today): '-' default, 'M' Manual, 'A' Automatic.
 * SET_COLUMN takes {operation, plan, planFrom}. Older firmware publishes none of it, and the page doesn't offer it. */
const PLAN_DAYS = 62;                       // RUN_PLAN_DAYS in the firmware: today and the next 61 days
const OPERATION_SHORT = { AUTOMATIC: "Automatic", MANUAL: "Manual" };
let zoneCardsPlan = false;                  // whether the cards were last built with the Operation parts
const planViewMonth = {};                   // column id -> the month (yyyymm01) its plan calendar shows

function ymdOfDate(date) {
  return date.getFullYear() * 10000 + (date.getMonth() + 1) * 100 + date.getDate();
}
function validYmd(value) {
  const n = Number(value);
  if (!Number.isInteger(n)) return false;
  const y = Math.floor(n / 10000), m = Math.floor(n / 100) % 100, d = n % 100;
  if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1) return false;
  return d <= new Date(Date.UTC(y, m, 0)).getUTCDate();
}
// Whole days, counted in UTC so a daylight-saving change in the viewer's own time zone can't skip or repeat a date.
function ymdToDayNumber(ymd) {
  const n = Number(ymd);
  return Math.round(Date.UTC(Math.floor(n / 10000), Math.floor(n / 100) % 100 - 1, n % 100) / 86400000);
}
function dayNumberToYmd(day) {
  const d = new Date(day * 86400000);
  return d.getUTCFullYear() * 10000 + (d.getUTCMonth() + 1) * 100 + d.getUTCDate();
}
function ymdAddDays(ymd, days) { return dayNumberToYmd(ymdToDayNumber(ymd) + days); }
function monthOfYmd(ymd) { return Math.floor(Number(ymd) / 100) * 100 + 1; }   // yyyymm01
function ymdLabel(ymd) {                     // "Fri 20 Nov", the same form Up next uses
  const n = Number(ymd);
  const date = new Date(Math.floor(n / 10000), Math.floor(n / 100) % 100 - 1, n % 100);
  return `${date.toLocaleDateString([], { weekday: "short" })} ${date.getDate()} ${date.toLocaleDateString([], { month: "short" })}`;
}
// ESP1's date: a plan's days are its days. The browser's own date only while ESP1's clock can't be read.
function rigTodayYmd() { return ymdOfDate(rigNow() || new Date(serverNow())); }
function planSupported() {
  return Object.values(liveData.sensors?.zones || {}).some(z => z?.operation === "AUTOMATIC" || z?.operation === "MANUAL");
}
// {yyyymmdd: "M" | "A"} for the marked days of a plan text that starts at fromYmd; null without a usable text.
function planMarksFrom(text, fromYmd) {
  if (typeof text !== "string" || !validYmd(fromYmd)) return null;
  const marks = {};
  [...text.slice(0, PLAN_DAYS)].forEach((ch, i) => { if (ch === "M" || ch === "A") marks[ymdAddDays(fromYmd, i)] = ch; });
  return marks;
}
// The text ESP1 takes, from today: one character a day, trailing defaults cut off, so "" means no plan at all.
function planTextFrom(marks, todayYmd) {
  let text = "";
  for (let i = 0; i < PLAN_DAYS; i++) text += marks?.[ymdAddDays(todayYmd, i)] || "-";
  return text.replace(/-+$/, "");
}
// What applies on a day: that day's mark, else the column's default.
function operationOn(s, ymd) {
  const mark = s.plan?.[ymd];
  return mark === "M" ? "MANUAL" : mark === "A" ? "AUTOMATIC" : s.operation;
}
// The marked days from today on, in order.
function plannedDays(marks, todayYmd) {
  const days = [];
  for (let i = 0; i < PLAN_DAYS; i++) {
    const ymd = ymdAddDays(todayYmd, i);
    if (marks?.[ymd]) days.push(ymd);
  }
  return days;
}

// One month of a column's day plan as a 7-column grid, Sunday first. editable: each day is a button (Edit settings);
// otherwise plain cells (under the readings). A day is drawn as what applies then: amber Manual, green Automatic; a day
// set in the plan carries its word and a stronger edge, the rest follow the default. Days before today or past the
// plan's reach are greyed and can't be changed.
const PLAN_WEEK_START = Date.UTC(2026, 10, 1);   // 1 Nov 2026 was a Sunday
// locked: shown but not clickable, while ESP1 can't take a plan yet (firmware before 18 Nov 2026).
function planCalendarHtml(id, { month, marks, defaultOp, today, editable, locked = false }) {
  const y = Math.floor(month / 10000), m = Math.floor(month / 100) % 100;
  const firstDay = ymdToDayNumber(month);
  const dim = new Date(Date.UTC(y, m, 0)).getUTCDate();
  const lead = new Date(Date.UTC(y, m - 1, 1)).getUTCDay();
  const todayN = ymdToDayNumber(today), lastN = todayN + PLAN_DAYS - 1;
  const canBack = month > monthOfYmd(today), canOn = month < monthOfYmd(dayNumberToYmd(lastN));
  const title = new Date(y, m - 1, 1).toLocaleDateString([], { month: "long", year: "numeric" });
  const nav = (dir, ok, words) => `<button type="button" class="plan-nav" data-plan-nav="${dir}" data-col="${id}"${ok ? "" : " disabled"} aria-label="${words}">${dir < 0 ? "‹" : "›"}</button>`;
  const dows = Array.from({ length: 7 }, (_, i) => new Date(PLAN_WEEK_START + i * 86400000)
    .toLocaleDateString([], { weekday: "short", timeZone: "UTC" }));
  const cells = [];
  for (let i = 0; i < lead; i++) cells.push(`<span class="plan-blank" aria-hidden="true"></span>`);
  for (let d = 1; d <= dim; d++) {
    const n = firstDay + d - 1;
    const ymd = dayNumberToYmd(n);
    const inRange = n >= todayN && n <= lastN;
    const mark = inRange ? marks?.[ymd] || "" : "";
    const op = mark === "M" ? "MANUAL" : mark === "A" ? "AUTOMATIC" : defaultOp;
    const cls = ["plan-day", inRange ? (op === "MANUAL" ? "is-manual" : "is-auto") : "is-out",
                 mark ? "is-marked" : "", n === todayN ? "is-today" : ""].filter(Boolean).join(" ");
    const tag = mark === "M" ? "Manual" : mark === "A" ? "Auto" : "";
    const words = `${ymdLabel(ymd)}${n === todayN ? " (today)" : ""}: ${inRange ? `${OPERATION_SHORT[op] || "unknown"}${mark ? ", set in the plan" : ", the default"}` : "outside the plan"}`;
    const body = `<span class="plan-num">${d}</span><span class="plan-tag">${tag}</span>`;
    cells.push(editable
      ? `<button type="button" class="${cls}" data-plan-day="${ymd}" data-col="${id}"${inRange && !locked ? "" : " disabled"} aria-label="${escapeHtml(words)}" title="${escapeHtml(words)}">${body}</button>`
      : `<span class="${cls}" role="img" aria-label="${escapeHtml(words)}" title="${escapeHtml(words)}">${body}</span>`);
  }
  return `<div class="plan-cal${editable ? " editable" : ""}">
      <div class="plan-cal-head">${nav(-1, canBack, "Previous month")}<span class="plan-month">${escapeHtml(title)}</span>${nav(1, canOn, "Next month")}</div>
      <div class="plan-grid">${dows.map(w => `<span class="plan-dow">${escapeHtml(w)}</span>`).join("")}${cells.join("")}</div>
      <p class="plan-legend"><span class="plan-swatch is-manual"></span>Manual <span class="plan-swatch is-auto"></span>Automatic
        <span class="plan-legend-note">A day with a word on it is set in the plan; the others follow the default (${escapeHtml(OPERATION_SHORT[defaultOp] || "unknown")}).</span></p>
    </div>`;
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
    targets: Object.fromEntries(TARGET_FIELDS.map(f => [f.key, numberOrNull(z[f.key])])),
    // Operation + day plan (ESP1 from 18 Nov 2026; null from older firmware).
    operation: z.operation === "AUTOMATIC" || z.operation === "MANUAL" ? z.operation : null,
    plan: planMarksFrom(z.plan, z.planFrom),
    // Why a due column is passed over (ESP1 from 18 Nov 2026): resting after repeated faults, or the day closed.
    restMin: numberOrNull(z.restMin),
    skippedToday: z.skippedToday === true,
    // Its watering lines (ESP1 from 18 Nov 2026 b): as they apply, and whether they are its own or the defaults.
    startOwn: z.soilStartOwn === true,
    wetOwn: z.soilWetOwn === true,
    startPct: hasValue(z.soilStartPct) ? Number(z.soilStartPct) : soilStartPercent(),
    wetPct: hasValue(z.soilWetPct) ? Number(z.soilWetPct) : null
  };
  let wetRaw = s.wetPct;
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
      if (p.operation === "AUTOMATIC" || p.operation === "MANUAL") s.operation = p.operation;
      if (typeof p.plan === "string") s.plan = planMarksFrom(p.plan, hasValue(p.planFrom) ? Number(p.planFrom) : rigTodayYmd());
      // -1 sends a column back to the default line.
      if (hasValue(p.soilStart)) { s.startOwn = Number(p.soilStart) >= 0; s.startPct = s.startOwn ? Number(p.soilStart) : soilStartPercent(); }
      if (hasValue(p.soilWet)) { s.wetOwn = Number(p.soilWet) >= 0; wetRaw = s.wetOwn ? Number(p.soilWet) : soilWetDefault(); }
    });
  s.wetPct = wetEffPercent(wetRaw ?? soilWetDefault(), s.startPct);
  // What applies today, by ESP1's date. ESP1 also publishes it (operationToday); worked out here so a confirmed change
  // shows before the next report does.
  s.operationToday = s.operation ? operationOn(s, rigTodayYmd()) : null;
  // On the default window ESP1 reports the default, whatever own times it has kept; on its own times, those.
  if (s.sched === 0 && DEFAULT_WINDOWS[id]) s.windows = [DEFAULT_WINDOWS[id]];
  else if (s.sched === 1 && s.ownWindows) s.windows = s.ownWindows;
  delete s.ownWindows;
  if (s.windows) [s.ws, s.we] = s.windows[0];
  return s;
}

// "6:00 AM – 7:00 AM", "6:00 AM – 7:00 AM and 12:00 PM – 1:00 PM", ... (the 12-hour clock since 18 Nov 2026)
function windowListText(windows) {
  const parts = (windows || []).map(([a, b]) => clockSpan(a, b));
  return parts.length > 1 ? `${parts.slice(0, -1).join(", ")} and ${parts.at(-1)}` : parts[0] || "";
}
function scheduleText(id, sched, windows) {
  if (sched === 0) {
    const [ds, de] = DEFAULT_WINDOWS[id] || windows?.[0] || [null, null];
    return `${clockSpan(ds, de)} (rig default)`;
  }
  if (sched === 1 && windows?.length) return `${windowListText(windows)} (${windows.length > 1 ? "my own times" : "my own time"})`;
  return "Unavailable";
}

function targetsText(targets) {
  if (!TARGET_FIELDS.every(f => hasValue(targets[f.key]))) return "Unavailable";
  const t = targets;
  const none = Number(t.targetN) === 0 && Number(t.targetP) === 0 && Number(t.targetK) === 0;
  return none ? `None set: no nutrients added (pH ${t.targetPH})`
    : `N ${t.targetN} · P ${t.targetP} · K ${t.targetK} ppm · pH ${t.targetPH}`;
}
// What it does, in two words (the long form is in its (i)).
const MODE_SHORT = { AUTO: "Water + nutrients", IRRIGATION_ONLY: "Water only" };

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

// How far through today the column is, in a few words: "Not watered yet", "Done for today · Watered once (5.0 L)",
// "Manual day: no scheduled watering". Only ESP1 from 5 Nov 2026 reports runsToday, from 6 Nov the tally (zones.today).
// "Done" rather than "watered": ESP1 also closes the rest of the day on a STOP reply or a lost fault.
function zoneTodayText(id, s, runs) {
  if (s.known && !s.enabled) return "Switched off: not watered";
  const tally = dayTallyText(liveData.sensors?.zones?.[id]?.today).replace(/^Today: /, "").replace(/\.$/, "");
  const parts = [];
  // Why ESP1 is passing a due column over (firmware of 18 Nov 2026), first.
  if (s.skippedToday) parts.push("Skipped for the rest of today: a STOP reply or a lost fault closed the day");
  else if (s.restMin) parts.push(`Resting ${s.restMin} more min after repeated faults`);
  let fedNote = "";
  if (s.operationToday === "MANUAL") parts.push("Manual day: no scheduled watering");
  else if (s.runsToday !== null && s.sched !== null && !s.skippedToday) {
    const done = Math.min(s.runsToday, runs);
    if (runs > 1) parts.push(`${done} of ${runs} watering times done`);
    else if (done) parts.push("Done for today");
    else if (!tally) parts.push("Not watered yet");
    if (s.fedToday && s.mode === "AUTO" && done < runs) fedNote = "nutrients already in, the rest is plain water";
  }
  if (tally) parts.push(tally.charAt(0).toUpperCase() + tally.slice(1));
  if (fedNote) parts.push(fedNote);
  return parts.length ? parts.join(" · ") : "Nothing reported yet";
}

// The card's own words for what ESP1 does with the column (18 Nov 2026, the owner's second round: short, under this
// week's plan, with the long explanations in the (i) icons). Runs on every snapshot and command update.
function renderZoneSummary(zone) {
  const id = zone.id;
  const s = zoneSettings(id);
  const off = s.known && !s.enabled;
  const block = document.querySelector(`.zone-block[data-zone="${id}"]`);
  if (block) block.classList.toggle("is-off", off);
  setText(`zoneDoes${id}`, s.mode ? MODE_SHORT[s.mode] : "Unavailable");
  const runs = s.sched === 1 && s.windows ? s.windows.length : 1;
  setText(`zoneWhen${id}`, s.sched === null || !s.windows?.length ? "Unavailable" : windowListText(s.windows));
  // The long form stays one hover away: whose times they are, and how often.
  const when = document.getElementById(`zoneWhen${id}`);
  if (when) when.title = s.sched === null ? "Watering times" : `${scheduleText(id, s.sched, s.windows)}, ${runs > 1 ? "at most once in each" : "once a day"}, when the soil is drier than ${s.startPct} %` +
    (s.operation === "MANUAL" ? ". Only on days planned Automatic" : "");
  // Operation: the default, and today's when the day plan says otherwise. Firmware before 18 Nov 2026 has only Automatic.
  setText(`zoneOps${id}`, !s.operation ? (s.known ? "Automatic" : "Unavailable")
    : OPERATION_SHORT[s.operation] + (s.operationToday && s.operationToday !== s.operation ? ` (today ${OPERATION_SHORT[s.operationToday]})` : ""));
  const ops = document.getElementById(`zoneOps${id}`);
  if (ops) ops.dataset.op = off ? "off" : (s.operationToday || s.operation) === "MANUAL" ? "manual" : s.known ? "auto" : "none";
  renderZoneWeek(id, s);
  renderPlanView(id, s);
  setText(`zoneToday${id}`, zoneTodayText(id, s, runs));
  setText(`zoneTargets${id}`, targetsText(s.targets));
  const offNote = document.getElementById(`zoneOffNote${id}`);
  if (offNote) offNote.hidden = !(s.known && !s.enabled);
  renderZonePower(id, s);
  if (zoneDrafts[id]) renderZoneChanges(id);
}

/* ---- Day plan views (18 Nov 2026) ---- */
// This week on a column's card (the owner's second round, 18 Nov 2026): Sunday to Saturday by ESP1's own date, each
// day with its date and what applies then, A (Automatic) or M (Manual), today ringed. Read-only: it changes in Edit
// settings. Days already gone have no mark (the plan only reaches forward). Rebuilt only when what it shows changes.
const zoneWeekKeys = {};
function weekdayShort(ymd) {
  const n = Number(ymd);
  return new Date(Date.UTC(Math.floor(n / 10000), Math.floor(n / 100) % 100 - 1, n % 100))
    .toLocaleDateString([], { weekday: "short", timeZone: "UTC" });
}
function renderZoneWeek(id, s = zoneSettings(id)) {
  const box = document.getElementById(`zoneWeek${id}`);
  if (!box) return;
  const today = rigTodayYmd();
  const todayN = ymdToDayNumber(today);
  const sunday = todayN - new Date(todayN * 86400000).getUTCDay();
  const off = s.known && !s.enabled;
  const planOn = Boolean(s.operation);
  const days = Array.from({ length: 7 }, (_, i) => {
    const n = sunday + i;
    const ymd = dayNumberToYmd(n);
    const past = n < todayN;
    const op = !s.known ? "" : off ? "OFF" : past ? "" : planOn ? operationOn(s, ymd) : "AUTOMATIC";
    return { n, ymd, past, op, marked: planOn && !past && Boolean(s.plan?.[ymd]) };
  });
  const key = JSON.stringify([today, s.known, off, planOn, days.map(d => `${d.op}${d.marked ? "*" : ""}`)]);
  if (zoneWeekKeys[id] === key) return;
  zoneWeekKeys[id] = key;
  const WORD = { AUTOMATIC: "Automatic", MANUAL: "Manual", OFF: "Off" };
  const TAG = { AUTOMATIC: "A", MANUAL: "M", OFF: "Off" };
  box.innerHTML = days.map(d => {
    const isToday = d.n === todayN;
    const words = `${ymdLabel(d.ymd)}${isToday ? ", today" : ""}: ${d.past ? "already past" : WORD[d.op] || "unknown"}${d.marked ? " (set in the day plan)" : ""}`;
    const cls = ["zw-day", isToday ? "is-today" : "", d.past ? "is-past" : "", d.op ? `is-${d.op.toLowerCase()}` : "", d.marked ? "is-marked" : ""].filter(Boolean).join(" ");
    return `<li class="${cls}"><span class="s-visually-hidden">${escapeHtml(words)}</span>` +
      `<span class="zw-dow" aria-hidden="true">${escapeHtml(weekdayShort(d.ymd))}</span>` +
      `<span class="zw-num" aria-hidden="true">${d.ymd % 100}</span>` +
      `<span class="zw-tag" aria-hidden="true">${d.past ? "" : TAG[d.op] || "?"}</span></li>`;
  }).join("");
  const note = document.getElementById(`zoneWeekNote${id}`);
  if (note) {
    note.textContent = off ? "Switched off: no watering on any day."
      : !s.known ? "Waiting for the controller's report."
      : planOn ? "A = Automatic (waters when dry) · M = Manual (no scheduled watering)."
      : "A = Automatic. Manual days need ESP1's firmware of 18 Nov 2026.";
  }
}

// Under a column's readings: its plan as ESP1 has it. Rebuilt only when what it shows changes, so a month picked with
// the arrows stays put between reports, and nothing flickers every few seconds.
const planViewKeys = {};
function renderPlanView(id, s = zoneSettings(id)) {
  const box = document.getElementById(`planView${id}`);
  if (!box) return;
  // Hidden while the column's editor is open: its own calendar is right beside it, and this one would say "press Edit".
  if ((s.known && !s.enabled) || zoneDrafts[id] || (!s.operation && !s.known)) { box.hidden = true; planViewKeys[id] = ""; return; }
  // ESP1 reports the column but not its Operation: firmware from before 18 Nov 2026. Say what is missing, rather than
  // leaving the feature invisible (the owner looked for it on 18 Nov and found nothing).
  if (!s.operation) {
    if (planViewKeys[id] === "waiting") return;
    planViewKeys[id] = "waiting";
    box.hidden = false;
    box.innerHTML = `<p class="plan-none">Day plan (Manual or Automatic days): ESP1 needs its firmware of 18 Nov 2026 first. Until then every day is Automatic.</p>`;
    return;
  }
  const today = rigTodayYmd();
  const days = plannedDays(s.plan, today);
  const lastMonth = monthOfYmd(ymdAddDays(today, PLAN_DAYS - 1));
  let month = planViewMonth[id] || monthOfYmd(today);
  if (month < monthOfYmd(today) || month > lastMonth) month = monthOfYmd(today);
  planViewMonth[id] = month;
  const marks = s.plan || {};
  const key = JSON.stringify([s.operation, today, month, days.map(d => `${d}${marks[d]}`), hasZoneEditAccess()]);
  if (key === planViewKeys[id]) return;
  planViewKeys[id] = key;
  box.hidden = false;
  if (!days.length && s.operation === "AUTOMATIC") {
    box.innerHTML = `<p class="plan-none">Automatic every day, with no day plan.${hasZoneEditAccess() ? " To plan Manual days, press Edit settings." : ""}</p>`;
    return;
  }
  // Today, and the next day that differs from it, in words above the calendar.
  const todayOp = operationOn(s, today);
  let change = "No change planned.";
  for (let i = 1; i < PLAN_DAYS; i++) {
    const ymd = ymdAddDays(today, i);
    const op = operationOn(s, ymd);
    if (op !== todayOp) { change = `Next change: ${ymdLabel(ymd)}, ${OPERATION_SHORT[op]}.`; break; }
  }
  box.innerHTML = `<div class="plan-view-head"><h4>Day plan<button type="button" class="info-icon" aria-haspopup="dialog" aria-expanded="false" aria-label="About the day plan" data-info-title="Day plan" data-info-text="Which days this column runs Automatic and which Manual, as ESP1 has it. Manual means readings only: no scheduled run, you water it with Water a column now. Change it in Edit settings." data-help="help-operation">i</button></h4>` +
    `<p class="plan-next">Today: <strong>${OPERATION_SHORT[todayOp]}</strong>. ${escapeHtml(change)}</p></div>` +
    planCalendarHtml(id, { month, marks, defaultOp: s.operation, today, editable: false });
}

// Inside Edit settings: the draft's plan, where a click changes a day.
function renderPlanEditor(id) {
  const d = zoneDrafts[id];
  const box = document.getElementById(`cfgPlan${id}`);
  if (!d || !box) return;
  const today = rigTodayYmd();
  const lastMonth = monthOfYmd(ymdAddDays(today, PLAN_DAYS - 1));
  if (!d.planMonth || d.planMonth < monthOfYmd(today) || d.planMonth > lastMonth) d.planMonth = monthOfYmd(today);
  const locked = !planSupported();
  box.innerHTML = planCalendarHtml(id, { month: d.planMonth, marks: d.plan, defaultOp: d.operation || "AUTOMATIC", today, editable: true, locked });
  const clear = document.getElementById(`cfgPlanClear${id}`);
  if (clear) clear.disabled = locked || !plannedDays(d.plan, today).length;
}

// default -> Manual -> Automatic -> default (the user's choice, 18 Nov 2026).
const PLAN_CYCLE = { "": "M", M: "A", A: "" };
function cyclePlanDay(id, ymd) {
  const d = zoneDrafts[id];
  if (!d) return;
  const next = PLAN_CYCLE[d.plan[ymd] || ""];
  if (next) d.plan[ymd] = next; else delete d.plan[ymd];
  renderPlanEditor(id);
  renderZoneChanges(id);
  document.querySelector(`#cfgPlan${id} [data-plan-day="${ymd}"]`)?.focus();
}

function shiftMonth(month, step) {
  const date = new Date(Date.UTC(Math.floor(month / 10000), Math.floor(month / 100) % 100 - 1 + step, 1));
  return date.getUTCFullYear() * 10000 + (date.getUTCMonth() + 1) * 100 + 1;
}

// One listener for every plan calendar: the cards are rebuilt often, so nothing is bound to their buttons.
document.addEventListener("click", event => {
  const day = event.target.closest("button[data-plan-day]");
  if (day) {
    if (!day.disabled) cyclePlanDay(day.dataset.col, Number(day.dataset.planDay));
    return;
  }
  const nav = event.target.closest("button[data-plan-nav]");
  if (!nav || nav.disabled) return;
  const id = nav.dataset.col;
  const step = Number(nav.dataset.planNav);
  if (nav.closest(".plan-edit") && zoneDrafts[id]) {
    zoneDrafts[id].planMonth = shiftMonth(zoneDrafts[id].planMonth, step);
    renderPlanEditor(id);
    document.querySelector(`#cfgPlan${id} [data-plan-nav="${step}"]`)?.focus();
  } else {
    planViewMonth[id] = shiftMonth(planViewMonth[id] || monthOfYmd(rigTodayYmd()), step);
    renderPlanView(id);
    document.querySelector(`#planView${id} [data-plan-nav="${step}"]`)?.focus();
  }
});

// The plan's changes in words, for the editor's "You changed ..." line.
function planChangeWords(before, after, today) {
  const changed = [];
  for (let i = 0; i < PLAN_DAYS; i++) {
    const ymd = ymdAddDays(today, i);
    const a = before?.[ymd] || "", b = after?.[ymd] || "";
    if (a !== b) changed.push(`${ymdLabel(ymd)} ${b === "M" ? "Manual" : b === "A" ? "Automatic" : "back to the default"}`);
  }
  if (!plannedDays(after, today).length) return "the day plan, cleared";
  const shown = changed.slice(0, 6).join(", ");
  return `the day plan: ${shown}${changed.length > 6 ? ` and ${changed.length - 6} more days` : ""}`;
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
    setupsOpen: false,
    // Operation + day plan (18 Nov 2026): the plan from today on, as ESP1 has it.
    operation: s.operation || "AUTOMATIC",
    plan: Object.fromEntries(plannedDays(s.plan, rigTodayYmd()).map(ymd => [ymd, s.plan[ymd]])),
    planMonth: monthOfYmd(rigTodayYmd()),
    // Soil start and Too wet (18 Nov 2026 b): its own value, or the System tab's default.
    startDef: !s.startOwn, start: String(s.startPct),
    wetDef: !s.wetOwn, wet: String(s.wetPct)
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
  form.querySelectorAll(`input[name="cfgOps${id}"]`).forEach(radio => { radio.checked = radio.value === d.operation; });
  form.querySelectorAll(`input[name="cfgMode${id}"]`).forEach(radio => { radio.checked = radio.value === d.mode; });
  form.querySelectorAll(`input[name="cfgSched${id}"]`).forEach(radio => { radio.checked = radio.value === d.sched; });
  renderPlanEditor(id);
  const runs = document.getElementById(`cfgRuns${id}`);
  if (runs) runs.value = String(d.runs);
  for (let w = 0; w < MAX_RUNS_PER_DAY; w++) fillWindowRow(id, w);
  TARGET_FIELDS.forEach(f => { const el = document.getElementById(`${f.input}${id}`); if (el) el.value = d.targets[f.key]; });
  const name = document.getElementById(`cfgSaveName${id}`);
  if (name) name.value = d.saveName;
  syncZoneTimeInputs(id);
  syncLineInputs(id);
  renderZoneChanges(id);
}

/* ---- A column's Soil moisture lines in Edit settings (18 Nov 2026 b) ---- */
// The slider and the box of one line hold the same number.
function syncLinePair(el) {
  if (!el?.dataset?.pair || !/^(range|number)$/.test(el.type)) return;
  const other = document.getElementById(el.dataset.pair);
  if (other && other.value !== el.value) other.value = el.value;
}
// Each line shows its own value, or, while "Same as the System tab" is ticked, the default, greyed out.
function syncLineInputs(id) {
  const d = zoneDrafts[id];
  if (!d) return;
  const on = zoneThresholdsSupported();
  [["Start", d.startDef, d.start, soilStartPercent()], ["Wet", d.wetDef, d.wet, soilWetDefault()]].forEach(([key, def, own, deflt]) => {
    const box = document.getElementById(`cfg${key}${id}`);
    const range = document.getElementById(`cfg${key}Range${id}`);
    const tick = document.getElementById(`cfg${key}Def${id}`);
    const value = String(def ? deflt : own);
    if (tick) tick.checked = def;
    [box, range].forEach(el => {
      if (!el) return;
      if (el.value !== value && document.activeElement !== el) el.value = value;
      el.disabled = !on || def;
    });
  });
  renderLinePreview(id);
}
// The bar under the two lines: dry (waters) up to Soil start, good up to Too wet, too wet above, and where the soil is now.
function renderLinePreview(id) {
  const d = zoneDrafts[id];
  const bar = document.getElementById(`cfgLinePreview${id}`);
  if (!d || !bar) return;
  const pct = v => Math.min(100, Math.max(0, Number.isFinite(Number(v)) ? Number(v) : 0));
  const start = pct(d.startDef ? soilStartPercent() : d.start);
  const wet = Math.max(start, pct(d.wetDef ? soilWetDefault() : d.wet));
  bar.style.setProperty("--lp-start", `${start}%`);
  bar.style.setProperty("--lp-wet", `${wet}%`);
  const moisture = liveData.sensors?.zones?.[id]?.moisture;
  const now = bar.querySelector(".lp-now");
  if (now) { now.hidden = !hasValue(moisture); if (hasValue(moisture)) now.style.left = `${pct(moisture)}%`; }
  const legend = document.getElementById(`cfgLineLegend${id}`);
  if (legend) legend.textContent = `Waters below ${start} % · good ${start}–${wet} % · too wet above ${wet} %` +
    (hasValue(moisture) ? ` · now ${Math.round(Number(moisture))} %` : "");
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
  const operation = form.querySelector(`input[name="cfgOps${id}"]:checked`)?.value || d.operation;
  if (operation !== d.operation) { d.operation = operation; renderPlanEditor(id); }   // the default colours every unset day
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
  // Soil moisture lines: a box's own value only while its default isn't ticked (unticking starts from the default).
  [["Start", "startDef", "start"], ["Wet", "wetDef", "wet"]].forEach(([key, defKey, valKey]) => {
    const tick = document.getElementById(`cfg${key}Def${id}`);
    if (!tick || tick.disabled) return;
    if (d[defKey] && !tick.checked) d[valKey] = String(key === "Start" ? soilStartPercent() : soilWetDefault());
    d[defKey] = tick.checked;
    const box = document.getElementById(`cfg${key}${id}`);
    if (!d[defKey] && box && !box.disabled) d[valKey] = box.value.trim();
  });
  syncZoneTimeInputs(id);
  syncLineInputs(id);
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
  // Operation + day plan (18 Nov 2026), only to firmware that has them. The plan goes as a whole, from ESP1's today.
  if (planSupported()) {
    if (d.operation && d.operation !== s.operation) {
      payload.operation = d.operation;
      words.push(`operation, from ${s.operation ? OPERATION_SHORT[s.operation] : "unknown"} to ${OPERATION_SHORT[d.operation]}`);
    }
    const today = rigTodayYmd();
    const after = planTextFrom(d.plan, today);
    if (after !== planTextFrom(s.plan, today)) {
      payload.plan = after;
      payload.planFrom = today;
      words.push(planChangeWords(s.plan, d.plan, today));
    }
  }
  // Soil start and Too wet (18 Nov 2026 b), only to firmware that has them: -1 sends the column back to the default.
  if (zoneThresholdsSupported()) {
    const startOwn = !d.startDef, wetOwn = !d.wetDef;
    const startVal = Number(d.start), wetVal = Number(d.wet);
    if (startOwn && (!Number.isInteger(startVal) || startVal < 0 || startVal > 100)) problems.push("Water when drier than must be a whole number from 0 to 100.");
    else if (wetOwn && (!Number.isInteger(wetVal) || wetVal < 0 || wetVal > 100)) problems.push("Too wet must be a whole number from 0 to 100.");
    else {
      const effStart = startOwn ? startVal : soilStartPercent();
      const effWet = wetOwn ? wetVal : soilWetDefault();
      const startChanged = startOwn !== s.startOwn || (startOwn && startVal !== s.startPct);
      const wetChanged = wetOwn !== s.wetOwn || (wetOwn && wetVal !== s.wetPct);
      if ((startChanged || wetChanged) && effWet < effStart + THRESH_MIN_GAP) {
        problems.push(`Too wet (${effWet} %) must be at least ${THRESH_MIN_GAP} above Water when drier than (${effStart} %).`);
      } else {
        const said = (v, own) => `${v} %${own ? "" : " (the default)"}`;
        if (startChanged) { payload.soilStart = startOwn ? startVal : -1; words.push(`Soil start, from ${said(s.startPct, s.startOwn)} to ${said(effStart, startOwn)}`); }
        if (wetChanged) { payload.soilWet = wetOwn ? wetVal : -1; words.push(`Too wet, from ${said(s.wetPct, s.wetOwn)} to ${said(effWet, wetOwn)}`); }
      }
    }
  }
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
  // ESP1's own warning (THRESH_NO_FERT): at or under the NPK probe's floor its nutrient readings are never usable.
  if (Number(payload.soilStart) >= 0 && Number(payload.soilStart) <= NPK_MIN_MOIST &&
      !confirm(`Soil start ${payload.soilStart} % is at or under ${NPK_MIN_MOIST} %, where the NPK probe can't read the soil, so column ${id} would get water but never nutrients. Save it anyway?`)) {
    show("Nothing was sent."); return;
  }
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
    name: d.saveName.trim() || `Saved ${dateTimeText(new Date())}`,
    savedAt: firebase.database.ServerValue.TIMESTAMP,
    mode: d.mode, schedMode: d.sched,
    // winStart/winEnd stay the first time, as older saves have it; "windows" carries every one (5 Nov 2026).
    winStart: own ? d.wins[0][0] : "", winEnd: own ? d.wins[0][1] : "",
    targetN: d.targets.targetN, targetP: d.targets.targetP, targetK: d.targets.targetK, targetPH: d.targets.targetPH
  };
  if (own) snapshot.windows = d.wins.slice(0, d.runs).map(([a, b]) => ({ from: a, to: b }));
  // The Operation travels with a setup; the day plan doesn't, because its dates belong to now, not to the setup.
  if (planSupported() && d.operation) snapshot.operation = d.operation;
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
  if (chosen.operation === "AUTOMATIC" || chosen.operation === "MANUAL") d.operation = chosen.operation;
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
  // Operation + day plan only for firmware that has them (18 Nov 2026). The plan views are built fresh below.
  const planOn = planSupported();
  zoneCardsPlan = planOn;
  Object.keys(planViewKeys).forEach(id => delete planViewKeys[id]);
  Object.keys(zoneWeekKeys).forEach(id => delete zoneWeekKeys[id]);
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
    const reading = (label, valueId, noteId, infoText, help, note = "Crop profile: --", extra = "") => `
        <div class="reading${extra ? ` ${extra}` : ""}">
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

    // This week's plan and today, read-only, in place of the old "What ESP1 does with it" list (the owner's second round,
    // 18 Nov 2026): the same ids, in short words; what they mean is in the (i) icons. Changes go through Edit settings.
    const week = `
        <div class="zone-section-head">
          <h4><svg class="si" aria-hidden="true"><use href="#si-calendar"/></svg>This week${info("This week", "Each day this week, by the controller's own date: A is Automatic (it waters at its watering times when the soil is dry), M is Manual (no scheduled watering that day). Today has a ring. Change the days in Edit settings, Day plan.", "help-operation")}</h4>
        </div>
        <ol class="zone-week" id="zoneWeek${id}" aria-label="Column ${id}: this week"></ol>
        <p class="zone-week-note" id="zoneWeekNote${id}"></p>
        <div class="zone-today-box">
          <p class="zone-today-line"><strong>Today</strong><span id="zoneWhen${id}" title="Watering times">Unavailable</span><span class="zt-dot" aria-hidden="true">·</span><span id="zoneToday${id}">Unavailable</span></p>
          <p class="zone-today-line zt-sub"><span id="zoneDoes${id}" title="What it does">Unavailable</span>${info("What it does", "Water + nutrients: the controller adds nutrients when the soil is short of them, at most once a day. Water only: it never adds any.", "help-what-it-does")}<span class="zt-dot" aria-hidden="true">·</span><span id="zoneTargets${id}" title="Nutrient targets">Unavailable</span>${info("Targets", "The N, P and K levels the controller aims for, in ppm, and the pH. It adds nutrients when a reading falls short of one by the Fertigation gap. pH is recorded, not corrected.", "help-npk")}</p>
        </div>`;
    const summary = `
        <div class="zone-ops-row">
          <span class="zone-ops-label">Operation${info("Operation", "Automatic: the controller waters this column at its watering times, when the soil is dry. Manual: readings only, no scheduled watering; you water it with Water a column now. The day plan changes single days. Both need ESP1's firmware of 18 Nov 2026; before it, every day is Automatic.", "help-operation")}</span>
          <span class="zone-ops-pill" id="zoneOps${id}" data-op="none">Unavailable</span>
        </div>
        ${canEdit ? `<button type="button" id="zoneEdit${id}" class="secondary zone-edit-btn"><svg class="si" aria-hidden="true"><use href="#si-gear"/></svg>Edit settings</button>`
                  : `<p class="field-note">Read-only. Changing a column's settings needs access to edit zone profiles${isApprovedUser() && !isOperator() ? ", which you can ask for at the top of this tab" : ""}.</p>`}`;

    // Soil start and Too wet for this column (18 Nov 2026, the owner): a slider and a box each, or the System tab's default.
    const linesOn = zoneThresholdsSupported();
    const lineRow = (key, name, min) => `
            <div class="line-row">
              <label class="line-name" for="cfg${key}${id}">${name}</label>
              <input type="range" class="line-range line-${key.toLowerCase()}" id="cfg${key}Range${id}" min="${min}" max="100" step="1" data-pair="cfg${key}${id}" aria-label="${name}, column ${id}"${linesOn ? "" : " disabled"}>
              <span class="line-num"><input type="number" id="cfg${key}${id}" min="${min}" max="100" step="1" inputmode="numeric" data-pair="cfg${key}Range${id}"${linesOn ? "" : " disabled"}><span aria-hidden="true">%</span></span>
              <label class="line-def"><input type="checkbox" id="cfg${key}Def${id}"${linesOn ? "" : " disabled"}>Same as the System tab</label>
            </div>`;
    const lines = `
          <fieldset class="zone-lines">
            <legend>Soil moisture lines${info("Soil moisture lines", `Water when drier than (Soil start): the controller waters this column at its watering times only when its soil is drier than this. Too wet above: wetter than this, the card says Too wet; it is a warning only. Same as the System tab follows the defaults there. Too wet must be at least ${THRESH_MIN_GAP} above Soil start.`, "help-soil-start")}</legend>
            ${lineRow("Start", "Water when drier than", 0)}
            ${lineRow("Wet", "Too wet above", THRESH_MIN_GAP)}
            <div class="line-preview" id="cfgLinePreview${id}" aria-hidden="true"><span class="lp-dry"></span><span class="lp-ok"></span><span class="lp-wet"></span><i class="lp-now"></i></div>
            <p class="line-legend" id="cfgLineLegend${id}"></p>
            ${linesOn ? "" : `<p class="plan-wait">A column's own lines need ESP1's firmware of 18 Nov 2026 (b). Until the rig runs it, every column uses the System tab's Soil start, and Save to rig leaves these out.</p>`}
          </fieldset>`;

    const form = `
        <form id="zoneForm${id}" class="zone-form" novalidate>
          <fieldset class="zone-ops-field">
            <legend>Operation${info("Operation", "Automatic: the controller waters this column at its watering times when the soil is dry. Manual: readings only, no scheduled watering; you water or feed it yourself with Water a column now. This is the default for every day; the day plan below changes single days.", "help-operation")}</legend>
            <div class="seg-switch">
              <label class="seg-opt"><input type="radio" name="cfgOps${id}" value="AUTOMATIC"${planOn ? "" : " disabled"}><span>Automatic</span></label>
              <label class="seg-opt"><input type="radio" name="cfgOps${id}" value="MANUAL"${planOn ? "" : " disabled"}><span>Manual</span></label>
            </div>
            ${planOn ? "" : `<p class="plan-wait">ESP1 hasn't reported Manual or Automatic yet: it needs its firmware of 18 Nov 2026. Until it runs that, these stay off and Save to rig leaves them out; the rest of this form works as usual.</p>`}
          </fieldset>
          <fieldset class="plan-fieldset">
            <legend>Day plan${info("Day plan", "Click a day to set it for that date only: once for Manual, twice for Automatic, a third time to go back to the default above. Up to about two months ahead, by ESP1's own clock.", "help-operation")}</legend>
            <div id="cfgPlan${id}" class="plan-edit"></div>
            <button type="button" id="cfgPlanClear${id}" class="secondary small"${planOn ? "" : " disabled"}>Clear the day plan</button>
          </fieldset>
          <fieldset>
            <legend>What it does${info("What it does", "Water + nutrients lets ESP1 add nutrients when the soil is short of them. Water only never adds any.", "help-what-it-does")}</legend>
            <label class="choice"><input type="radio" name="cfgMode${id}" value="AUTO">Water + nutrients when the soil needs them</label>
            <label class="choice"><input type="radio" name="cfgMode${id}" value="IRRIGATION_ONLY">Water only</label>
          </fieldset>
          <fieldset>
            <legend>When it runs${info("When it runs", multi
              ? "Rig default uses the one window built into the firmware. My own time uses up to 4 times you pick, in order through the day, each ending the same day."
              : "Rig default uses the window built into the firmware. My own time uses the start and end you pick, and must end the same day.", "help-set-time")}</legend>
            <label class="choice"><input type="radio" name="cfgSched${id}" value="0">Rig default, ${clockSpan(defStart, defEnd)}, once a day</label>
            <label class="choice"><input type="radio" name="cfgSched${id}" value="1">My own time</label>
            ${multi ? `<label class="runs-pick">Waterings a day<select id="cfgRuns${id}">${ORDINALS.map((_, w) => `<option value="${w + 1}">${w + 1}</option>`).join("")}</select></label>` : ""}
            ${Array.from({ length: multi ? MAX_RUNS_PER_DAY : 1 }, (_, w) => `
            <div class="time-row" id="cfgWinRow${id}${w + 1}" role="group" aria-label="${multi ? `${ORDINALS[w]} watering time` : "Watering time"}">
              ${multi ? `<span class="time-row-label" aria-hidden="true">${ORDINALS[w]}</span>` : ""}
              <label>From<input id="${winInputId("cfgWinStart", id, w)}" type="time"></label>
              <label>To<input id="${winInputId("cfgWinEnd", id, w)}" type="time"></label>
            </div>`).join("")}
            <p class="field-note">${multi
              ? "At most once in each time, and only when the soil is drier than its Soil start. Nutrients go in at most once a day: later waterings that day are plain water. Times follow the rig's clock."
              : "At most once a day, inside this time, when the soil is drier than its Soil start. Times follow the rig's clock."}</p>
          </fieldset>
          ${lines}
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
      : `<div class="zone-name-row"><h3 class="zone-name">${escapeHtml(zone.name)}</h3>${canEdit ? `<button type="button" class="inline-link" id="zoneRename${id}">Rename</button>` : ""}</div>`;
    // The zone's own name on top in large letters, the column under it (the owner's second round, 18 Nov 2026).
    const hidden = Boolean(zoneReadingsHidden[id]);
    block.innerHTML = `
      <div class="zone-art" aria-hidden="true">${zoneArtSvg(id)}</div>
      <header class="zone-head">
        <div class="zone-title">
          ${nameLine}
          <p class="zone-col"><span class="zone-col-tag">Column ${id}</span><span class="zone-crop-line" id="zoneCropLine${id}"></span></p>
        </div>
        ${power}
      </header>
      ${zoneHeroHtml(id)}
      <div class="zone-body">
        <section class="zone-now" aria-label="${escapeHtml(zone.name)} (column ${id}): readings">
          <div class="zone-section-head">
            <h4><svg class="si" aria-hidden="true"><use href="#si-beaker"/></svg>Readings</h4>
            <button type="button" class="zone-fold" data-z-readings="${id}" aria-expanded="${!hidden}" aria-controls="zoneReadings${id}">${hidden ? "Show" : "Hide"}</button>
          </div>
          <p class="zone-off-note" id="zoneOffNote${id}" hidden>This column is off. ESP1 isn't watering it, and isn't sending its readings. Its settings are kept.</p>
          <dl class="reading-grid" id="zoneReadings${id}">
            <div class="reading reading-gauge complete-only">
              <dt>Soil moisture${info("Soil moisture", "The column's main moisture figure. ESP1 waters it when this is below its Soil start, the marked line on the gauge. The other mark is what the crop profile wants.", "help-moisture")}</dt>
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
            ${reading("NPK probe moisture", `npkMoist${id}`, "", "The NPK probe's own moisture reading. It counts toward Soil moisture when the two agree.", "help-moisture", "Counts toward soil moisture when it agrees", "complete-only")}
            ${reading("Soil temperature", `soilTemp${id}`, "", "Root-zone temperature, from the 7-in-1 probe.", "", "Root zone")}
          </dl>
          ${compare}
        </section>
        <section class="zone-does" aria-label="${escapeHtml(zone.name)} (column ${id}): this week and its settings">
          ${week}
          <div class="plan-view complete-only" id="planView${id}" hidden></div>
          ${draft ? form : summary}
          <p id="cfgResult${id}" class="control-result" aria-live="polite"></p>
        </section>
      </div>`;
    block.classList.toggle("readings-hidden", hidden);
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
        // A Soil moisture line's slider and box move together; "Same as the System tab" puts the default in both.
        editor.addEventListener("input", event => { syncLinePair(event.target); readZoneForm(id); });
        editor.addEventListener("change", event => { syncLinePair(event.target); readZoneForm(id); });
        editor.addEventListener("submit", event => { event.preventDefault(); saveZoneEditor(id); });
        block.querySelector(`#cfgCancel${id}`)?.addEventListener("click", () => closeZoneEditor(id));
        block.querySelector(`#cfgFromCrop${id}`)?.addEventListener("click", () => fillTargetsFromCrop(zone));
        block.querySelector(`#cfgSaveSettingsBtn${id}`)?.addEventListener("click", () => saveColumnSettings(id));
        block.querySelector(`#cfgPlanClear${id}`)?.addEventListener("click", () => {
          if (!zoneDrafts[id]) return;
          zoneDrafts[id].plan = {};
          renderPlanEditor(id);
          renderZoneChanges(id);
        });
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
    renderSimpleZone(zone);              // Simple mode's big figure and status (18 Nov 2026)
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
  const start = zoneStartPercent(zone.id);          // its own Soil start since 18 Nov 2026 (b), or the default
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
  const reported = hasValue(liveData.system?.soilStartPct) || hasValue(z.soilStartPct);
  setText(`soilStartNote${zone.id}`, `Waters below ${start} %${reported || start === 35 ? "" : " (as last sent)"}`);
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
  syncCropFold();                        // Simple mode: open while an editor is, folded otherwise
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

/* ESP1's reservoir floor (RES_LOW_PCT, = RES_FILTER_PCT, 18 %): where its sensor stops seeing water. Since ESP1's firmware of
 * 18 Nov 2026 (b) the pause can be raised from here (resPausePercent()); it can never go lower. The old rig drawing
 * that lived here was replaced by the system picture shared by both modes (buildSimpleDiagram()). */
const RIG_RES_LOW_PCT = 18;

// ESP1 doesn't report its Soil start %, so this is the last value sent from this website (in the commands still
// listed), or the firmware's default.
function soilStartPercent() {
  // ESP1 from 18 Nov 2026 reports its own value; before that, the last one this page sent, or the firmware's default.
  if (hasValue(liveData.system?.soilStartPct)) return Number(liveData.system.soilStartPct);
  const sent = commandData
    .filter(c => c.type === "SET_THRESH" && c.status === "completed" && hasValue(c.payload?.thStart))
    .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))[0];
  return sent ? Number(sent.payload.thStart) : 35;
}

/* Watering lines (18 Nov 2026, the owner): Soil start and a Too wet line per column, each with a default on the System
 * tab, and the reservoir pause. ESP1 from its firmware of 18 Nov 2026 (b) keeps them all and reports them:
 * system.soilWetPct / resPausePct, and zones.<id>.soilStartPct / soilWetPct as they apply (+ soilStartOwn / soilWetOwn
 * when the column has its own). Too wet only warns. Older firmware: one Soil start for all, no Too wet, a fixed 18 %. */
const SOIL_WET_DEFAULT = 90;       // SOIL_WET_DEFAULT_PCT in ESP1's pure_math.h
const THRESH_MIN_GAP = 5;          // THRESH_MIN_GAP_PCT: Too wet sits at least this far above Soil start
const NPK_MIN_MOIST = 15;          // NPK_MIN_MOIST_PCT: at or under this the NPK probe's N/P/K can't be trusted
const RES_PAUSE_MAX = 50;          // RES_PAUSE_MAX_PCT: the highest reservoir pause
function soilWetDefault() {
  return hasValue(liveData.system?.soilWetPct) ? Number(liveData.system.soilWetPct) : SOIL_WET_DEFAULT;
}
// The Too wet line that applies with a given Soil start (ESP1's wetEffPct()).
function wetEffPercent(wet, start) { return Math.min(100, Math.max(Number(wet), Number(start) + THRESH_MIN_GAP)); }
function thresholdsSupported() { return hasValue(liveData.system?.soilWetPct) && hasValue(liveData.system?.resPausePct); }
function zoneThresholdsSupported() { return Object.values(liveData.sensors?.zones || {}).some(z => hasValue(z?.soilStartPct)); }
// Below this no run starts (ESP1 reports 18 to 50 %; older firmware is fixed at 18 %).
function resPausePercent() {
  const v = Number(liveData.system?.resPausePct);
  return Number.isFinite(v) && v >= RIG_RES_LOW_PCT && v <= RES_PAUSE_MAX ? v : RIG_RES_LOW_PCT;
}
function zoneStartPercent(id) { return zoneSettings(id).startPct; }

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
  setPumpLamp("transferPumpStatus", fresh && Boolean(actuators.transferRunning), fresh);
  setPumpLamp("boosterPumpStatus", fresh && Boolean(actuators.boosterRunning), fresh);
  setPumpLamp("mixerPumpStatus", fresh && Boolean(actuators.mixerRunning), fresh);
  // The column editor offers several watering times a day only to firmware that has them, and the first snapshot
  // usually lands after the cards are built.
  // The same for the Operation and day plan, from ESP1 from 18 Nov 2026.
  if (multiRunSupported() !== zoneCardsMultiRun || planSupported() !== zoneCardsPlan) renderZonesUI();
  updateZoneReadings();

  renderDiagnostics();
  renderRawSensors();
  renderFlowMeters();
  renderExercise();
  renderManualHold();
  updateFaultBanner();
  renderTankCheck();
  updateForceArmed();
  renderUpNext();
  updateSystemTab();
  syncControlAvailability();
  renderSimpleDashboard();               // Simple mode's dashboard, from the same report
}

// On an old report the lamps say UNKNOWN, never OFF (18 Nov 2026): a stale snapshot must not read as "definitely idle".
function setPumpLamp(id, on, fresh = true) {
  if (!fresh && liveData.meta?.updatedAt) { setDeviceStatus(id, "UNKNOWN", "off"); return; }
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
  syncForceOffColumns();
}

// The columns ESP1 reports switched off, each with a way to switch it on from here (18 Nov 2026). The LCD's Force Run
// switches a chosen OFF column on by itself; here it is a separate, confirmed step, because switching a column on also
// restarts its schedule. Needs zone editing, the same as the column's own switch.
let forceOffKey = "";
function syncForceOffColumns() {
  const box = document.getElementById("forceOffCols");
  if (!box) return;
  const off = ["A", "B", "C"].filter(id => liveData.sensors?.zones?.[id]?.enabled === false);
  const canSwitch = hasZoneEditAccess();
  const key = `${off.join("")}|${canSwitch}`;
  if (key === forceOffKey) return;
  forceOffKey = key;
  box.hidden = !off.length;
  if (!off.length) { box.innerHTML = ""; return; }
  const names = off.length === 1 ? `Column ${off[0]} is` : `Columns ${off.join(" and ")} are`;
  box.innerHTML = `<p>${names} switched off on ESP1, so ${off.length === 1 ? "it" : "they"} can't be picked.` +
    (canSwitch ? " Switch one on here, then pick it once ESP1 confirms (a few seconds)."
               : " Switching a column on needs access to edit zone profiles.") + "</p>" +
    (canSwitch ? `<div class="force-off-actions">${off.map(id => `<button type="button" class="secondary small" data-force-enable="${id}">Switch column ${id} on</button>`).join("")}</div>` : "");
}

document.getElementById("forceOffCols")?.addEventListener("click", event => {
  const button = event.target.closest("button[data-force-enable]");
  if (!button) return;
  const id = button.dataset.forceEnable;
  const result = resultLine("forceRunResult");
  if (!confirmEnableColumn(id)) { result("Nothing was sent."); return; }
  sendFromPanel(result, "SET_COLUMN", { col: id, enabled: 1 }).then(key => {
    if (!key) return;
    zonePowerSent[id] = key;            // the column's own switch on Zone profiles shows "Sending…" too
    logActivity(`Switched column ${id} on (for a forced run)`);
  });
});

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
// Why ESP1 last started (system.bootReason, esp_reset_reason() in its firmware of 18 Nov 2026), in words.
const BOOT_REASON_TEXT = {
  POWER_ON: "a power loss or switch-on", BROWNOUT: "a supply dip (brown-out)", SOFTWARE: "a restart by itself or on request",
  PANIC: "a crash", TASK_WDT: "the watchdog (a stuck task)", INT_WDT: "the watchdog (interrupts)", WDT: "the watchdog",
  EXT_PIN: "the reset button", DEEPSLEEP: "waking from sleep", OTHER: "an unknown cause"
};
function bootText() {
  const s = liveData.system || {};
  if (!s.bootReason) return "Not reported by this firmware";
  const words = BOOT_REASON_TEXT[s.bootReason] || s.bootReason;
  // uptimeS is as of the report; the report's own age is added so it keeps counting between reports.
  const up = hasValue(s.uptimeS)
    ? Number(s.uptimeS) * 1000 + Math.max(0, serverNow() - Number(liveData.meta?.updatedAt || serverNow())) : null;
  return `${up !== null ? `${formatAge(up)} ago, after ` : "After "}${words} (${s.bootReason})`;
}

// The calibration ESP1 reports (diagnostics.calibration, firmware of 18 Nov 2026), as Diagnostics card groups.
function calibrationGroups(c) {
  const num = (v, d, unit) => hasValue(v) ? numberText(v, d, unit) : "Unavailable";
  const r = c.rates || {}, k = c.flowK || {}, t = c.tanks || {}, p = c.probes || {};
  // A pump's line: litres a minute plus what it moves at the start; 0 = never measured, so ESP1 uses its built-in line.
  const line = (rate, start) => Number(rate) > 0
    ? `${numberText(rate, 2, "L/min")}${hasValue(start) ? `, ${numberText(start, 2, "L")} at the start` : ""}`
    : "Not measured: the built-in line";
  const nut = i => hasValue(r.nutMlpm?.[i]) ? `${numberText(r.nutMlpm[i], 1, "mL/min")}${r.nutMeasured?.[i] ? "" : " (default)"}` : "Unavailable";
  return [
    ["Calibration: pumps (set on the LCD)", [["Transfer pump", line(r.transferLpm, r.transferStartL)], ["Booster pump", line(r.boosterLpm, r.boosterStartL)],
      ["Nutrient A pump", nut(0)], ["Nutrient B pump", nut(1)], ["Nutrient C pump", nut(2)]]],
    ["Calibration: flow meters (pulses per litre)", [["Nutrient A", num(k.nutA, 0)], ["Nutrient B", num(k.nutB, 0)], ["Nutrient C", num(k.nutC, 0)],
      ["Nutrient D", num(k.nutD, 0)], ["pH up", num(k.phUp, 0)], ["pH down", num(k.phDn, 0)], ["Reservoir refill scale", num(k.reservoirScale, 3)]]],
    ["Calibration: tanks and probes", [["Reservoir sensor, empty / full", `${num(t.resEmptyCm, 1, "cm")} / ${num(t.resFullCm, 1, "cm")}`],
      ["Mixing tank sensor, empty / full", `${num(t.mixEmptyCm, 1, "cm")} / ${num(t.mixFullCm, 1, "cm")}`],
      ["pH probe (slope, offset)", `${num(p.phM, 6)}, ${num(p.phB, 3)}`], ["EC probe (slope, offset)", `${num(p.ecM, 6)}, ${num(p.ecB, 3)}`],
      ["Mixer current sensor zero", num(p.acs712Zero, 3, "V")],
      ["Air offsets (temp, humidity, light)", `${num(p.tempOff, 1, "°C")}, ${num(p.humOff, 1, "%")}, ${num(p.luxOff, 0, "lux")}`]]]
  ];
}

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
  // ESP1 from 18 Nov 2026: the tank check, and how often it has started today.
  if (d.tankCheck?.needed === true) add("bad", "The mixing tank must be checked: ESP1 starts no scheduled run until someone confirms it. Use the banner at the top of the page.");
  const boots = Number(liveData.system?.bootsToday || 0);
  if (boots >= 3) {
    const reason = liveData.system?.bootReason;
    const power = reason === "POWER_ON" || reason === "BROWNOUT";
    add("warn", `ESP1 has started ${boots} times today; the last start was ${BOOT_REASON_TEXT[reason] || "for an unknown reason"}.` +
      (power ? " Repeated power restarts point at its supply: give ESP1 and the SMS modem a steady 5 V supply of at least 2 A, with a large capacitor at the modem." : ""));
  }
  if (d.gsm?.simReady === false) add("warn", "The SIM card isn't ready, so SMS alerts can't be sent.");
  else if (d.gsm?.networkRegistered === false) add("warn", "The SMS modem isn't on the mobile network, so SMS alerts can't be sent right now.");
  if (Number(d.firebase?.attempts || 0) > 0 && d.firebase?.lastUploadOk === false) add("warn", "ESP1's last upload to this website failed. It retries by itself.");
  if (d.thingspeak?.configured && d.thingspeak?.attempted && d.thingspeak?.lastUploadOk === false) add("warn", "ESP1's last upload to the ThingSpeak graphs failed.");
  if (!items.length) add("ok", "Everything ESP1 checks looks fine.");
  const lastFault = d.system?.lastFault;
  if (lastFault && lastFault !== "none" && !f.held) {
    const words = faultInfoText(lastFault);
    add("", `The last fault ESP1 recorded was ${lastFault}${d.system?.lastFaultTime ? ` at ${rigTimeText(d.system.lastFaultTime)}` : ""}${words ? ` (${words.replace(/\.$/, "")})` : ""}. It is over; it stays here as a record.`);
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

// What a Wi-Fi signal means for ESP1's reports (18 Nov 2026; dBm, nearer 0 is stronger). The secure uploads start to
// fail below about -70; on 17 Nov the rig sat at -75 to -90 and about one upload in three failed some mornings.
function wifiQualityText(rssi) {
  const v = Number(rssi);
  if (!Number.isFinite(v) || v >= 0) return "";
  if (v >= -60) return " (excellent)";
  if (v >= -67) return " (good)";
  if (v >= -72) return " (fair: reports may sometimes be late)";
  if (v >= -80) return " (weak: reports will often be late or fail)";
  return " (very weak: expect drop-outs)";
}

// ESP1's link health (firmware of 18 Nov 2026 c): reports sent and failed, new TLS sessions (each one a pause of a second
// or two) and its free memory, all since its last start. "Not reported" on older firmware.
function linkHealthRows(fb) {
  if (!fb || !hasValue(fb.failures)) return [["Link health", "Not reported by this firmware (needs web v.4 of 18 Nov 2026 c)"]];
  const attempts = Number(fb.attempts || 0), failures = Number(fb.failures || 0);
  const failedPct = attempts > 0 ? ` (${(failures * 100 / attempts).toFixed(failures * 100 / attempts < 10 ? 1 : 0)}% failed)` : "";
  const kb = value => `${Math.round(Number(value) / 1024)} KB`;
  return [
    ["Reports since start", attempts > 0 ? `${attempts - failures} of ${attempts} sent${failedPct}` : "None yet"],
    ["New connections since start", hasValue(fb.handshakes) ? String(fb.handshakes) : "Unavailable"],
    // Free memory: since web v.4 of 18 Nov 2026 h the memory text and buffers can use (ESP1 needs ~24 KB, with one 17 KB
    // block, to open a connection). Before it the figure included ~40 KB of code memory: "largest block" read 40 KB always.
    ["Free memory", hasValue(fb.freeHeap) ? `${kb(fb.freeHeap)}${hasValue(fb.maxAllocHeap) ? `, largest block ${kb(fb.maxAllocHeap)}` : ""}${hasValue(fb.tlsSkips) ? "" : " (counts code memory on this firmware)"}` : "Unavailable"],
    // Recovery (web v.4 of 18 Nov 2026 e): ESP1 starts a fresh connection after failed reports, and rejoins Wi-Fi after more.
    ...(hasValue(fb.linkResets) ? [["Recoveries since start",
      `${fb.linkResets} fresh connection${Number(fb.linkResets) === 1 ? "" : "s"}, ${fb.wifiRejoins || 0} Wi-Fi rejoin${Number(fb.wifiRejoins) === 1 ? "" : "s"}`]] : []),
    // 18 Nov 2026 h: connections not tried because memory was short, and reports whose text was cleaned of noise bytes.
    ...(hasValue(fb.tlsSkips) ? [["Waited for memory", `${fb.tlsSkips} connection${Number(fb.tlsSkips) === 1 ? "" : "s"}`]] : []),
    ...(Number(fb.textFixes) > 0 ? [["Reports with noise cleaned", String(fb.textFixes)]] : [])
  ];
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
    ["ESP1's report", [["Report", current ? "Current" : "Stale or unavailable"], ["Received", snapshotAgeText()], ["ESP1 state", liveData.system?.state || "Unavailable"], ["Firmware", !liveData.system ? "Unavailable" : liveData.system.firmware ? String(liveData.system.firmware) : isV3Firmware() ? "web v.3 (or web v.4 before 17 Nov)" : "Older than web v.3"], ["Report size", liveData.meta?.docUsed ? `${liveData.meta.docUsed} / ${liveData.meta.docCapacity} B` : "Unavailable"], ["Last start", bootText()], ["Starts today", hasValue(liveData.system?.bootsToday) ? String(liveData.system.bootsToday) : "Not reported by this firmware"]]],
    ["Wi-Fi", [["Wi-Fi enabled", booleanText(d.network?.wifiEnabled, "Enabled", "Disabled")], ["Wi-Fi link", booleanText(d.network?.wifiConnected, "Connected", "Disconnected")], ["Signal strength", hasValue(d.network?.wifiRssi) ? `${d.network.wifiRssi} dBm${wifiQualityText(d.network.wifiRssi)}` : "Unavailable"]]],
    ["Link to this website (Firebase)", [["Enabled", booleanText(d.firebase?.enabled, "Enabled", "Disabled")], ["RTDB URL", booleanText(d.firebase?.urlConfigured, "Configured", "Not configured")], ["Device account", booleanText(d.firebase?.deviceCredentialsConfigured, "Configured", "Not configured")], ["Signed in", booleanText(d.firebase?.signedIn, "Yes", "No")], ["Last upload", Number(d.firebase?.attempts || 0) > 0 ? booleanText(d.firebase?.lastUploadOk, "OK", "Failed") : "Not attempted"], ["Last HTTP", Number(d.firebase?.attempts || 0) > 0 ? rawText(d.firebase?.lastHttp) : "Not attempted"], ["TLS validation", booleanText(d.firebase?.tlsValidationEnabled, "Enabled", "Disabled")], ["Last auth issue", rawText(d.firebase?.lastAuthError, "None")], ...linkHealthRows(d.firebase)]],
    ["ThingSpeak graphs", [["Configured", booleanText(d.thingspeak?.configured, "Configured", "Not configured")], ["Last upload", d.thingspeak?.attempted ? booleanText(d.thingspeak?.lastUploadOk, "OK", "Failed") : "Not attempted"]]],
    ["Log upload", [["Goes to", d.logUpload?.target === "firebase" ? "Firebase (SD card logs, below)" : d.logUpload?.target === "supabase" || (!d.logUpload && d.supabase?.configured) ? "Supabase" : "Not configured"], ["Upload", booleanText(d.logUpload ? d.logUpload.busy : d.supabase?.uploadBusy, "In progress", "Idle")], ["Last uploaded day", Number(d.logUpload?.lastUploadedDay || d.supabase?.lastUploadedDay || 0) > 0 ? rawText(d.logUpload?.lastUploadedDay || d.supabase?.lastUploadedDay) : "No completed upload"]]],
    ["Runs and faults", [["Work order", booleanText(d.system?.workOrderActive, "Active", "Inactive")], ["Run waiting to start", booleanText(d.system?.pendingRun, "Yes", "No")], ["Last fault", rawText(d.system?.lastFault === "none" ? "" : d.system?.lastFault, "None") + (explainFaultCode(d.system?.lastFault) ? ` — ${explainFaultCode(d.system?.lastFault)}` : "")], ["Fault time", rigTimeText(rawText(d.system?.lastFaultTime, "None"))], ["Rig lock", booleanText(d.system?.lcdLocked, "Locked", "Unlocked")], ["Tank check", !d.tankCheck ? "Not reported by this firmware" : d.tankCheck.needed ? "Needed: scheduled runs wait" : "Not needed"]]],
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
    // What the LCD's Calibration menu has set (ESP1 from 18 Nov 2026), read-only: calibrating needs someone at the rig.
    if (d.calibration) calibrationGroups(d.calibration).forEach(([title, rows]) => container.appendChild(diagnosticGroup(title, rows)));
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
    meta.textContent = `${rigTimeText(rawText(event.at, "time unavailable"))} · ${rawText(event.source)} · ${rawText(event.type)}`;
    detail.textContent = rawText(event.detail);
    row.append(meta, detail);
    // A fault in words, from ESP1's own alert table (18 Nov 2026). Only for faults: other events carry codes too.
    if (/^(FAULT|SMS_ERR|REJECT)$/i.test(String(event.type || ""))) {
      const words = faultInfoText(event.detail);
      if (words) {
        const explain = document.createElement("p");
        explain.className = "event-explain";
        explain.textContent = words;
        row.appendChild(explain);
      }
    }
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
  return dateTimeText(value);
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
// BATT_SWITCH_FAIL / INVERTER_FAIL: held like SERVO_FAIL since 2026-11-16 (ESP1 main.cpp, the "resp ==
// BATT_SWITCH_FAIL || INVERTER_FAIL" branch -> enterFaultHold(resp, ...)).
const HELD_FAULT_CODE = /^(ESP2_ERROR|FLOW_FAIL|PWR_FAIL|SAFE_STOP|DOSE_TIMEOUT|LEVEL_BLIND|SERVO_FAIL|BATT_SWITCH_FAIL|INVERTER_FAIL)\b/;
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
  // ESP2 moves the boards to the secondary battery (P0) before every AC pump start and checks that all of them came
  // through the switch; the reason after the "|" says which check failed. The inverter is never switched on.
  if (/^BATT_SWITCH_FAIL\b/.test(text)) {
    const why = /NANO_RESET/.test(text) ? " The Nano restarted during the switch."
      : /NANO_SILENT/.test(text) ? " The Nano stopped answering during the switch."
      : /PCF_RESET/.test(text) ? " The relay board (PCF8575) restarted during the switch."
      : /NO_CONFIRM|TIMEOUT/.test(text) ? " ESP1 never confirmed that every board came through the switch." : "";
    return `Moving the boards to the secondary battery failed, so the inverter was never switched on.${why} Check the secondary battery's charge, the battery-switch relay and its wiring.`;
  }
  // The inverter relay was on for 6 s and the PZEM read no AC voltage on its output; ESP2 switched the relay off again.
  if (/^INVERTER_FAIL\b/.test(text)) {
    return /v=none/.test(text)
      ? "The inverter was switched on but the PZEM power meter did not answer at all, so its output could not be confirmed. ESP2 switched the inverter off again. Check the PZEM and its wiring, then the inverter."
      : "The inverter was switched on but gave no AC voltage within 6 seconds, so ESP2 switched it off again. Check the inverter (its own switch, fuse and battery input) and its relay.";
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
  // Every other code: ESP1's own headline and advice (18 Nov 2026), so nothing shows as a bare code.
  return faultInfoText(text);
}

/* ESP1's alert table (SMS_FAULTS in its main.cpp, web v.4 of 17 Nov 2026), word for word: each fault code's headline
 * and what to do, the same words its SMS alerts use. Added 18 Nov 2026: 26 of these 52 codes had no words anywhere on
 * this page and showed as bare codes. Keep in step with the firmware's table when a code is added there. */
const FAULT_INFO = {
  FLOW_FAIL:            ["Tank level not moving, pump stopped", "Check pump, valves, tank sensors"],
  ESP2_ERROR:           ["Pump controller stopped the run", "Check the pump controller board"],
  LEVEL_BLIND:          ["Tank level sensor not reading", "Check the mixing tank ultrasonic"],
  SERVO_FAIL:           ["Pump selector failed or stuck", "Check the servo and its switch"],
  BATT_SWITCH_FAIL:     ["Switch to backup battery failed", "Check the backup battery and relay"],
  INVERTER_FAIL:        ["Inverter gave no AC power", "Check the inverter and its wiring"],
  PWR_FAIL:             ["Pump power fault, pump stopped", "Check pump, wiring and AC supply"],
  SAFE_STOP:            ["Safety stop", "Check the mixer and pumps"],
  DOSE_TIMEOUT:         ["Nutrient pump ran too long", "Prime the nutrient line"],
  MIX_OVERFLOW:         ["Mixing tank overfilling", "Pumps stopped. Check tank, clear on LCD"],
  ESP2_DONE_TIMEOUT:    ["Pump controller never finished", "All stopped. Check tank, clear on LCD"],
  BATTERY_CRITICAL:     ["Main battery critically low", "All stopped. Charge it, clear on LCD"],
  ESTOP_KEPT:           ["Emergency stop kept after restart", "Clear it on the LCD to resume"],
  ESP2_RESTARTED:       ["Pump controller restarted", ""],
  ESP2_SILENCE:         ["Pump controller not answering", "Restarting it automatically"],
  ESP2_COMM_LOST:       ["Lost link to pump controller", "Retrying every 20 min. Check wiring"],
  ESP2_NO_READY:        ["Pump controller did not start", "Check its power and wiring"],
  ESP2_DEGRADED:        ["Pump controller disabled a part", "Run continues. Check it after"],
  PCF_FAIL:             ["Relay board not responding", "Check the relay board wiring"],
  COL_MISMATCH:         ["Column is off on pump controller", "Skipped today. Match column settings"],
  NANO_GARBAGE:         ["Sensor board sending bad data", "Check the sensor board wiring"],
  NANO_RESET:           ["Sensor board was restarted", ""],
  NANO_SILENCE:         ["Sensor board not answering", "Check its power and wiring"],
  SELF_RESET:           ["Main controller restarting itself", "Max once a day. Check if it repeats"],
  NET_STALL:            ["Internet task stuck", "Website paused. Watering not affected"],
  RTC_MISSING:          ["Clock module not found", ""],
  RTC_INVALID:          ["Clock time is wrong", ""],
  SERVO_MISMATCH:       ["Wrong pump ran, retrying", "Check the pump selector servo"],
  LEVEL_HIGH:           ["Mixing tank full before target", "Run goes on. Check tank litres cal"],
  RES_EMPTY:            ["Water reservoir ran dry", "Refill it. Run went on with less water"],
  RES_LOW:              ["Water reservoir is low", "Refill the reservoir"],
  TANK_CHECK:           ["Check the mixing tank", "Runs wait. Then text TANKCHECK,OK"],
  EC_FAIL:              ["Mixed batch EC out of range", "Batch still given. Check dosing"],
  PH_FAIL:              ["Mixed batch pH out of range", "Batch still given. Check pH probe"],
  DOSE_BATCH_LOW:       ["Batch too small to dose", "Watering only. Check volume settings"],
  DOSE_CARRY:           ["Dosed batch went to other column", ""],
  THRESH_NO_FERT:       ["Soil start % too low to fertilize", "Only watering will run. Raise it"],
  RESUME_NO_REPLY:      ["No answer to your resume", "Still on hold. Check link, try again"],
  WO_OVERSIZE:          ["Run command too long, not sent", "Run skipped. Check column settings"],
  FORCE_ABORT:          ["Manual run cancelled", ""],
  MANUAL_REVOKED:       ["Website control ended", "The operator at the rig took over"],
  SENSOR_FAIL:          ["Sensor not working", "Check its wiring"],
  SOIL_MISSING:         ["Soil moisture reading missing", "Check that column's soil probes"],
  NPK_FAULT:            ["NPK sensor not reading", "Watering only. Check NPK sensor"],
  BATTERY_LOW:          ["Main battery low", "Check charging and the solar panel"],
  BATT2_LOW:            ["Backup battery low", "AC pumps blocked until it charges"],
  BATT2_CRIT:           ["Backup battery critically low", "Charge the backup battery"],
  BATT_SWITCH_RESET:    ["Battery switch reset the boards", ""],
  BATT_SWITCH_FAULT:    ["Battery switching locked", "AC pumps blocked. Clear on LCD, web or text BATTSW,CLEAR (auto in 24h)"],
  MAIN_BATT_CRIT:       ["Main battery critically low", ""],
  BACKUP_EXHAUSTED:     ["Backup battery used up", ""],
  BACKUP_SWITCH_FAILED: ["Switch to backup failed", ""]
};
// The first known code in a fault text: "MAJ|SENSOR_FAIL|INA226", "ESP2_DEGRADED PUMP_WEAK|..." and the like.
function faultCodeIn(text) {
  return String(text || "").split(/[|\s,]+/).find(token => Object.prototype.hasOwnProperty.call(FAULT_INFO, token)) || "";
}
/* ---- Tank check (18 Nov 2026) ----------------------------------------------------------------------------------------
 * ESP1 from 18 Nov 2026 publishes diagnostics.tankCheck = {needed, why}. While needed, it starts no scheduled run until
 * someone confirms the mixing tank: here (TANK_CHECK_OK, control accounts, idle only), SMS TANKCHECK,OK, or the LCD. */
const TANK_CHECK_WHY = {
  ESP1_RESET_DURING_RUN: "ESP1 restarted in the middle of a run, so the mixing tank may still hold that run's water.",
  CARRIED_OVER_RESTART: "It was already waiting before ESP1's last restart."
};
function tankCheckBlockReason() {
  if (!hasControlAccess()) return "Confirming needs control access. Ask an operator, or send SMS TANKCHECK,OK from the owner's phone.";
  if (!firebaseConnected) return "This browser is offline.";
  if (!deviceIsFresh()) return "ESP1 isn't reporting, so it can't be told. Use SMS TANKCHECK,OK or the LCD.";
  if (rigLocked()) return LOCKED_TITLE;
  if (liveData.system?.state !== "IDLE_STATE") return "Only while the rig is idle: ESP1 refuses it during a run or a test.";
  return "";
}
function renderTankCheck() {
  const banner = document.getElementById("tankCheckBanner");
  if (!banner) return;
  const check = liveData.diagnostics?.tankCheck;
  const needed = check?.needed === true;
  banner.hidden = !needed || !currentUserIsSignedIn() || !isApprovedUser();
  if (banner.hidden) return;
  const why = TANK_CHECK_WHY[check.why] || (check.why ? `ESP1's reason: ${check.why}.` : "");
  setText("tankCheckDetail", `ESP1 starts no scheduled run until someone has looked at the mixing tank. ${why}`);
  const button = document.getElementById("tankCheckBtn");
  if (!button) return;
  const block = tankCheckBlockReason();
  button.hidden = !hasControlAccess();
  const sent = latestCommandOfType("TANK_CHECK_OK");
  const waiting = sent && ["queued", "received"].includes(sent.status);
  button.disabled = Boolean(block) || Boolean(waiting);
  button.title = block || (waiting ? "Waiting for ESP1." : "");
  const result = document.getElementById("tankCheckResult");
  if (result && block && !waiting && (!sent || serverNow() - Number(sent.updatedAt || sent.requestedAt || 0) > 60000)) {
    result.textContent = block;
    result.className = "control-result";
  }
}
document.getElementById("tankCheckBtn")?.addEventListener("click", () => {
  const block = tankCheckBlockReason();
  const show = resultLine("tankCheckResult");
  if (block) { show(block, true); return; }
  if (!confirm("Have you looked at the mixing tank, and sent any water left in it to its column? Scheduled runs start again once ESP1 has this.")) {
    show("Nothing was sent.");
    return;
  }
  sendFromPanel(show, "TANK_CHECK_OK", {});
});

// "Mixing tank overfilling. Pumps stopped. Check tank, clear on LCD." or "" when no code is known.
function faultInfoText(text) {
  const code = faultCodeIn(text);
  if (!code) return "";
  const [what, todo] = FAULT_INFO[code];
  return todo ? `${what}. ${todo}.` : `${what}.`;
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
  return clock12(date.getHours() * 60 + date.getMinutes());
}

function upNextItems(now) {
  const items = [];
  const nowMin = now.getHours() * 60 + now.getMinutes();
  const today = dayStart(now);
  const tomorrow = dayStart(new Date(today.getTime() + 36 * 3600000));
  ["A", "B", "C"].forEach(id => {
    const s = zoneSettings(id);
    if (!s.known || !s.enabled || s.sched === null || !s.windows?.length) return;
    // Its own Soil start since 18 Nov 2026 (b). Kept together: a lone "%." wrapped onto its own line.
    const ifDry = `Waters if drier than ${s.startPct}${NBSP}%`;
    const span = ([a, b]) => clockSpan(a, b);
    const started = s.windows.filter(([a]) => a <= nowMin).length;
    const done = s.runsToday === null ? 0 : s.runsToday;
    // A Manual day (18 Nov 2026): ESP1 skips the column's watering times, so they are left out and the day says why.
    const manualOn = day => s.operation !== null && operationOn(s, ymdOfDate(day)) === "MANUAL";
    const manualNote = "Manual: no scheduled watering. Water it with Water a column now.";
    if (manualOn(today)) items.push({ day: today, at: -1, time: "All day", what: `Column ${id}`, note: manualNote });
    // The day closed by a STOP reply or a lost fault (ESP1 from 18 Nov 2026): nothing more today.
    else if (s.skippedToday) items.push({ day: today, at: -1, time: "Rest of today", what: `Column ${id}`, note: "Skipped: a STOP reply or a lost fault closed the day." });
    else s.windows.forEach(w => {
      const [a, b] = w;
      // ESP1 counts both ends as inside a window (windowAt() in its main.cpp), so 12:00–13:00 is still open at 13:00.
      if (done >= s.windows.length || b < nowMin) return;           // the day is done, or this time is over
      const open = a <= nowMin;
      if (open && done >= started) return;                          // it has watered in this one already
      // Resting after repeated faults (ESP1 from 18 Nov 2026): a time inside the rest is passed over until it ends.
      const rest = s.restMin && (open || a < nowMin + s.restMin) ? ` Resting about ${s.restMin} more min after repeated faults first.` : "";
      items.push({ day: today, at: open ? nowMin : a, time: span(w), what: `Column ${id}`, note: (open ? `Open now. ${ifDry}.` : `${ifDry}.`) + rest, open });
    });
    if (manualOn(tomorrow)) items.push({ day: tomorrow, at: -1, time: "All day", what: `Column ${id}`, note: manualNote });
    else s.windows.forEach(w => items.push({ day: tomorrow, at: w[0], time: span(w), what: `Column ${id}`, note: `${ifDry}.` }));
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
  if (d.tankCheck?.needed === true) reasons.push("The mixing tank must be checked first: no column is watered by the schedule until someone confirms it (the banner at the top).");
  const reservoir = Number(liveData.sensors?.reservoirLevel);
  if (hasValue(liveData.sensors?.reservoirLevel) && reservoir < resPausePercent()) reasons.push(`The reservoir is under ${resPausePercent()} %: no column is watered until it is refilled.`);
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

/* Times as words (18 Nov 2026, the owner: "6:00 – 8:00, with am or pm each", for every time shown): the 12-hour clock,
 * "6:00 AM", "12:30 PM". minutesToHhmm() above stays for <input type="time"> values, which take 24-hour text; those
 * boxes themselves follow the device's own clock setting. */
function clock12(mins) {
  if (!hasValue(mins)) return "--";
  const m = ((Math.round(Number(mins)) % 1440) + 1440) % 1440;
  const h = Math.floor(m / 60);
  return `${h % 12 || 12}:${String(m % 60).padStart(2, "0")} ${h < 12 ? "AM" : "PM"}`;
}
function clockSpan(a, b) { return `${clock12(a)} – ${clock12(b)}`; }
// A Date's time of day, the same way ("6:05 PM"), with seconds when asked ("6:05:09 PM").
function timeOfDayText(date, seconds = false) {
  const text = clock12(date.getHours() * 60 + date.getMinutes());
  return seconds ? text.replace(" ", `:${String(date.getSeconds()).padStart(2, "0")} `) : text;
}
// A moment as "18 Nov 2026, 6:05 PM" (a Date or milliseconds).
function dateTimeText(value) {
  const date = value instanceof Date ? value : new Date(Number(value));
  if (Number.isNaN(date.getTime())) return "Unknown";
  return `${date.getDate()} ${date.toLocaleDateString([], { month: "short" })} ${date.getFullYear()}, ${timeOfDayText(date)}`;
}
// ESP1's own "2026-11-18 14:05:09" text, its times turned to the 12-hour clock ("2026-11-18 2:05:09 PM").
function rigTimeText(text) {
  return String(text).replace(/\b(\d{1,2}):(\d{2})(?::(\d{2}))?\b/g, (all, h, m, s) => {
    if (Number(h) > 23 || Number(m) > 59) return all;
    const t = clock12(Number(h) * 60 + Number(m));
    return s ? t.replace(" ", `:${s} `) : t;
  });
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
  ["TANK_CHECK_OK", "tankCheckResult"],
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
  // Too wet and the reservoir pause (18 Nov 2026): only to ESP1 that has them, which says so by reporting them.
  const newLines = thresholdsSupported();
  const start = numberFromInput("sysThreshStart");
  const stop  = needsStop ? numberFromInput("sysThreshStop") : null;
  const gap   = numberFromInput("sysThreshGap");
  const wet   = newLines ? numberFromInput("sysThreshWet") : null;
  const res   = newLines ? numberFromInput("sysThreshRes") : null;
  if (![start, gap, ...(needsStop ? [stop] : []), ...(newLines ? [wet, res] : [])].every(Number.isFinite)) { show("Fill in every field with a number. Nothing was sent."); return; }
  if (!Number.isInteger(start) || start < 0 || start > 100) { show("Soil start must be a whole number from 0 to 100. Nothing was sent."); return; }
  if (needsStop && (!Number.isInteger(stop) || stop < 0 || stop > 100)) { show("Soil stop must be a whole number from 0 to 100. Nothing was sent."); return; }
  if (gap < 0 || gap > 500) { show("Gap must be 0-500. Nothing was sent."); return; }
  if (newLines && (!Number.isInteger(wet) || wet > 100 || wet < start + THRESH_MIN_GAP)) {
    show(`Too wet must be a whole number from ${Math.min(100, start + THRESH_MIN_GAP)} to 100: at least ${THRESH_MIN_GAP} above Soil start. Nothing was sent.`); return;
  }
  if (newLines && (!Number.isInteger(res) || res < RIG_RES_LOW_PCT || res > RES_PAUSE_MAX)) {
    show(`The reservoir pause must be a whole number from ${RIG_RES_LOW_PCT} to ${RES_PAUSE_MAX} %. It can't go under ${RIG_RES_LOW_PCT} %: the reservoir's sensor can't see the water below its filter, so the pumps could run dry. Nothing was sent.`); return;
  }
  if (start === 0 && !confirm("Soil start 0% means no column will ever be dry enough, so scheduled watering stops. Send it anyway?")) { show("Nothing was sent.", false); return; }
  const level = liveData.sensors?.reservoirLevel;
  if (newLines && res > resPausePercent() && hasValue(level) && Number(level) < res &&
      !confirm(`The reservoir is at ${Math.round(Number(level))} % now, under the new pause of ${res} %. No column will be watered until it is refilled above ${res} %. Send it anyway?`)) {
    show("Nothing was sent.", false); return;
  }
  const payload = { thStart: start, thGap: gap };
  if (needsStop) payload.thStop = stop;
  if (newLines) { payload.thWet = wet; payload.thRes = res; }
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
  // The thresholds as ESP1 has them (firmware of 18 Nov 2026), and as the starting values of empty boxes.
  const known = hasValue(system.soilStartPct) && hasValue(system.fertGap);
  const newLines = thresholdsSupported();
  setText("sysThreshNow", !known ? "Not reported by this firmware (ESP1 reports them from its firmware of 18 Nov 2026)"
    : `Soil start ${Number(system.soilStartPct)} %, ` +
      (newLines ? `Too wet ${Number(system.soilWetPct)} %, ` : "") +
      `Fertigation gap ${Number(system.fertGap)} mg/kg` +
      (newLines ? `, reservoir pause ${resPausePercent()} %` : `, reservoir pause ${RIG_RES_LOW_PCT} % (fixed in this firmware)`));
  if (known) {
    [["sysThreshStart", system.soilStartPct], ["sysThreshGap", system.fertGap],
     ...(newLines ? [["sysThreshWet", system.soilWetPct], ["sysThreshRes", system.resPausePct]] : [])].forEach(([id, value]) => {
      const box = document.getElementById(id);
      if (box && !box.value && document.activeElement !== box) box.value = String(Number(value));
    });
  }
  // Too wet and the reservoir pause can only be sent to ESP1 that has them (firmware of 18 Nov 2026, b).
  ["sysThreshWet", "sysThreshRes"].forEach(id => { const box = document.getElementById(id); if (box) box.disabled = !newLines; });
  const newNote = document.getElementById("sysThreshNewNote");
  if (newNote) newNote.hidden = newLines || !liveData.system;
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
// nutD since 18 Nov 2026: the owner opened Nutrient D on 16 Nov (LCD Testing, flow calibration); see index.html.
const MT_DOSING = { nutA: 1, nutB: 1, nutC: 1, nutD: 1, phUp: 1, phDn: 1 };
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

// Light is the default from 18 Nov 2026 (the owner's choice); a saved choice is kept. The <head> script applies it
// before the first paint; this repeats it for safety and keeps the button in step. Storage can be blocked (private
// windows, strict settings), so every access is guarded and the page works without it.
const themeToggle = document.getElementById("theme-toggle");
let savedTheme = null;
try { savedTheme = localStorage.getItem("theme"); } catch { /* storage blocked */ }
document.documentElement.dataset.theme = savedTheme === "dark" ? "dark" : "light";
function refreshThemeButton() {
  if (!themeToggle) return;
  const dark = document.documentElement.dataset.theme === "dark";
  // The words have their own span: the button also holds the sun / moon icons.
  const label = themeToggle.querySelector(".theme-label") || themeToggle;
  label.textContent = dark ? "Light theme" : "Dark theme";
  themeToggle.setAttribute("aria-label", dark ? "Switch to the light theme" : "Switch to the dark theme");
}
refreshThemeButton();
themeToggle?.addEventListener("click", () => {
  const next = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem("theme", next); } catch { /* storage blocked: kept for this visit only */ }
  refreshThemeButton();
});

const contributorsDialog = document.getElementById("contributorsDialog");
document.getElementById("contributorsBtn")?.addEventListener("click", () => contributorsDialog?.showModal());
contributorsDialog?.querySelector(".closeDialog")?.addEventListener("click", () => contributorsDialog.close());

/* ---- Refresh (2026-11-17; "link lite" since 18 Nov 2026 g) ----------------------------------------------------------
 * The owner, 18 Nov 2026 (evening): "make the website communication back to before, no longer high realtime update ...
 * but still it automatically refreshes to request data from the system, and add a refresh button. To lessen the load of
 * ESP1." ESP1 (web v.4 of 18 Nov g) reports by itself once a minute (meta.refreshMs), and this page shows each report as
 * it arrives. On top of that it asks ESP1 for one report at once (irrigation/viewer/refreshAt):
 *   - when the page opens or comes back to the front,
 *   - by itself when ESP1's regular report is overdue (one interval + 30 s), at most every AUTO_REFRESH_MIN_GAP_MS,
 *   - and when Refresh is pressed (which also says how long ESP1 took).
 * It no longer tells ESP1 that someone is watching (irrigation/viewer/at every 15 s, 17 Nov to 18 Nov f): that set a
 * faster pace, the load this undoes. The pace itself is ESP1's; nothing here assumes a number. */
const AUTO_REFRESH_MIN_GAP_MS = 120000;
// How long Refresh waits for ESP1's answer: ESP1 reads the request within ~6 s and answers at most every 10 s (30 s busy).
function refreshWaitMs() { return liveData.meta?.linkMode === "busy" ? 45000 : 20000; }
let refreshAskedAt = 0, refreshBaseStamp = 0, refreshTimeout = null, refreshClearTimer = null;
let pageWasVisible = false, lastAutoRefreshAt = 0;

// Ask ESP1 for one report now. The rule takes refreshAt within the last minute and at most every 2 s.
function askForReport() {
  if (!db || !auth?.currentUser || !isApprovedUser() || databaseAccessDenied) return Promise.resolve(false);
  return db.ref("irrigation/viewer").update({ refreshAt: firebase.database.ServerValue.TIMESTAMP }).then(() => true).catch(error => {
    console.warn("irrigation/viewer", error);
    return false;
  });
}

// From the 1 s tick and on visibility changes: show Refresh to the accounts that may use it, ask for a report when the
// page comes (back) into view, and by itself when ESP1's regular report is overdue.
function syncAutoRefresh() {
  const allowed = Boolean(auth?.currentUser) && isApprovedUser() && !databaseAccessDenied;
  const refreshButton = document.getElementById("refreshBtn");
  if (refreshButton) refreshButton.hidden = !allowed;
  const visible = allowed && document.visibilityState === "visible";
  const now = Date.now();
  if (visible && !pageWasVisible) {
    lastAutoRefreshAt = now;
    askForReport();
  } else if (visible && firebaseConnected && !refreshAskedAt && now - lastAutoRefreshAt >= AUTO_REFRESH_MIN_GAP_MS) {
    const stamp = Number(liveData.meta?.updatedAt || 0);
    const every = Number(liveData.meta?.refreshMs || 60000);
    if (stamp && serverNow() - stamp > every + 30000) { lastAutoRefreshAt = now; askForReport(); }
  }
  pageWasVisible = visible;
}
document.addEventListener("visibilitychange", syncAutoRefresh);

/* Quick reconnect (18 Nov 2026, the owner: "very realtime connection"). While its connection keeps failing, the Firebase
 * SDK waits longer and longer between attempts (x1.3 each time, up to 5 minutes), so on patchy mobile data a page could
 * stay offline for minutes after the network was back. Once this page has been offline for RECONNECT_NUDGE_MS while on
 * screen, the attempt is restarted at once, and again every RECONNECT_NUDGE_MS at most, so a slow attempt still has time
 * to finish. Coming back to the front also restarts it, 2 s later, if the SDK hasn't reconnected by itself by then. */
const RECONNECT_NUDGE_MS = 20000;
function nudgeFirebaseReconnect(onReturn) {
  if (!db || !databaseListenersAttached || firebaseConnected || !firebaseDownSince) return;
  if (document.visibilityState !== "visible" || navigator.onLine === false) return;
  const now = Date.now();
  if (now - lastReconnectNudge < (onReturn ? 5000 : RECONNECT_NUDGE_MS)) return;
  if (!onReturn && now - firebaseDownSince < RECONNECT_NUDGE_MS) return;
  lastReconnectNudge = now;
  db.goOffline();   // drops the waiting attempt and its growing delay...
  db.goOnline();    // ...and starts a new one now
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible") setTimeout(() => nudgeFirebaseReconnect(true), 2000);
});

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
  // The label has its own span since 18 Nov 2026: the button also holds Simple mode's icon.
  if (button) { button.disabled = true; (button.querySelector(".refresh-label") || button).textContent = "Refreshing…"; }
  refreshStatusText(liveData.meta?.linkMode === "busy"
    ? "Asking ESP1 for a fresh report… The rig is working on a watering, so it may take up to 20 s."
    : "Asking ESP1 for a fresh report…");
  askForReport().then(ok => {
    if (!ok) finishRefresh(false, "Couldn't ask ESP1: this account may not be allowed, or the connection dropped.");
  });
  // Re-read what Firebase already holds, so the page is current even before ESP1 answers.
  liveRef?.once("value").then(snapshot => {
    liveData = snapshot.val() || {};
    lastKnownFresh = deviceIsFresh();
    lastKnownDelayed = deviceDelayMs() > 0;
    updateDashboard();
    setText("liveAge", liveAgeText());
    syncConnection();
    syncControlAvailability();
  }).catch(() => {});
  if (refreshTimeout) clearTimeout(refreshTimeout);
  refreshTimeout = setTimeout(() => finishRefresh(false), refreshWaitMs());
}

function finishRefresh(answered, message) {
  if (!refreshAskedAt) return;
  const took = Math.max(0, serverNow() - refreshAskedAt);
  refreshAskedAt = 0;
  if (refreshTimeout) { clearTimeout(refreshTimeout); refreshTimeout = null; }
  const button = document.getElementById("refreshBtn");
  if (button) { button.disabled = false; (button.querySelector(".refresh-label") || button).textContent = "Refresh"; }
  if (answered) refreshStatusText(`Fresh report from ESP1 in ${(took / 1000).toFixed(1)} s`, 8000);
  else refreshStatusText(message || "No new report from ESP1 yet: it may be offline (often a weak Wi-Fi signal at the rig) or busy. Its next regular report will still come.", 15000);
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
    // The rig's own date (its log files are named by it), so "today" here is ESP1's today.
    const day = String(todayLogDay());
    dayInput.value = `${day.slice(0, 4)}-${day.slice(4, 6)}-${day.slice(6, 8)}`;
  }
  if (want && !logsRef && !logsDenied) {
    logsRef = db.ref("logs/index");
    logsRef.on("value", snapshot => {
      logsIndex = snapshot.val() || {};
      renderLogs();
      renderLogRequest();
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
    case "done": {
      const kb = Number(entry.size || 0) / 1024;
      return `${kb > 0 && kb < 1 ? "Under 1" : kb.toFixed(0)} KB${entry.partial ? " (the day so far)" : ""}, sent${when}`;
    }
    case "uploading": return logUploadStalled(entry)
      ? `Stopped while being sent${when}. Ask again with Upload from the rig`
      : `Being sent from the rig${when}…`;
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
    // Pieces are 0000, 0001, ...: joined in number order, whatever form Firebase hands the list back in.
    const keys = Object.keys(pieces).sort((a, b) => Number(a) - Number(b));
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

/* Fetching a day on demand. Merged 18 Nov 2026: today's log used to have its own panel on Manual/Test (user, 17 Nov: "a
 * button to update the CSV, especially if I want the logs any time in the morning ... when the system is idle"), and its
 * rule now covers every request from here: only while ESP1 is reporting, idle, with no run and no pump, valve or mixer
 * on, because the upload shares ESP1's memory and its one connection with the live report. ESP1 sends a day as far as it
 * is written (partial); Download joins the pieces. */
let logAskedAt = 0, logAskedDay = 0;
// ESP1 gives one upload FB_LOG_JOB_MAX_MS (15 min) and then drops it, leaving the index at "uploading" for good. (The
// firmware of 17 Nov afternoon left 3 and 4 Nov like that.) A minute more, so a slow last piece isn't called stopped.
const LOG_UPLOAD_GIVE_UP_MS = 16 * 60000;

function logUploadStalled(entry) {
  return entry?.state === "uploading" && serverNow() - Number(entry.at || 0) > LOG_UPLOAD_GIVE_UP_MS;
}

// The rig's date: its log files are named by its own clock. The browser's date only while ESP1's can't be read.
function todayLogDay() {
  return ymdOfDate(rigNow() || new Date(serverNow()));
}

function selectedLogDay() {
  const parts = (document.getElementById("logDay")?.value || "").match(/^(\d{4})-(\d{2})-(\d{2})$/);
  return parts ? Number(parts[1] + parts[2] + parts[3]) : 0;
}

function logRequestBlockReason() {
  if (!deviceIsFresh()) return "ESP1 isn't reporting, so it can't be asked.";
  const state = liveData.system?.state || "";
  if (!["IDLE_STATE", "SAFE_MODE"].includes(state)) return `Only while the rig is idle (it reports ${state || "nothing"}).`;
  if (liveData.system?.masterWorkOrderActive || liveData.diagnostics?.system?.workOrderActive) return "Only while no run is in progress.";
  const a = liveData.actuators || {};
  if (a.transferRunning || a.boosterRunning || a.mixerRunning) return "Only while no pump is running.";
  if (liveData.diagnostics?.actuator?.pumpTestActive) return "Only while no pump test is running.";
  return "";
}

// A request for `day` is on its way: asked in the last 30 s, or ESP1 is sending it now.
function logDayBeingSent(day) {
  const entry = logsIndex[String(day)];
  if (entry?.state === "uploading" && !logUploadStalled(entry)) return true;
  return logAskedDay === day && serverNow() - logAskedAt < 30000 && entry?.state !== "done";
}

function renderLogRequest() {
  const button = document.getElementById("logRequestBtn");
  if (!button) return;
  const day = selectedLogDay();
  const block = logRequestBlockReason();
  const sending = Boolean(day) && logDayBeingSent(day);
  button.disabled = Boolean(block) || sending || !canAccessPrivilegedTabs();
  button.title = block || (sending ? "Being sent now" : "Ask ESP1 to send this day's log");
  const blocked = document.getElementById("logRequestBlocked");
  if (blocked) { blocked.hidden = !block; blocked.textContent = block; }
  // Today's log at a glance (what the Manual/Test panel showed).
  const today = todayLogDay();
  const entry = logsIndex[String(today)];
  const state = entry?.state || "";
  const stalled = logUploadStalled(entry);
  const asked = logDayBeingSent(today);
  setDeviceStatus("logTodayState", `TODAY: ${
      state === "done" ? (entry.partial ? "SENT (SO FAR)" : "SENT")
    : state === "uploading" ? (stalled ? "STOPPED" : "SENDING")
    : state === "failed" ? "FAILED" : state === "missing" ? "NOT ON CARD" : asked ? "ASKED" : "NOT SENT"}`,
    state === "done" ? "active" : (state === "uploading" && !stalled) || asked ? "warn" : state === "failed" || stalled ? "danger" : "off");
}

function requestLogUpload() {
  const day = selectedLogDay();
  if (!db || !day) { setText("logsNote", "Pick a day first."); return; }
  const block = logRequestBlockReason();
  if (block) { setText("logsNote", block); return; }
  sendLogRequest(day, "logsNote").then(ok => {
    if (!ok) return;
    logAskedAt = serverNow(); logAskedDay = day;
    renderLogRequest();
  });
}

document.getElementById("logRequestBtn")?.addEventListener("click", requestLogUpload);
document.getElementById("logDay")?.addEventListener("change", renderLogRequest);
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
  setText("liveAge", liveAgeText());   // the age moves every second
  syncAutoRefresh();                   // Refresh button, and a report asked for on opening / when one is overdue
  nudgeFirebaseReconnect(false);       // offline a while on screen: don't wait out the SDK's growing delay (18 Nov 2026)
  syncLogsListener();                  // the SD card logs list, for the accounts that may see it
  renderLogRequest();                  // Diagnostics > SD card logs: idle-only request, today's state
  updateForceArmed();
  renderUpNext();             // the clock moves on: a watering time opens or closes, the day turns over
  if (faultAckLocalUntil && Date.now() >= faultAckLocalUntil) faultAckLocalUntil = 0;
  updateFaultBanner();
  renderTankCheck();                   // its button follows freshness and the rig's state
  renderSimpleDashboard();             // Simple mode: ages, countdowns and "unknown" when ESP1 goes quiet
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

/* ==== Simple mode (18 Nov 2026) =====================================================================================
 * A plainer way of showing the same page, after the owner's sample design: the farm at a glance on the dashboard, big
 * numbers on Zone profiles, and the technical parts of Controls folded away. Presentation only. It reads the same
 * liveData and commandData as everything else, sends nothing of its own, and every control it shows is the same element,
 * with the same handler and the same checks, as in Complete mode. The choice is this browser's own (localStorage
 * "uiMode"), never written to Firebase; the <head> script applies it before the first paint.
 * Freshness: anything deviceIsFresh() doesn't vouch for is shown as "last known" or "unknown", never as running or idle,
 * and nothing in the picture moves. */
const UI_MODE_KEY = "uiMode";
function storedUiMode() {
  try { return localStorage.getItem(UI_MODE_KEY) === "complete" ? "complete" : "simple"; } catch { return "simple"; }
}
function uiMode() {
  return document.documentElement.dataset.mode === "complete" ? "complete" : "simple";
}

function applyUiMode(mode, { save = false, focus = false } = {}) {
  const next = mode === "complete" ? "complete" : "simple";
  document.documentElement.dataset.mode = next;
  if (save) { try { localStorage.setItem(UI_MODE_KEY, next); } catch { /* storage blocked: kept for this visit only */ } }
  document.querySelectorAll("#modeSwitch [data-mode-choice]").forEach(option => {
    const on = option.dataset.modeChoice === next;
    option.setAttribute("aria-checked", on ? "true" : "false");
    option.tabIndex = on ? 0 : -1;
    if (on && focus) option.focus();
  });
  // Simple mode has no Manual/Test, System or Diagnostics. Leaving one goes through the tab's own click handler, the
  // one path that leaves Manual/Test properly: it disarms the page and gives ESP1's manual hold straight back.
  if (next === "simple" && document.querySelector(".tab.active")?.classList.contains("complete-tab")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  renderSimpleDashboard();
}

function initModeSwitch() {
  const group = document.getElementById("modeSwitch");
  if (!group) return;
  const options = [...group.querySelectorAll("[data-mode-choice]")];
  options.forEach(option => option.addEventListener("click", () => {
    if (option.dataset.modeChoice !== uiMode()) applyUiMode(option.dataset.modeChoice, { save: true });
  }));
  // A radio group: Tab reaches the chosen option, the arrow keys (and Home / End) choose the other.
  group.addEventListener("keydown", event => {
    const steps = { ArrowLeft: -1, ArrowUp: -1, ArrowRight: 1, ArrowDown: 1 };
    if (!(event.key in steps) && event.key !== "Home" && event.key !== "End") return;
    event.preventDefault();
    const at = Math.max(0, options.findIndex(o => o.dataset.modeChoice === uiMode()));
    const index = event.key === "Home" ? 0 : event.key === "End" ? options.length - 1
      : (at + steps[event.key] + options.length) % options.length;
    applyUiMode(options[index].dataset.modeChoice, { save: true, focus: true });
  });
  // Changed in another tab of this site: follow it, so two windows never disagree about what is shown.
  window.addEventListener("storage", event => {
    if (event.key === UI_MODE_KEY) applyUiMode(storedUiMode());
    if (event.key === "theme") {
      document.documentElement.dataset.theme = event.newValue === "dark" ? "dark" : "light";
      refreshThemeButton();
    }
  });
  applyUiMode(storedUiMode());
}

// Who is signed in (18 Nov 2026, the owner: "show the user account name"): the name on the account's own record (given
// at sign-up, users/<uid>/name), else the part of the address before the @. Its first letter is the avatar, and the
// whole address is in the tooltip. Text from Firebase only ever goes in through textContent.
function syncAccountBadge(user = auth?.currentUser) {
  const chip = document.getElementById("accountChip");
  const badge = document.getElementById("accountBadge");
  const nameEl = document.getElementById("accountName");
  if (!chip || !badge || !nameEl) return;
  const email = String(user?.email || "").trim();
  const recorded = String(myAccountName || "").trim();
  const name = !email ? "" : recorded && recorded.toLowerCase() !== email.toLowerCase() ? recorded : email.split("@")[0];
  chip.hidden = !email;
  badge.textContent = name ? name.charAt(0).toUpperCase() : "";
  nameEl.textContent = name;
  chip.title = email ? `Signed in as ${name} (${email})` : "";
}

// Small setters that only touch the page when something changed, as this runs every second.
function sSet(id, text) {
  const el = document.getElementById(id);
  if (!el) return;
  if (el.classList.contains("s-roll")) { setText(id, text); return; }
  if (el.textContent !== text) el.textContent = text;
}
function sAttr(el, name, value) {
  if (el && el.getAttribute(name) !== value) el.setAttribute(name, value);
}
function sTone(id, tone) {
  const el = document.getElementById(id);
  if (el && el.dataset.tone !== tone) el.dataset.tone = tone;
}
function sIcon(useId, symbol) {
  sAttr(document.getElementById(useId), "href", `#${symbol}`);
}
// A level bar: fills to pct (0 to 100 shown; a tank can read over 100 %), or empties and says so when there is no reading.
function sBar(id, pct, stale) {
  const bar = document.getElementById(id);
  if (!bar) return;
  const known = pct !== null;
  const shown = known ? Math.min(100, Math.max(0, pct)) : 0;
  const fill = bar.firstElementChild;
  if (fill) fill.style.width = `${shown}%`;
  bar.classList.toggle("is-stale", Boolean(stale));
  bar.classList.toggle("is-empty", !known);
  if (known) { sAttr(bar, "aria-valuenow", String(Math.round(shown))); sAttr(bar, "aria-valuetext", `${Math.round(pct)} percent${stale ? ", last known" : ""}`); }
  else { bar.removeAttribute("aria-valuenow"); sAttr(bar, "aria-valuetext", "No reading"); }
}

// ESP2's stage names in plain words (the ones this page knows; anything else is shown as ESP2 named it).
const SIMPLE_STAGE_WORDS = {
  "Starting": "Getting ready",
  "Transfer Water": "Filling the mixing tank from the reservoir",
  "Pump Nutrients": "Adding nutrients to the mixing tank",
  "Mixing": "Stirring the water and nutrients together",
  "Release": "Sending the water to the column",
  "Flush Fill": "Rinsing: filling the mixing tank with clean water",
  "Flush Release": "Rinsing: sending the clean water to the column"
};
function simpleStageText(run) {
  return SIMPLE_STAGE_WORDS[run.stage] || rawText(run.stage, "Working");
}
// The same words mid-sentence ("Watering column A: getting ready.").
function lowerFirst(text) {
  return text ? text.charAt(0).toLowerCase() + text.slice(1) : text;
}

// Whether someone is testing by hand (Manual/Test on this site, the LCD's tests, or a pump exercise). ESP1's pump flags
// only follow a run's stages, so during a test a pump can be running while they say off: the page must not claim idle.
function simpleTesting() {
  const d = liveData.diagnostics || {};
  return liveData.system?.state === "TEST_MODE" || Boolean(d.actuator?.pumpTestActive) || d.webManual?.held === true;
}

// The one-line situation for the big status card, worst first: unknown, connection lost, a problem that stops the rig,
// running, something worth knowing, idle.
function simpleSituation() {
  const d = liveData.diagnostics || {};
  const sys = liveData.system || {};
  const run = d.runProgress || {};
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (databaseAccessDenied) return { tone: "wait", icon: "si-alert", title: "Can't read the farm's data", text: "This account isn't allowed to read the controller's reports." };
  if (!currentUserIsSignedIn() || !databaseListenersAttached) return { tone: "wait", icon: "si-clock", title: "Connecting…", text: "Waiting for the farm's data." };
  if (!stamp) return { tone: "wait", icon: "si-clock", title: "Waiting for the system", text: "The farm's controller (ESP1) hasn't sent a report yet, so its state is unknown." };
  if (!firebaseConnected) return { tone: "lost", icon: "si-unlink", title: "Connection lost", text: "This browser has lost its connection, so what the system is doing now is unknown. The numbers below are the last ones received." };
  // No running age in this sentence: the card is a live region, and the age (in the Controller chip) changes every
  // minute, which would make a screen reader repeat the whole card.
  if (!deviceIsFresh()) return { tone: "lost", icon: "si-unlink", title: "Controller not reporting", text: "It has stopped sending reports, so what the system is doing now is unknown. The numbers below are from its last report." };
  const bad = [];
  const warn = [];
  if (d.fault?.held) bad.push("A problem stopped the system and is waiting for someone to decide what to do. See the red banner at the top.");
  else if (sys.state === "EMERGENCY_STOP") bad.push("The emergency stop is on. Nothing runs until someone returns the system to normal.");
  if (d.tankCheck?.needed === true) bad.push("The mixing tank must be checked before scheduled watering goes on. See the banner at the top.");
  if (d.esp2?.communicationLost) bad.push("The controller has lost contact with the pump board (ESP2).");
  if (hasValue(d.nano?.lastSampleAgeMs) && ageFlag(d.nano.lastSampleAgeMs) !== "ok") bad.push("The sensor board (Nano) has stopped sending readings.");
  if (d.peripherals?.rtcOk === false || sys.rtcOk === false) bad.push("The controller's clock isn't working, so its watering times can't be trusted.");
  if (d.power?.batteryCritical) bad.push("The battery is critically low.");
  if (run.active && run.phase === "error") bad.push("The last run stopped with a problem. The controller's screen says what happened.");
  if (actuationsAreOff()) warn.push("Pumps and valves are switched off, so nothing runs, not even the schedule.");
  const res = liveData.sensors?.reservoirLevel;
  if (hasValue(res) && Number(res) < resPausePercent()) warn.push(`The reservoir is below ${resPausePercent()} %, so watering waits until it is refilled.`);
  if (d.power?.batteryLow && !d.power?.batteryCritical) warn.push("The battery is low.");
  // Too wet (18 Nov 2026): a warning, never a stop.
  const wet = ["A", "B", "C"].filter(id => simpleSoilState(id).tone === "wet");
  if (wet.length) warn.push(`Column ${columnList(wet)} ${wet.length > 1 ? "are" : "is"} too wet. Check for a leak or a stuck valve.`);
  const more = n => n > 0 ? ` (${n} more thing${n === 1 ? "" : "s"} to check in Complete mode.)` : "";
  if (bad.length) return { tone: "bad", icon: "si-stop", title: "Attention needed", text: bad[0] + more(bad.length + warn.length - 1) };
  const running = run.active && (run.phase === "starting" || run.phase === "running");
  if (running) {
    const what = run.operation === "fertigation" ? "Feeding" : "Watering";
    return { tone: "run", icon: "si-drop", title: "System is running", text: `${what} column ${rawText(run.zone, "?")}: ${lowerFirst(simpleStageText(run))}.${warn.length ? ` Also: ${warn[0]}` : ""}` };
  }
  if (simpleTesting()) return { tone: "warn", icon: "si-hand", title: "Testing in progress", text: "Someone is testing the pumps and valves by hand. Scheduled watering waits until the test is closed." };
  if (warn.length) return { tone: "warn", icon: "si-alert", title: "Attention needed", text: warn[0] + more(warn.length - 1) };
  if (["BOOT_STATE", "STARTUP_SYNC", "RECOVERY_STATE"].includes(sys.state)) {
    return { tone: "warn", icon: "si-clock", title: "Getting ready", text: `${(STATE_BUSY_TEXT[sys.state] || "ESP1 is starting up").replace(/^ESP1/, "The controller")}.` };
  }
  if (run.active && run.phase === "finishing") return { tone: "ok", icon: "si-check", title: "Watering finished", text: `Column ${rawText(run.zone, "?")} has just been ${run.operation === "fertigation" ? "fed" : "watered"}. All pumps are off.` };
  return { tone: "ok", icon: "si-check", title: "System is idle", text: "All pumps are off. It waters each column at its watering time when the soil is dry." };
}

// The reservoir or mixing tank card.
function renderSimpleTank(prefix, pct, litres, status, stale) {
  const known = hasValue(pct);
  const value = known ? Number(pct) : null;
  sSet(`${prefix}Pct`, known ? `${Math.round(value)} %` : "No reading");
  const why = !known ? TANK_STATUS_WORDS[status] : "";
  sSet(`${prefix}Litres`, known ? (hasValue(litres) ? `About ${numberText(litres, 1, "L")}` : "Litres not reported") : (why ? `The level sensor reports ${why}.` : "The controller sent no level."));
  sBar(`${prefix}Bar`, value, stale);
  const card = document.getElementById(prefix);
  card?.classList.toggle("is-stale", stale);
  const tag = card?.querySelector(".s-stale-tag");
  if (tag) tag.hidden = !stale || !known;
  return { known, value };
}

// A column's state in plain words, from its moisture against ESP1's Soil start % and the crop profile's moisture.
// Nothing invented: ESP1 waters below Soil start %, and the crop profile's number is this site's own reference.
function simpleSoilState(id) {
  const z = liveData.sensors?.zones?.[id] || {};
  const zone = activeZones.find(item => item.id === id);
  const target = zone ? zoneCrop(zone).stage?.moisture : null;
  const lines = zoneSettings(id);              // its own Soil start / Too wet (18 Nov 2026 b), or the defaults
  const start = lines.startPct;
  const wet = lines.wetPct;
  const fresh = deviceIsFresh();
  const run = liveData.diagnostics?.runProgress || {};
  const live = fresh && run.active && (run.phase === "starting" || run.phase === "running") && String(run.zone || "").toUpperCase().includes(id);
  if (z.enabled === false) return { tone: "off", word: "Switched off", say: "Switched off on the controller: not watered, no readings.", pct: null };
  if (!hasValue(z.moisture)) return { tone: "none", word: "No reading", say: "The controller sent no moisture reading for it.", pct: null };
  const pct = Math.min(100, Math.max(0, Number(z.moisture)));
  if (live) {
    const booster = Boolean(liveData.actuators?.boosterRunning);
    return { tone: "flow", word: booster ? "Being watered" : "Water being prepared", say: booster ? "Water is flowing to it now." : "Its water is being prepared in the mixing tank.", pct };
  }
  // Too wet (18 Nov 2026, the owner's Overwatered line): a warning only, nothing stops.
  if (pct >= wet) return { tone: "wet", word: "Too wet", pct, say: `Wetter than ${wet} %: too much water. Check for a leak or a stuck valve.` };
  // A Manual day (ESP1 from 18 Nov 2026): no scheduled run, so don't promise one.
  const manualToday = lines.operationToday === "MANUAL";
  if (pct < start) return { tone: "dry", word: "Needs water", pct, say: manualToday
    ? `Drier than ${start} %. Manual today: use Water a column now.`
    : `Drier than ${start} %: watered at its next watering time.` };
  if (hasValue(target) && pct < Number(target)) return { tone: "low", word: "Below target", say: `Moist enough, but under the crop's ${Number(target)} %.`, pct };
  return { tone: "ok", word: hasValue(target) ? "On target" : "Moist enough", say: hasValue(target) ? `At or above the crop's ${Number(target)} %.` : `Wetter than ${start} %: no water needed.`, pct };
}

function renderSimpleSoil(stale) {
  ["A", "B", "C"].forEach(id => {
    const s = simpleSoilState(id);
    const zone = activeZones.find(item => item.id === id);
    const crop = zone ? zoneCrop(zone) : {};
    sSet(`sColCrop${id}`, crop.crop ? `${crop.crop.name}${crop.stage ? `, ${crop.stage.name.toLowerCase()}` : ""}` : "");
    sSet(`sColPct${id}`, s.pct === null ? "--" : `${Math.round(s.pct)} %`);
    sSet(`sColState${id}`, s.word);
    sTone(`sCol${id}`, stale && s.tone === "flow" ? "none" : s.tone);
    document.getElementById(`sCol${id}`)?.classList.toggle("is-stale", stale);
    const tag = document.getElementById(`sColStale${id}`);
    if (tag) tag.hidden = !stale || s.pct === null;      // an old reading always says so
  });
  // Each column's own Soil start since 18 Nov 2026 (b); one number when they all agree.
  const starts = ["A", "B", "C"].filter(id => zoneSettings(id).enabled || !zoneSettings(id).known).map(id => [id, zoneStartPercent(id)]);
  const same = starts.every(([, v]) => v === starts[0]?.[1]);
  sSet("sSoilHint", !starts.length ? "No column is switched on."
    : same ? `Watered at its time when drier than ${starts[0][1]} %.`
    : `Watered at its time when drier than: ${starts.map(([id, v]) => `${id} ${v} %`).join(" · ")}.`);
}

function renderSimpleActivity(fresh) {
  const d = liveData.diagnostics || {};
  const run = d.runProgress || {};
  const bar = document.getElementById("sActBar");
  const progress = document.getElementById("sActProgress");
  let title, text, tone = "none", pct = null, progressText = "";
  if (!liveData.meta?.updatedAt) { title = "Waiting for the system"; text = "The controller hasn't reported yet."; }
  else if (!fresh) {
    title = "Unknown right now"; tone = "lost";
    text = run.active ? `Its last report showed column ${rawText(run.zone, "?")} being ${run.operation === "fertigation" ? "fed" : "watered"}. This page can't tell whether that is still going.`
      : "The controller isn't reporting, so this page can't tell what it is doing.";
  } else if (run.active && (run.phase === "starting" || run.phase === "running")) {
    tone = "run";
    title = `${run.operation === "fertigation" ? "Feeding" : "Watering"} column ${rawText(run.zone, "?")}`;
    const step = hasValue(run.stageOrdinal) && hasValue(run.stageTotal) ? ` (step ${run.stageOrdinal} of ${run.stageTotal})` : "";
    text = `${simpleStageText(run)}${step}.`;
    // As on the Complete dashboard (updateRunProgress()): the Release stage under way counts toward the total.
    const releasing = run.stage === "Release" || run.stage === "Flush Release";
    const delivered = Number(run.waterDeliveredLiters || 0) + (releasing ? Number(run.stageLiters || 0) : 0);
    if (Number(run.waterTargetLiters) > 0) {
      pct = Math.min(100, Math.max(0, delivered / Number(run.waterTargetLiters) * 100));
      progressText = `${numberText(delivered, 1, "L")} of ${numberText(run.waterTargetLiters, 1, "L")} delivered`;
    }
  } else if (run.active && run.phase === "finishing") {
    tone = "ok"; title = "Just finished";
    text = `Column ${rawText(run.zone, "?")} has been ${run.operation === "fertigation" ? "fed" : "watered"}. All pumps are off.`;
  } else if (run.active && run.phase === "error") {
    tone = "bad"; title = "Last run stopped";
    text = "The last run stopped with a problem. The controller's screen says what happened.";
  } else if (simpleTesting()) {
    tone = "warn"; title = "Testing by hand";
    const ex = EXERCISE_IDS.indexOf(String(d.actuator?.pumpUnderTest || ""));
    text = d.actuator?.pumpTestActive && ex >= 0 ? `${EXERCISE_PUMPS[ex]}: a short run to keep it healthy (pump exercise).` : "Someone is testing the pumps and valves. Scheduled watering waits.";
  } else if (forceArmedInfo) {
    tone = "run"; title = "Forced run counting down";
    text = `Column ${rawText(forceArmedInfo.columns, "?")}, ${numberText(forceArmedInfo.liters, 1, "L")}, starts in ${Math.max(0, forceArmedInfo.left)} s. Cancel it on Controls.`;
  } else {
    tone = "ok"; title = "Nothing running"; text = "All pumps are off.";
  }
  sTone("sActivity", tone);
  sSet("sActTitle", title);
  sSet("sActText", text);
  if (bar) bar.hidden = pct === null;
  if (pct !== null) sBar("sActBar", pct, false);
  if (progress) { progress.hidden = !progressText; if (progress.textContent !== progressText) progress.textContent = progressText; }
}

// Why the next watering may not happen: upNextBlockText()'s reasons, in Simple mode's plain words.
function simpleNextBlockText() {
  const d = liveData.diagnostics || {};
  const reasons = [];
  if (!deviceIsFresh()) reasons.push("From the controller's last report.");
  if (d.fault?.held) reasons.push("A problem is waiting for a decision, so nothing runs until then.");
  else if (liveData.system?.state === "EMERGENCY_STOP") reasons.push("The emergency stop is on, so nothing runs until it is cleared.");
  if (actuationsAreOff()) reasons.push("Pumps and valves are switched off, so nothing runs.");
  if (d.webManual?.held === true) reasons.push("Testing by hand is open, so nothing starts by itself.");
  if (d.tankCheck?.needed === true) reasons.push("The mixing tank must be checked first (see the banner at the top).");
  const res = liveData.sensors?.reservoirLevel;
  if (hasValue(res) && Number(res) < resPausePercent()) reasons.push(`The reservoir is below ${resPausePercent()} %, so nothing is watered until it is refilled.`);
  return reasons.join(" ");
}

function renderSimpleNext(fresh) {
  const now = liveData.system ? rigNow() : null;
  let what = "--", when = "Waiting for the controller's report.", note = "";
  if (liveData.system && !now) { when = "Unknown"; note = "The controller's clock can't be read, so its watering times are unknown."; }
  else if (now) {
    const items = upNextItems(now).filter(item => !/exercise$/.test(item.what));
    const next = items.find(item => item.forced || item.at >= 0);
    const manual = items.filter(item => item.at < 0 && item.day.getTime() === dayStart(now).getTime()).map(item => item.what);
    if (next) {
      const days = Math.round((next.day.getTime() - dayStart(now).getTime()) / 86400000);
      what = next.what;
      when = `${days === 0 ? "Today" : days === 1 ? "Tomorrow" : next.day.toLocaleDateString([], { weekday: "long" })}, ${next.time}`;
      note = next.note;
    } else {
      what = "Nothing planned";
      when = "No watering time ahead";
      const on = ["A", "B", "C"].map(zoneSettings).filter(s => s.known && s.enabled);
      note = on.length ? "" : "No column is switched on.";
    }
    if (manual.length) note = `${note}${note ? " " : ""}${columnList(manual.map(w => w.replace(/^Column /, "")))} ${manual.length === 1 ? "is" : "are"} not watered by the schedule today.`;
    const block = simpleNextBlockText();
    if (block) note = `${block} ${note}`.trim();
  }
  if (!fresh && liveData.meta?.updatedAt && !now) note = `From the controller's last report. ${note}`.trim();
  sSet("sNextWhat", what);
  sSet("sNextWhenText", when);
  sSet("sNextNote", note);
}

function renderSimpleMore(fresh) {
  const s = liveData.sensors || {};
  sSet("sTemp", numberText(s.temperature, 1, "°C").replace("Unavailable", "--"));
  sSet("sHum", numberText(s.humidity, 0, "%").replace("Unavailable", "--"));
  sSet("sLux", hasValue(s.lightLevel) ? `${Math.round(Number(s.lightLevel)).toLocaleString()} lux` : "--");
  sSet("sPh", numberText(s.waterPH, 1).replace("Unavailable", "--"));
  sSet("sEc", numberText(s.waterEC, 2, "mS/cm").replace("Unavailable", "--"));
  sSet("sBattV", numberText(s.batteryVoltage, 1, "V").replace("Unavailable", "--"));
  sSet("sSolarToday", numberText(s.solarEnergyTodayWh, 0, "Wh").replace("Unavailable", "--"));
  sSet("sBatt2", numberText(s.secondaryBatteryPercent, 0, "%").replace("Unavailable", "--"));
  const stamp = Number(liveData.meta?.updatedAt || 0);
  sSet("sMoreFoot", !stamp ? "Waiting for the controller's first report."
    : `${fresh ? "From the controller's latest report" : "From the controller's last report"}, ${formatAge(Math.max(0, serverNow() - stamp))} ago. pH and EC are measured in the mixing tank. Every reading is in Complete mode.`);
}

// Everything on the Simple dashboard. Runs with every snapshot and every second (ages, countdowns, freshness).
function renderSimpleDashboard() {
  if (!document.getElementById("simpleDashboard")) return;
  const fresh = deviceIsFresh();
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const stale = Boolean(stamp) && !fresh;
  const sit = simpleSituation();
  sTone("sStatus", sit.tone);
  sIcon("sStatusIconUse", sit.icon);
  sSet("sStatusTitle", sit.title);
  sSet("sStatusText", sit.text);

  // The chips: the controller's link, solar and battery.
  const link = !stamp ? { tone: "wait", icon: "si-wifi", text: databaseListenersAttached ? "No report yet" : "Connecting…" }
    : !firebaseConnected ? { tone: "bad", icon: "si-unlink", text: "This browser is offline" }
    : fresh && deviceDelayMs() ? { tone: "warn", icon: "si-wifi", text: "Delayed" }   // 18 Nov 2026; the age is below it
    : fresh ? { tone: "ok", icon: "si-wifi", text: "Connected" }
    : { tone: "bad", icon: "si-unlink", text: `Not reporting (${formatAge(serverNow() - stamp)})` };
  sTone("sChipLink", link.tone);
  sIcon("sLinkIconUse", link.icon);
  sSet("sLinkText", link.text);
  // How fresh the picture is, and ESP1's own pace (linkPaceText(): no realtime since 18 Nov 2026 e).
  const ageMs = Math.max(0, serverNow() - stamp);
  sSet("sLinkAge", !stamp || !fresh ? "" : `${ageMs < 2000 ? "Just now" : `${formatAge(ageMs)} ago`} · ${linkPaceText()}`);
  const sensors = liveData.sensors || {};
  const last = stale ? " · last known" : "";
  sSet("sSolar", hasValue(sensors.solarPower) ? `${numberText(sensors.solarPower, 1, "W")}${last}` : "No reading");
  const power = liveData.diagnostics?.power || {};
  const battWord = power.batteryCritical ? " · critical" : power.batteryLow ? " · low" : "";
  sSet("sBattery", hasValue(sensors.batteryPercent) ? `${numberText(sensors.batteryPercent, 0, "%")}${battWord}${last}` : "No reading");
  sTone("sChipBattery", stale || !hasValue(sensors.batteryPercent) ? "none" : power.batteryCritical ? "bad" : power.batteryLow ? "warn" : "ok");
  sTone("sChipSolar", stale || !hasValue(sensors.solarPower) ? "none" : "solar");

  // The two tanks.
  const rawTank = liveData.diagnostics?.sensorsRaw?.tank || {};
  const res = renderSimpleTank("sRes", sensors.reservoirLevel, sensors.reservoirLiters, rawTank.reservoirStatus, stale);
  // The reservoir pause: 18 % at least, adjustable on the System tab (ESP1 from 18 Nov 2026 b). The mark follows it.
  const pause = resPausePercent();
  const low = res.known && res.value < pause;
  sTone("sRes", !res.known || stale ? "none" : low ? "warn" : "water");
  const mark = document.getElementById("sResMark");
  if (mark) mark.style.left = `${pause}%`;
  sSet("sResHint", low ? `Under ${pause} %: watering waits until it is refilled, so the pumps never run dry.`
    : `Watering pauses below ${pause} % (the line), so the pumps never run dry.`);
  setText("reservoirNote", `Runs wait below ${pause} %`);
  const mix = renderSimpleTank("sMix", sensors.mixingLevel, sensors.mixingLiters, rawTank.mixingStatus, stale);
  sTone("sMix", !mix.known || stale ? "none" : "mix");

  renderSimpleSoil(stale);
  updateSimpleDiagram(fresh, stale);
  renderSimpleActivity(fresh);
  renderSimpleNext(fresh);
  renderSimpleMore(fresh);
  renderDashPlan();
}

/* The watering plan on the dashboard (18 Nov 2026, the owner: "a calendar button ... shows the operations calendar plan,
 * which day is manual or automatic ... In default it shows, but I can hide it"). A month, by ESP1's own date, each day
 * with every switched-on column in green (Automatic) or amber (Manual), and the letter on it so colour is never the only
 * sign. Read-only: the plan is changed in a column's Edit settings. Shown unless hidden with the Plan button; that choice
 * stays in this browser only (localStorage "planHidden"). */
const PLAN_HIDDEN_KEY = "planHidden";
let dashPlanMonth = 0;
let dashPlanKey = "";
let dashPlanHidden = (() => { try { return localStorage.getItem(PLAN_HIDDEN_KEY) === "1"; } catch { return false; } })();
function syncDashPlanToggle() {
  const card = document.getElementById("sPlanCard");
  if (card) card.hidden = dashPlanHidden;
  const toggle = document.getElementById("sPlanToggle");
  if (toggle) toggle.setAttribute("aria-expanded", String(!dashPlanHidden));
  sSet("sPlanToggleText", dashPlanHidden ? "Show plan" : "Hide plan");
}
document.getElementById("sPlanToggle")?.addEventListener("click", () => {
  dashPlanHidden = !dashPlanHidden;
  try { localStorage.setItem(PLAN_HIDDEN_KEY, dashPlanHidden ? "1" : "0"); } catch { /* storage blocked: this visit only */ }
  dashPlanKey = "";
  syncDashPlanToggle();
  renderDashPlan();
});
function renderDashPlan() {
  const box = document.getElementById("sPlanCal");
  const card = document.getElementById("sPlanCard");
  if (!box || !card || card.hidden) return;
  const known = Boolean(liveData.sensors?.zones);
  const today = rigTodayYmd();
  const lastMonth = monthOfYmd(ymdAddDays(today, PLAN_DAYS - 1));
  if (!dashPlanMonth || dashPlanMonth < monthOfYmd(today) || dashPlanMonth > lastMonth) dashPlanMonth = monthOfYmd(today);
  const cols = ["A", "B", "C"].map(id => [id, zoneSettings(id)]).filter(([, s]) => s.known && s.enabled);
  const planOn = cols.some(([, s]) => s.operation);
  const key = JSON.stringify([known, today, dashPlanMonth, planOn, cols.map(([id, s]) => [id, s.operation, plannedDays(s.plan, today).map(d => `${d}${s.plan[d]}`)]),
    ["A", "B", "C"].map(id => { const s = zoneSettings(id); return [s.known, s.enabled, activeZones.find(z => z.id === id)?.name]; })]);
  if (key === dashPlanKey) return;
  dashPlanKey = key;
  sSet("sPlanNote", !known ? "" : !cols.length ? "No column is switched on."
    : planOn ? "By the controller's own date. Change a column's days in Zone profiles, Edit settings."
    : "Every day is Automatic: Manual days need ESP1's firmware of 18 Nov 2026.");
  if (!known) { box.innerHTML = `<p class="s-hint">Waiting for the controller's report.</p>`; return; }
  const y = Math.floor(dashPlanMonth / 10000), m = Math.floor(dashPlanMonth / 100) % 100;
  const first = ymdToDayNumber(dashPlanMonth);
  const days = new Date(Date.UTC(y, m, 0)).getUTCDate();
  const lead = new Date(Date.UTC(y, m - 1, 1)).getUTCDay();
  const todayN = ymdToDayNumber(today), lastN = todayN + PLAN_DAYS - 1;
  const title = new Date(y, m - 1, 1).toLocaleDateString([], { month: "long", year: "numeric" });
  const nav = (dir, ok, words) => `<button type="button" class="plan-nav" data-dash-plan="${dir}"${ok ? "" : " disabled"} aria-label="${words}">${dir < 0 ? "‹" : "›"}</button>`;
  const cells = [];
  for (let i = 0; i < lead; i++) cells.push(`<li class="dp-blank" aria-hidden="true"></li>`);
  for (let d = 1; d <= days; d++) {
    const n = first + d - 1;
    const ymd = dayNumberToYmd(n);
    const inRange = n >= todayN && n <= lastN;
    const marks = inRange ? cols.map(([id, s]) => [id, s.operation ? operationOn(s, ymd) : "AUTOMATIC", Boolean(s.plan?.[ymd])]) : [];
    const words = `${ymdLabel(ymd)}${n === todayN ? ", today" : ""}: ${!inRange ? (n < todayN ? "already past" : "past the plan") : marks.length
      ? marks.map(([id, op]) => `column ${id} ${OPERATION_SHORT[op]}`).join(", ") : "no column on"}`;
    cells.push(`<li class="dp-day${n === todayN ? " is-today" : ""}${inRange ? "" : " is-out"}"><span class="s-visually-hidden">${escapeHtml(words)}</span>` +
      `<span class="dp-num" aria-hidden="true">${d}</span><span class="dp-cols" aria-hidden="true">${marks.map(([id, op, set]) =>
        `<span class="dp-col ${op === "MANUAL" ? "is-manual" : "is-auto"}${set ? " is-set" : ""}">${id}</span>`).join("")}</span></li>`);
  }
  const dows = Array.from({ length: 7 }, (_, i) => weekdayShort(dayNumberToYmd(ymdToDayNumber(20261101) + i)));
  // Beside the month: each column in words, its default and the days that differ from it.
  const side = ["A", "B", "C"].map(id => {
    const s = zoneSettings(id);
    const name = activeZones.find(z => z.id === id)?.name || `Column ${id}`;
    let words;
    if (!s.known) words = "Not reported yet";
    else if (!s.enabled) words = "Switched off: no watering";
    else if (!s.operation) words = "Automatic every day";
    else {
      const other = s.operation === "MANUAL" ? "AUTOMATIC" : "MANUAL";
      const days = plannedDays(s.plan, today).filter(d => operationOn(s, d) === other);
      words = `${OPERATION_SHORT[s.operation]}${days.length
        ? `; ${OPERATION_SHORT[other]} on ${days.slice(0, 4).map(ymdLabel).join(", ")}${days.length > 4 ? ` and ${days.length - 4} more` : ""}`
        : " every day"}`;
    }
    const op = !s.known || !s.enabled ? "off" : s.operation === "MANUAL" ? "manual" : "auto";
    return `<li class="dp-side-row" data-op="${op}"><span class="dp-col ${op === "manual" ? "is-manual" : op === "auto" ? "is-auto" : "is-off"}" aria-hidden="true">${id}</span>` +
      `<span><strong>${escapeHtml(name)}</strong><span class="dp-side-words">Column ${id} · ${escapeHtml(words)}</span></span></li>`;
  }).join("");
  box.innerHTML = `<div class="dp-main"><div class="plan-cal-head">${nav(-1, dashPlanMonth > monthOfYmd(today), "Previous month")}<span class="plan-month">${escapeHtml(title)}</span>${nav(1, dashPlanMonth < lastMonth, "Next month")}</div>` +
    `<ol class="dp-grid">${dows.map(w => `<li class="dp-dow" aria-hidden="true">${escapeHtml(w)}</li>`).join("")}${cells.join("")}</ol>` +
    `<p class="dp-legend"><span class="dp-col is-auto">A</span>Automatic: waters when dry <span class="dp-col is-manual">A</span>Manual: no scheduled watering <span class="dp-legend-note">The letter is the column; today has a ring.</span></p></div>` +
    `<ul class="dp-side" aria-label="Each column's plan">${side}</ul>`;
}
document.getElementById("sPlanCal")?.addEventListener("click", event => {
  const nav = event.target.closest("button[data-dash-plan]");
  if (!nav || nav.disabled) return;
  dashPlanMonth = shiftMonth(dashPlanMonth, Number(nav.dataset.dashPlan));
  renderDashPlan();
  document.querySelector(`#sPlanCal [data-dash-plan="${nav.dataset.dashPlan}"]`)?.focus();
});

/* The system picture (both modes): reservoir → transfer pump → mixing tank (the nutrient and pH bottles above it, the
 * mixer inside) → booster pump → columns. Drawn in code, in two layouts (wide, and tall for phones), and driven by ESP1's
 * pump flags: water moves along a pipe only while ESP1's fresh report says that pump is running, and nothing moves at all
 * on an old report. The water is blue in both tanks, and its surface is simulated (surfStep()), not a looping animation. */
// Below this width the wide picture's words get too small to read (tablets included), so the tall one is drawn.
const SIMPLE_DIAGRAM_NARROW_PX = 700;
// The bottles over the mixing tank (18 Nov 2026, the owner: "add a ph up and ph down ... indicate the names of the
// nutrients"). The names are the project's own (system reference, Nutrient System). Nutrient D (magnesium sulfate) is
// left out on purpose: runs never dose it. ESP1 doses A, B and C in a fertigation run; the pH pumps only run in hand tests.
const DIAGRAM_BOTTLES = [
  { key: "A", tag: "A", kind: "nut", name: ["Calcium", "nitrate"] },
  { key: "B", tag: "B", kind: "nut", name: ["Ammonium", "phosphate"] },
  { key: "C", tag: "C", kind: "nut", name: ["Potassium", "nitrate"] },
  { key: "phUp", tag: "+", kind: "phup", name: ["pH Up"] },
  { key: "phDn", tag: "−", kind: "phdn", name: ["pH Down"] }
];
const NUTRIENT_WORDS = { A: "Calcium nitrate (A)", B: "Ammonium phosphate (B)", C: "Potassium nitrate (C)" };
const SIMPLE_LAYOUTS = {
  wide: {
    w: 1000, h: 340,
    res: { x: 30, y: 120, w: 150, h: 170 },
    tp: { cx: 262, cy: 258 },
    mix: { x: 380, y: 130, w: 180, h: 160, mixerDx: 34 },
    bottles: { xs: [330, 405, 480, 555, 630], y: 6, gap: 13, dosingY: 104, inletX: 500 },
    bp: { cx: 640, cy: 258 },
    manifoldY: 150, nozzleY: 178,
    cols: { xs: [700, 805, 910], w: 80, y: 232, h: 62 },
    pipes: {
      resOut: "M180 258 H238",
      transferOut: "M286 258 H380",
      mixOut: "M560 258 H616",
      boosterOut: "M640 234 V150 H950",
      dropA: "M740 150 V178", dropB: "M845 150 V178", dropC: "M950 150 V178"
    },
    // Where each pump's name and state go, from its centre.
    labels: { transfer: { dx: 0, dy: 44, anchor: "middle" }, booster: { dx: 0, dy: 44, anchor: "middle" } },
    ground: "M0 306 C 120 296 240 300 360 304 S 620 298 760 302 S 920 296 1000 300 V340 H0 Z",
    waveMax: 7
  },
  narrow: {
    w: 360, h: 640,
    res: { x: 14, y: 124, w: 106, h: 150 },
    tp: { cx: 160, cy: 240 },
    mix: { x: 200, y: 124, w: 146, h: 150, mixerDx: 24 },
    bottles: { xs: [38, 108, 178, 248, 318], y: 4, gap: 12, dosingY: 100, inletX: 300 },
    bp: { cx: 273, cy: 332 },
    manifoldY: 394, nozzleY: 422,
    cols: { xs: [18, 133, 248], w: 94, y: 474, h: 70 },
    pipes: {
      resOut: "M120 240 H136",
      transferOut: "M184 240 H200",
      mixOut: "M273 274 V308",
      boosterOut: "M273 356 V394 H65 M273 394 H295",
      dropA: "M65 394 V422", dropB: "M180 394 V422", dropC: "M295 394 V422"
    },
    // The booster's words sit to its left, clear of the pipe running down from it.
    labels: { transfer: { dx: 0, dy: 44, anchor: "middle" }, booster: { dx: -34, dy: -4, anchor: "end" } },
    ground: "M0 590 C 90 580 180 584 270 588 S 340 582 360 584 V640 H0 Z",
    waveMax: 6
  }
};
// seen: which levels have had a reading since the picture was drawn. The first one is set at once, with no long rise
// from empty; later changes move smoothly.
const sDiagram = { layout: "", els: null, seen: new Set(), fresh: false };
function sInstant(el, apply) {
  el.style.transition = "none";
  apply();
  el.getBoundingClientRect();            // commit it now, so the transition only covers later changes
  el.style.transition = "";
}

/* ---- The water's surface (18 Nov 2026, the owner: "make the water blue, and it must wave with physics") ----------------
 * Each tank's surface is a row of SURF_N points joined by springs: every point is pulled back to the level and towards
 * its neighbours, and slowed by friction (the usual damped wave equation for a water surface), so a push in one place
 * travels across, bounces off the walls and dies away. What pushes it comes from ESP1's report: water pouring in
 * (transfer pump), water drawn off (transfer or booster), the mixer stirring, nutrient drops, a level change sloshing it,
 * and a faint breeze. It moves only on a fresh report, pauses while the page is hidden or the picture is off screen, and
 * lies still for anyone who asks for reduced motion. */
const SURF_N = 28;
const SURF = { k: 24, damp: 2.6, spread: 520, step: 1 / 120 };
const surfState = { frame: 0, last: 0, visible: true, sims: {} };
function surfSim(key, t, max) {
  const inner = { x: t.x + 6, y: t.y + 6, w: t.w - 12, h: t.h - 12 };
  return { key, inner, max, ys: new Float32Array(SURF_N), vs: new Float32Array(SURF_N), force: new Float32Array(SURF_N),
           act: {}, pct: null, breeze: 0, drip: 0, clock: Math.random() * 10, shaft: 0.5, inlet: 0.5 };
}
// One frame's pushes, in px/s² per point (continuous sources) or px/s (single kicks).
function surfPushes(s, dt) {
  const a = s.act;
  const f = s.force;
  f.fill(0);
  const calm = s.pct === null ? 0 : Math.min(1, s.pct / 10);   // an almost empty tank can't make waves
  if (!calm) return;
  const at = frac => Math.max(0, Math.min(SURF_N - 1, Math.round(frac * (SURF_N - 1))));
  const kick = (i, v) => { if (i >= 0 && i < SURF_N) s.vs[i] += v * calm; };
  const rand = () => Math.random() * 2 - 1;
  s.clock += dt;
  // A faint breeze: a small random push now and then, so still water never looks frozen.
  s.breeze -= dt;
  if (s.breeze <= 0) { s.breeze = 0.18 + Math.random() * 0.3; kick(Math.floor(Math.random() * SURF_N), rand() * 45); }
  // Water pouring in at the bottom left: it wells up and churns that side.
  if (a.inflow) for (let i = 0; i < 5; i++) f[i] += (rand() * 2300 + 520 * Math.sin(s.clock * 9 + i)) * calm;
  // Water drawn off at the bottom right: a gentle stir over that side.
  if (a.outflow) for (let i = SURF_N - 4; i < SURF_N; i++) f[i] += rand() * 900 * calm;
  // The mixer: a strong stir round its shaft, with a slow swing back and forth across the tank.
  if (a.mixer) {
    const c = at(s.shaft);
    for (let i = 0; i < SURF_N; i++) {
      const near = Math.max(0, 1 - Math.abs(i - c) / 6);
      f[i] += (rand() * 1200 * near + 400 * Math.sin(s.clock * 3.1 + i * 0.45)) * calm;
    }
  }
  // Nutrient drops at the inlet: one small splash at a time.
  if (a.dosing) {
    s.drip -= dt;
    if (s.drip <= 0) { s.drip = 0.32; const i = at(s.inlet); kick(i, -55); kick(i - 1, 18); kick(i + 1, 18); }
  }
}
function surfStep(s, dt) {
  if (dt <= 0) return;
  surfPushes(s, dt);
  const { ys, vs, force } = s;
  for (let left = dt; left > 1e-6; left -= SURF.step) {
    const h = Math.min(SURF.step, left);
    for (let i = 0; i < SURF_N; i++) {
      const l = ys[i > 0 ? i - 1 : i], r = ys[i < SURF_N - 1 ? i + 1 : i];
      vs[i] += (-SURF.k * ys[i] - SURF.damp * vs[i] + SURF.spread * (l + r - 2 * ys[i]) + force[i]) * h;
    }
    for (let i = 0; i < SURF_N; i++) {
      ys[i] += vs[i] * h;
      if (ys[i] > s.max) { ys[i] = s.max; vs[i] *= -0.3; }
      else if (ys[i] < -s.max) { ys[i] = -s.max; vs[i] *= -0.3; }
    }
  }
}
// A level change sloshes it: one side up, the other down, more for a bigger change.
function surfSlosh(s, change) {
  const push = Math.max(-1, Math.min(1, change / 6)) * 60;
  for (let i = 0; i < SURF_N; i++) s.vs[i] += push * (i / (SURF_N - 1) - 0.5) * 2;
}
// The surface as a smooth curve through the points, filled down past the tank's bottom (the tank clips it).
function surfacePath(inner, ys, offset = 0, mirror = false, scale = 1) {
  const n = ys.length;
  const x = i => inner.x - 2 + (inner.w + 4) * i / (n - 1);
  const y = i => offset + (ys[mirror ? n - 1 - i : i] || 0) * scale;
  let d = `M${x(0).toFixed(1)} ${y(0).toFixed(2)}`;
  for (let i = 1; i < n; i++) {
    d += ` Q${x(i - 1).toFixed(1)} ${y(i - 1).toFixed(2)} ${((x(i - 1) + x(i)) / 2).toFixed(1)} ${((y(i - 1) + y(i)) / 2).toFixed(2)}`;
  }
  return `${d} L${x(n - 1).toFixed(1)} ${y(n - 1).toFixed(2)} V${inner.h + 8} H${(inner.x - 2).toFixed(1)} Z`;
}
function surfDraw() {
  const svg = sDiagram.els?.svg;
  if (!svg) return;
  Object.values(surfState.sims).forEach(s => {
    const front = svg.querySelector(`[data-s-surf="${s.key}"]`);
    const back = svg.querySelector(`[data-s-surfback="${s.key}"]`);
    // The back layer: the same water seen further off, a little higher, its ripples mirrored and softer.
    if (back) back.setAttribute("d", surfacePath(s.inner, s.ys, -4, true, 0.7));
    if (front) front.setAttribute("d", surfacePath(s.inner, s.ys, 0));
  });
}
function surfShouldRun() {
  return Boolean(sDiagram.els) && sDiagram.fresh && surfState.visible && !document.hidden && !reduceMotionQuery?.matches;
}
function surfLoop(time) {
  surfState.frame = 0;
  const dt = surfState.last ? Math.min(0.05, (time - surfState.last) / 1000) : 0;
  surfState.last = time;
  Object.values(surfState.sims).forEach(s => surfStep(s, dt));
  surfDraw();
  if (surfShouldRun()) surfState.frame = requestAnimationFrame(surfLoop);
}
// Starts or stops the water to match whether it should move and anyone can see it. Stopped, it lies flat.
function surfWake() {
  if (surfShouldRun()) {
    if (!surfState.frame) { surfState.last = 0; surfState.frame = requestAnimationFrame(surfLoop); }
    return;
  }
  if (surfState.frame) cancelAnimationFrame(surfState.frame);
  surfState.frame = 0;
  Object.values(surfState.sims).forEach(s => { s.ys.fill(0); s.vs.fill(0); });
  surfDraw();
}

function simpleTankSvg(key, t, name) {
  const inner = { x: t.x + 6, y: t.y + 6, w: t.w - 12, h: t.h - 12 };
  const flat = offset => `M${inner.x - 2} ${offset} H${inner.x + inner.w + 2} V${inner.h + 8} H${inner.x - 2} Z`;
  // The reservoir pause (18 % at least; adjustable since ESP1's firmware of 18 Nov 2026 b): updateSimpleDiagram() moves it.
  const lowY = (inner.y + inner.h * (1 - RIG_RES_LOW_PCT / 100)).toFixed(1);
  const low = key === "res" ? `<line class="s-dg-low" data-s-low x1="${t.x + 8}" x2="${t.x + t.w - 8}" y1="${lowY}" y2="${lowY}"/><text class="s-dg-note" data-s-lowtext x="${t.x + t.w - 10}" y="${(Number(lowY) - 5).toFixed(1)}" text-anchor="end">${RIG_RES_LOW_PCT} %</text>` : "";
  // The mixer: a motor on the lid, its shaft, and a propeller near the bottom that turns (seen from the side, it flips)
  // inside a swirl while ESP1 reports it running.
  const sx = t.x + (t.mixerDx || 0);
  const py = t.y + t.h - 30;
  const mixer = key === "mix" ? `<g class="s-dg-mixer">
        <rect class="s-dg-motor" x="${sx - 9}" y="${t.y - 12}" width="18" height="14" rx="3"/>
        <line class="s-dg-shaft" x1="${sx}" y1="${t.y + 2}" x2="${sx}" y2="${py}"/>
        <g transform="translate(${sx} ${py})"><g class="s-dg-swirl"><path d="M-20 0 a20 20 0 0 1 20 -20"/><path d="M20 0 a20 20 0 0 1 -20 20"/></g></g>
        <g transform="translate(${sx} ${py})"><g class="s-dg-prop"><path d="M-14 0 C -10 -6 -3 -6 0 0 C 3 6 10 6 14 0 C 10 -6 3 -6 0 0 C -3 6 -10 6 -14 0Z"/></g></g>
      </g>` : "";
  return `<g class="s-dg-tank s-dg-${key}" data-s-tank="${key}">
      <clipPath id="sClip-${key}"><rect x="${inner.x}" y="${inner.y}" width="${inner.w}" height="${inner.h}" rx="10"/></clipPath>
      <rect class="s-dg-tank-body" x="${t.x}" y="${t.y}" width="${t.w}" height="${t.h}" rx="16"/>
      <g clip-path="url(#sClip-${key})">
        <g class="s-dg-level" data-s-level="${key}" style="transform: translateY(${inner.h}px)">
          <g transform="translate(0 ${inner.y})">
            <path class="s-dg-water-back" data-s-surfback="${key}" d="${flat(-4)}"/>
            <path class="s-dg-water" data-s-surf="${key}" d="${flat(0)}"/>
          </g>
        </g>
      </g>
      ${low}${mixer}
      <text class="s-dg-tank-name" x="${t.x + t.w / 2}" y="${t.y + 24}" text-anchor="middle">${name}</text>
      <text class="s-dg-tank-value" x="${t.x + t.w / 2}" y="${t.y + 58}" text-anchor="middle" data-s-value="${key}">--</text>
      <text class="s-dg-tank-litres" x="${t.x + t.w / 2}" y="${t.y + 78}" text-anchor="middle" data-s-litres="${key}"></text>
    </g>`;
}

// Four blades, so the impeller's box is centred on its hub and it turns in place (transform-box: fill-box).
function simplePumpSvg(key, p, name, label) {
  const blade = angle => `<path d="M0 -15 C 6 -10 6 -4 0 0 C -6 -4 -6 -10 0 -15Z" transform="rotate(${angle})"/>`;
  return `<g class="s-dg-pump" data-s-pump="${key}" transform="translate(${p.cx} ${p.cy})">
      <circle class="s-dg-pump-body" r="24"/>
      <g class="s-dg-impeller">${[0, 90, 180, 270].map(blade).join("")}<circle r="3.5"/></g>
      <text class="s-dg-label" x="${label.dx}" y="${label.dy}" text-anchor="${label.anchor}">${name}</text>
      <text class="s-dg-state" x="${label.dx}" y="${label.dy + 17}" text-anchor="${label.anchor}" data-s-pumpstate="${key}">--</text>
    </g>`;
}

// The bottles in a row over the mixing tank, each with its name over it; their lines join one dosing line that runs
// into the tank's top.
function simpleBottlesSvg(L) {
  const B = L.bottles;
  const y0 = B.y;
  const first = Math.min(...B.xs), last = Math.max(...B.xs);
  const lines = `<path class="s-dg-dose-line" d="M${first} ${B.dosingY} H${last}"/>` +
    `<path class="s-dg-dose-line s-dg-inlet" data-s-inlet d="M${B.inletX} ${B.dosingY} V${L.mix.y}"/>`;
  return lines + DIAGRAM_BOTTLES.map((b, i) => {
    const x = B.xs[i];
    const name = b.name.length > 1
      ? `<text class="s-dg-bottle-name" x="${x}" y="${y0 + 10}" text-anchor="middle">${b.name[0]}</text><text class="s-dg-bottle-name" x="${x}" y="${y0 + 10 + B.gap}" text-anchor="middle">${b.name[1]}</text>`
      : `<text class="s-dg-bottle-name" x="${x}" y="${y0 + 10 + B.gap}" text-anchor="middle">${b.name[0]}</text>`;
    return `<g class="s-dg-bottle is-${b.kind}" data-s-bottle="${b.key}">
        ${name}
        <rect class="s-dg-bottle-cap" x="${x - 6}" y="${y0 + 28}" width="12" height="8" rx="2"/>
        <path class="s-dg-bottle-body" d="M${x - 11} ${y0 + 40} q0 -4 4 -4 h14 q4 0 4 4 v20 a4 4 0 0 1 -4 4 h-14 a4 4 0 0 1 -4 -4 z"/>
        <text class="s-dg-bottle-tag" x="${x}" y="${y0 + 56}" text-anchor="middle">${b.tag}</text>
        <path class="s-dg-drip-line" d="M${x} ${y0 + 66} V${B.dosingY}"/>
        <circle class="s-dg-drip" cx="${x}" cy="${y0 + 70}" r="3"/>
      </g>`;
  }).join("");
}

function buildSimpleDiagram(name) {
  const host = document.getElementById("sDiagram");
  if (!host) return;
  const L = SIMPLE_LAYOUTS[name];
  sDiagram.layout = name;
  const pipe = (key, d) => `<path class="s-dg-pipe" d="${d}"/><path class="s-dg-flow" data-s-flow="${key}" d="${d}"/>`;
  const cols = ["A", "B", "C"].map((id, i) => {
    const x = L.cols.xs[i];
    const cx = x + L.cols.w / 2;
    const top = L.cols.y;
    const plants = [-24, 0, 24].map((dx, k) => {
      const px = cx + dx;
      const hgt = k === 1 ? 24 : 18;
      return `<g class="s-dg-plant"><path class="s-dg-stem" d="M${px} ${top} V${top - hgt}"/>` +
        `<path class="s-dg-leaf" d="M${px} ${top - hgt * .4} c -11 -2 -16 -9 -16 -17 c 9 0 15 7 16 17z"/>` +
        `<path class="s-dg-leaf s-dg-leaf2" d="M${px} ${top - hgt * .72} c 11 -2 16 -9 16 -17 c -9 0 -15 7 -16 17z"/>` +
        `<path class="s-dg-leaf" d="M${px} ${top - hgt} c -4 -5 -4 -10 0 -14 c 4 4 4 9 0 14z"/></g>`;
    }).join("");
    const drops = [-14, 0, 14].map((dx, k) => `<circle class="s-dg-drop" style="animation-delay:${(k * .28).toFixed(2)}s" cx="${cx + dx}" cy="${L.nozzleY + 8}" r="3"/>`).join("");
    return `<g class="s-dg-col" data-s-col="${id}">
        <path class="s-dg-valve" d="M${cx - 7} ${L.nozzleY - 16} l14 8 v-8 l-14 8z" data-s-valve="${id}"/>
        <path class="s-dg-nozzle" d="M${cx - 8} ${L.nozzleY} h16 l-3 5 h-10z"/>
        <g class="s-dg-drops" data-s-drops="${id}">${drops}</g>
        ${plants}
        <clipPath id="sClipCol${id}"><rect x="${x + 4}" y="${top + 4}" width="${L.cols.w - 8}" height="${L.cols.h - 8}" rx="6"/></clipPath>
        <rect class="s-dg-bed" x="${x}" y="${top}" width="${L.cols.w}" height="${L.cols.h}" rx="10"/>
        <g clip-path="url(#sClipCol${id})"><rect class="s-dg-moist" data-s-moist="${id}" x="${x + 4}" y="${top + 4}" width="${L.cols.w - 8}" height="${L.cols.h - 8}" style="transform: scaleY(0)"/></g>
        <line class="s-dg-start" data-s-start="${id}" x1="${x + 6}" x2="${x + L.cols.w - 6}" y1="${top}" y2="${top}"/>
        <line class="s-dg-wetline" data-s-wetline="${id}" x1="${x + 6}" x2="${x + L.cols.w - 6}" y1="${top}" y2="${top}"/>
        <rect class="s-dg-col-pill" x="${cx - 28}" y="${top + L.cols.h / 2 - 12}" width="56" height="24" rx="12"/>
        <text class="s-dg-col-value" x="${cx}" y="${top + L.cols.h / 2 + 5}" text-anchor="middle" data-s-colvalue="${id}">--</text>
        <text class="s-dg-label" x="${cx}" y="${top + L.cols.h + 20}" text-anchor="middle">Column ${id}</text>
      </g>`;
  }).join("");
  host.innerHTML = `<svg class="s-dg s-dg-${name}" viewBox="0 0 ${L.w} ${L.h}" role="img" aria-labelledby="sDiagramDesc">
      <title id="sDiagramDesc">The irrigation system</title>
      <path class="s-dg-ground" d="${L.ground}"/>
      ${Object.entries(L.pipes).filter(([k]) => !k.startsWith("drop")).map(([k, d]) => pipe(k, d)).join("")}
      ${["A", "B", "C"].map(id => pipe(`drop${id}`, L.pipes[`drop${id}`])).join("")}
      ${simpleTankSvg("res", L.res, "Reservoir")}
      ${simpleTankSvg("mix", L.mix, "Mixing tank")}
      ${simpleBottlesSvg(L)}
      ${simplePumpSvg("transfer", L.tp, "Transfer pump", L.labels.transfer)}
      ${simplePumpSvg("booster", L.bp, "Booster pump", L.labels.booster)}
      ${cols}
    </svg>`;
  const svg = host.querySelector("svg");
  sDiagram.els = { svg, desc: svg.querySelector("#sDiagramDesc") };
  sDiagram.seen.clear();
  // A fresh surface for each tank in this layout; the mixer's shaft and the dosing inlet, as a share of the width.
  const mixInner = L.mix.w - 12;
  surfState.sims = { res: surfSim("res", L.res, L.waveMax), mix: surfSim("mix", L.mix, L.waveMax) };
  surfState.sims.mix.shaft = (L.mix.mixerDx - 6) / mixInner;
  surfState.sims.mix.inlet = (L.bottles.inletX - L.mix.x - 6) / mixInner;
}

function updateSimpleDiagram(fresh, stale) {
  const host = document.getElementById("sDiagram");
  if (!host) return;
  if (!sDiagram.els) buildSimpleDiagram(host.clientWidth && host.clientWidth < SIMPLE_DIAGRAM_NARROW_PX ? "narrow" : "wide");
  const svg = sDiagram.els?.svg;
  if (!svg) return;
  const L = SIMPLE_LAYOUTS[sDiagram.layout];
  const sensors = liveData.sensors || {};
  const actuators = liveData.actuators || {};
  const run = liveData.diagnostics?.runProgress || {};
  const known = Boolean(liveData.meta?.updatedAt);
  // Only a fresh report can show anything moving (the same rule as the pump lamps).
  const transfer = fresh && Boolean(actuators.transferRunning);
  const booster = fresh && Boolean(actuators.boosterRunning);
  const mixer = fresh && Boolean(actuators.mixerRunning);
  const dosing = fresh && Boolean(run.active) && run.stage === "Pump Nutrients";
  const doseNow = dosing ? ["A", "B", "C"].find(k => Number(run.dosesMl?.[k]?.target) > Number(run.dosesMl?.[k]?.delivered || 0)) || null : null;
  const zone = String(run.zone || "").toUpperCase();
  svg.classList.toggle("is-fresh", fresh);
  svg.classList.toggle("is-stale", stale);
  sDiagram.fresh = fresh;
  // Water levels, litres, and what is stirring each tank's surface.
  [["res", sensors.reservoirLevel, sensors.reservoirLiters, L.res], ["mix", sensors.mixingLevel, sensors.mixingLiters, L.mix]].forEach(([key, pct, litres, t]) => {
    const level = svg.querySelector(`[data-s-level="${key}"]`);
    const innerH = t.h - 12;
    const has = hasValue(pct);
    const frac = has ? Math.min(1, Math.max(0, Number(pct) / 100)) : 0;
    const place = () => { level.style.transform = `translateY(${(innerH * (1 - frac)).toFixed(1)}px)`; };
    if (level && has && !sDiagram.seen.has(key)) { sDiagram.seen.add(key); sInstant(level, place); }
    else if (level) place();
    const value = svg.querySelector(`[data-s-value="${key}"]`);
    if (value) value.textContent = has ? `${Math.round(Number(pct))} %` : "No reading";
    const litresText = svg.querySelector(`[data-s-litres="${key}"]`);
    if (litresText) litresText.textContent = has && hasValue(litres) ? `${numberText(litres, 1, "L")}` : "";
    svg.querySelector(`[data-s-tank="${key}"]`)?.classList.toggle("is-active", key === "res" ? transfer : (transfer || booster || mixer || Boolean(doseNow)));
    const sim = surfState.sims[key];
    if (sim) {
      const next = has ? Number(pct) : null;
      if (fresh && next !== null && sim.pct !== null && Math.abs(next - sim.pct) >= 0.5) surfSlosh(sim, next - sim.pct);
      sim.pct = next;
      sim.act = key === "res" ? { outflow: transfer } : { inflow: transfer, outflow: booster, mixer, dosing: Boolean(doseNow) };
    }
  });
  // The reservoir pause line, where ESP1 has it.
  const pause = resPausePercent();
  const lowLine = svg.querySelector("[data-s-low]");
  if (lowLine) {
    const y = (L.res.y + 6 + (L.res.h - 12) * (1 - pause / 100)).toFixed(1);
    lowLine.setAttribute("y1", y);
    lowLine.setAttribute("y2", y);
    const lowText = svg.querySelector("[data-s-lowtext]");
    if (lowText) { lowText.setAttribute("y", (Number(y) - 5).toFixed(1)); lowText.textContent = `${pause} %`; }
  }
  svg.querySelector('[data-s-tank="res"]')?.classList.toggle("is-low", hasValue(sensors.reservoirLevel) && Number(sensors.reservoirLevel) < pause);
  svg.querySelector(".s-dg-mixer")?.classList.toggle("on", mixer);
  // Pumps: a word under each, so the state never rests on colour alone.
  [["transfer", transfer], ["booster", booster]].forEach(([key, on]) => {
    svg.querySelector(`[data-s-pump="${key}"]`)?.classList.toggle("on", on);
    const state = svg.querySelector(`[data-s-pumpstate="${key}"]`);
    if (state) state.textContent = !known ? "--" : !fresh ? "unknown" : on ? "ON" : "off";
  });
  const flowing = new Set();
  if (transfer) { flowing.add("resOut"); flowing.add("transferOut"); }
  if (booster) { flowing.add("mixOut"); flowing.add("boosterOut"); ["A", "B", "C"].filter(id => zone.includes(id)).forEach(id => flowing.add(`drop${id}`)); }
  svg.querySelectorAll("[data-s-flow]").forEach(el => el.classList.toggle("on", flowing.has(el.dataset.sFlow)));
  svg.querySelectorAll("[data-s-bottle]").forEach(el => el.classList.toggle("on", el.dataset.sBottle === doseNow));
  svg.querySelector("[data-s-inlet]")?.classList.toggle("on", Boolean(doseNow));
  // Columns: moisture from the bottom, with each column's Soil start (amber, it waters below) and Too wet (blue) lines.
  const words = [];
  ["A", "B", "C"].forEach(id => {
    const z = sensors.zones?.[id] || {};
    const off = z.enabled === false;
    const has = !off && hasValue(z.moisture);
    const pct = has ? Math.min(100, Math.max(0, Number(z.moisture))) : 0;
    const lines = zoneSettings(id);
    const group = svg.querySelector(`[data-s-col="${id}"]`);
    group?.classList.toggle("off", off);
    group?.classList.toggle("dry", has && pct < lines.startPct);
    group?.classList.toggle("wet", has && pct >= lines.wetPct);
    const moist = svg.querySelector(`[data-s-moist="${id}"]`);
    const fill = () => { moist.style.transform = `scaleY(${(pct / 100).toFixed(3)})`; };
    if (moist && has && !sDiagram.seen.has(`col${id}`)) { sDiagram.seen.add(`col${id}`); sInstant(moist, fill); }
    else if (moist) fill();
    [["start", lines.startPct], ["wetline", lines.wetPct]].forEach(([kind, v]) => {
      const line = svg.querySelector(`[data-s-${kind}="${id}"]`);
      if (!line) return;
      const y = (L.cols.y + 4 + (L.cols.h - 8) * (1 - Math.min(100, Math.max(0, v)) / 100)).toFixed(1);
      line.setAttribute("y1", y);
      line.setAttribute("y2", y);
    });
    const value = svg.querySelector(`[data-s-colvalue="${id}"]`);
    if (value) value.textContent = off ? "off" : has ? `${Math.round(pct)} %` : "--";
    svg.querySelector(`[data-s-valve="${id}"]`)?.classList.toggle("open", booster && zone.includes(id));
    svg.querySelector(`[data-s-drops="${id}"]`)?.classList.toggle("on", booster && zone.includes(id));
    words.push(`column ${id} ${off ? "switched off" : has ? `${Math.round(pct)} % moist${pct >= lines.wetPct ? ", too wet" : ""}` : "no reading"}`);
  });
  const note = !known ? "Waiting for the controller's report."
    : !fresh ? "The controller isn't reporting, so this is its last report and nothing is shown moving."
    : booster ? `Water is flowing from the mixing tank to column ${columnList([...zone].filter(ch => "ABC".includes(ch))) || "?"}.`
    : mixer ? "The mixer is stirring the water and nutrients."
    : doseNow ? `${NUTRIENT_WORDS[doseNow]} is being added to the mixing tank.`
    : transfer ? "Water is flowing from the reservoir into the mixing tank."
    : run.active && (run.phase === "starting" || run.phase === "running") ? `${simpleStageText(run)}.`
    : simpleTesting() ? "Someone is testing by hand: a pump may run without showing here."
    : "Nothing is moving: all pumps are off.";
  sSet("sDiagramNote", note);
  const tank = key => hasValue(sensors[key]) ? `${Math.round(Number(sensors[key]))} %` : "no reading";
  if (sDiagram.els.desc) sDiagram.els.desc.textContent = `The irrigation system: reservoir ${tank("reservoirLevel")}, mixing tank ${tank("mixingLevel")}, nutrient bottles (calcium nitrate, ammonium phosphate, potassium nitrate) and pH Up and pH Down above the mixing tank, ${words.join(", ")}. ${note}`;
  surfWake();
}

function initSimpleDiagram() {
  const host = document.getElementById("sDiagram");
  if (!host) return;
  const pick = () => {
    if (!host.clientWidth) return;   // hidden (another tab): keep what is there
    const name = host.clientWidth < SIMPLE_DIAGRAM_NARROW_PX ? "narrow" : "wide";
    if (name === sDiagram.layout) return;
    buildSimpleDiagram(name);
    updateSimpleDiagram(deviceIsFresh(), Boolean(liveData.meta?.updatedAt) && !deviceIsFresh());
  };
  if (window.ResizeObserver) new ResizeObserver(pick).observe(host);
  else window.addEventListener("resize", pick);
  // The water only moves while the picture is on screen.
  if (window.IntersectionObserver) {
    new IntersectionObserver(entries => { surfState.visible = entries.some(e => e.isIntersecting); surfWake(); }).observe(host);
  }
  // Nothing animates while the page is hidden.
  const syncHidden = () => { document.documentElement.classList.toggle("page-hidden", document.hidden); surfWake(); };
  document.addEventListener("visibilitychange", syncHidden);
  reduceMotionQuery?.addEventListener?.("change", surfWake);
  syncHidden();
}

/* Zone profiles (both modes since the owner's second round, 18 Nov 2026): each card leads with the zone's name, the big
 * moisture figure and its state in words, over a bar that shows where it waters (Soil start) and where it is too wet.
 * The readings show by default and can be hidden; that choice is kept here, because renderZonesUI() rebuilds the cards.
 * Editing is untouched. */
const zoneReadingsHidden = {};
function zoneHeroHtml(id) {
  return `
      <div class="s-zone-hero" data-tone="none" id="sZoneHero${id}">
        <div class="s-zone-figure">
          <span class="s-zone-label">Soil moisture</span>
          <span class="s-zone-pct s-roll" id="sZonePct${id}">--</span>
          <span class="s-pill" id="sZoneState${id}">No reading</span>
          <span class="s-stale-tag" id="sZoneStale${id}" hidden>last known</span>
        </div>
        <div class="s-zone-words">
          <p class="s-zone-say" id="sZoneSay${id}">Waiting for the controller's report.</p>
          <div class="s-zone-scale" aria-hidden="true">
            <span class="s-zone-band s-zone-band-dry" id="sZoneBandDry${id}"></span>
            <span class="s-zone-band s-zone-band-wet" id="sZoneBandWet${id}"></span>
            <span class="s-zone-scale-fill" id="sZoneFill${id}"></span>
            <span class="s-zone-mark s-zone-mark-start" id="sZoneStart${id}"></span>
            <span class="s-zone-mark s-zone-mark-wet" id="sZoneWet${id}"></span>
            <span class="s-zone-mark s-zone-mark-crop" id="sZoneTarget${id}"></span>
          </div>
          <p class="s-zone-legend"><span class="s-key s-key-start"></span>Waters below <span id="sZoneStartPct${id}">--</span><span class="s-key s-key-wet"></span>Too wet above <span id="sZoneWetPct${id}">--</span><span class="s-key s-key-crop"></span>Crop <span id="sZoneTargetPct${id}">--</span></p>
        </div>
      </div>`;
}

// A strip of field along the bottom of each card (the owner: "trees and grass ... more colour"): rolling ground, grass,
// young plants and trees, drawn here, a little different for each column. Decorative only.
function zoneArtSvg(id) {
  const k = { A: 0, B: 1, C: 2 }[id] ?? 0;
  // Low enough to show whole on a wide card, where only the bottom ~50 of the 90 units are in view.
  const trees = [[[470, 60, 9], [512, 55, 12], [552, 60, 9], [586, 57, 11]], [[462, 58, 10], [505, 61, 8], [545, 55, 12], [584, 60, 9]], [[476, 61, 8], [515, 56, 11], [556, 59, 10], [588, 61, 8]]][k];
  const tree = ([x, y, r]) => `<g class="za-tree"><path class="za-trunk" d="M${x} ${y + r * .6} V${Math.min(86, y + r + 16)}"/>` +
    `<circle class="za-crown" cx="${x}" cy="${y}" r="${r}"/><circle class="za-crown2" cx="${x - r * .55}" cy="${y + r * .35}" r="${(r * .62).toFixed(1)}"/></g>`;
  let grass = "";
  for (let x = 4 + k * 3; x < 600; x += 11) {
    const h = 7 + ((x * 7 + k * 13) % 9);
    grass += `M${x} 90 q ${(1.5 + (x % 3)).toFixed(1)} ${-h / 2} ${(x % 2 ? 4 : -3)} ${-h} `;
  }
  const sprouts = [40, 92, 144, 196, 248].map((x, i) => {
    const y = 72 - (i % 2) * 2;
    return `<g class="za-sprout"><path d="M${x} ${y + 8} V${y}"/><path class="za-leaf" d="M${x} ${y + 2} c -7 -1 -10 -6 -10 -11 c 6 0 9 5 10 11z"/>` +
      `<path class="za-leaf2" d="M${x} ${y} c 7 -1 10 -6 10 -11 c -6 0 -9 5 -10 11z"/></g>`;
  }).join("");
  return `<svg class="zone-art-svg" viewBox="0 0 600 90" preserveAspectRatio="xMaxYMax slice" focusable="false">
      <path class="za-hill2" d="M0 58 C 90 40 190 46 280 55 S 460 38 600 47 V90 H0Z"/>
      <path class="za-hill" d="M0 72 C 120 60 230 66 330 71 S 510 60 600 67 V90 H0Z"/>
      ${trees.map(tree).join("")}
      ${sprouts}
      <path class="za-grass" d="${grass}"/>
    </svg>`;
}

function renderSimpleZone(zone) {
  const id = zone.id;
  const hero = document.getElementById(`sZoneHero${id}`);
  if (!hero) return;
  const s = simpleSoilState(id);
  const lines = zoneSettings(id);
  const stale = Boolean(liveData.meta?.updatedAt) && !deviceIsFresh();
  const start = lines.startPct;
  const wet = lines.wetPct;
  const crop = zoneCrop(zone);
  const target = crop.stage?.moisture;
  const tone = stale && s.tone === "flow" ? "none" : s.tone;
  if (hero.dataset.tone !== tone) hero.dataset.tone = tone;
  hero.classList.toggle("is-stale", stale);
  sSet(`sZonePct${id}`, s.pct === null ? "--" : `${Math.round(s.pct)} %`);
  sSet(`sZoneState${id}`, s.word);
  const staleTag = document.getElementById(`sZoneStale${id}`);
  if (staleTag) staleTag.hidden = !stale || s.pct === null;
  sSet(`sZoneSay${id}`, stale ? `From the last report: ${s.say.charAt(0).toLowerCase()}${s.say.slice(1)}` : s.say);
  sSet(`zoneCropLine${id}`, crop.crop ? `${cropLabel(crop.crop)}${crop.stage ? `, ${crop.stage.name.toLowerCase()}` : ""}` : "No crop picked");
  const pos = v => `${Math.min(100, Math.max(0, Number(v)))}%`;
  const fill = document.getElementById(`sZoneFill${id}`);
  if (fill) fill.style.width = `${s.pct === null ? 0 : s.pct}%`;
  const dry = document.getElementById(`sZoneBandDry${id}`);
  if (dry) dry.style.width = pos(start);
  const wetBand = document.getElementById(`sZoneBandWet${id}`);
  if (wetBand) wetBand.style.left = pos(wet);
  const startMark = document.getElementById(`sZoneStart${id}`);
  if (startMark) startMark.style.left = pos(start);
  const wetMark = document.getElementById(`sZoneWet${id}`);
  if (wetMark) wetMark.style.left = pos(wet);
  const targetMark = document.getElementById(`sZoneTarget${id}`);
  if (targetMark) {
    targetMark.hidden = !hasValue(target);
    if (hasValue(target)) targetMark.style.left = pos(target);
  }
  sSet(`sZoneStartPct${id}`, `${start} %`);
  sSet(`sZoneWetPct${id}`, `${wet} %`);
  sSet(`sZoneTargetPct${id}`, hasValue(target) ? `${Number(target)} %` : "not set");
}
function syncZoneReadingsFold(id) {
  const block = document.querySelector(`.zone-block[data-zone="${id}"]`);
  const hidden = Boolean(zoneReadingsHidden[id]);
  block?.classList.toggle("readings-hidden", hidden);
  const button = block?.querySelector(`[data-z-readings="${id}"]`);
  if (button) {
    button.setAttribute("aria-expanded", String(!hidden));
    button.textContent = hidden ? "Show" : "Hide";
  }
}
document.getElementById("dynamic-zones-container")?.addEventListener("click", event => {
  const button = event.target.closest("button[data-z-readings]");
  if (!button) return;
  const id = button.dataset.zReadings;
  zoneReadingsHidden[id] = !zoneReadingsHidden[id];
  syncZoneReadingsFold(id);
});

// The crop library folds away in Simple mode, and opens by itself while a crop is being added or changed.
let simpleCropOpen = false;
function syncCropFold() {
  const library = document.getElementById("cropLibrary");
  const toggle = document.getElementById("sCropToggle");
  if (!library || !toggle) return;
  const open = simpleCropOpen || Boolean(cropDraft);
  library.classList.toggle("s-open", open);
  toggle.setAttribute("aria-expanded", String(open));
  const label = toggle.querySelector("span");
  if (label) label.textContent = open ? "Hide the crop profiles" : "Show the crop profiles";
  toggle.disabled = Boolean(cropDraft);   // an editor that is open stays in view until it is saved or cancelled
}
document.getElementById("sCropToggle")?.addEventListener("click", () => {
  simpleCropOpen = !simpleCropOpen;
  syncCropFold();
});

// Controls in Simple mode: the nutrient boxes behind a toggle that always says what they hold, and Restart a board
// behind "Advanced controls". Same inputs, same buttons, same checks as Complete mode; only what is folded differs.
let simpleDoseOpen = false;
function syncDoseToggle() {
  const toggle = document.getElementById("sDoseToggle");
  const form = document.getElementById("forceRunForm");
  if (!toggle || !form) return;
  const doses = ["A", "B", "C"].map(k => [k, Number(document.getElementById(`forceDose${k}`)?.value || 0)]);
  const set = doses.filter(([, v]) => Number.isFinite(v) && v > 0);
  const open = simpleDoseOpen;
  form.classList.toggle("s-doses-open", open);
  toggle.setAttribute("aria-expanded", String(open));
  const text = set.length ? `Nutrients: ${set.map(([k, v]) => `${k} ${v} mL`).join(", ")}` : "Add nutrients (optional): now plain water";
  sSet("sDoseToggleText", `${text}${open ? "" : set.length ? " (show)" : ""}`);
}
document.getElementById("sDoseToggle")?.addEventListener("click", () => {
  simpleDoseOpen = !simpleDoseOpen;
  syncDoseToggle();
});
document.getElementById("forceRunForm")?.addEventListener("input", syncDoseToggle);
document.querySelectorAll(".s-collapsible > .s-collapse-toggle").forEach(toggle => toggle.addEventListener("click", () => {
  const panel = toggle.parentElement;
  const open = !panel.classList.contains("s-open");
  panel.classList.toggle("s-open", open);
  toggle.setAttribute("aria-expanded", String(open));
}));

initModeSwitch();
initSimpleDiagram();
syncDoseToggle();
syncDashPlanToggle();
renderZonesUI();
renderCropLibrary();
updateDashboard();
renderCommandHistory();
syncControlAvailability();
initInfoIcons();
initializeFirebase();
