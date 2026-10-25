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
 * irrigation/commands dashboard -> ESP1 (ESP1 validates every supported command)
 *
 * Commands ESP1 actually implements, and the exact payload each one expects. Kept complete on
 * purpose -- this drifted out of sync with the real command set once before (only 3 of 14 were
 * listed here for a long stretch).
 *   RUN_PUMP_TEST      { pump: "transfer" | "booster" | "mixer" }   5 s preventive exercise
 *   FORCE_RUN          { columns: "A"|"B"|"C"|"AB", liters: <=20, doseMl: {A,B,C} (each <=500),
 *                        delaySeconds: 0-300 }   no pH field exists -- see note below
 *   EMERGENCY_STOP     {}   no gating, no confirmation dialog, always actionable
 *   RECOVER            { action: "hold"|"release"|"irrigate"|"normal" }   only while a fault is held
 *   ESTOP_RECOVER      {}   only while stopped (EMERGENCY_STOP) and no fault is held
 *   ENABLE_ACTUATIONS  {}   the one escape hatch checked first, gated on nothing
 *   DISABLE_ACTUATIONS {}   monitoring keeps running
 *   CANCEL_FORCE       {}   only while a forced run is armed/counting down
 *   ACK_FAULT          {}   dashboard-only snooze; no effect on the rig
 *   SET_COLUMN         { col: "A"|"B"|"C", mode?, enabled?, schedMode?, winStart?, winEnd?,
 *                        targetN?, targetP?, targetK?, targetPH?, preset? }   every field but col
 *                        is optional; an absent field is left unchanged, never treated as zero.
 *                        ESP1 still accepts preset (one of its own CROP_PRESETS names, resolved
 *                        server-side) via the LCD/SMS, but this page no longer offers it -- see
 *                        "Firmware save settings" below, which replaced it with the operator's own
 *                        saved N/P/K/pH+mode+schedule combinations instead of ESP1's fixed 4-name
 *                        table.
 *   SET_EXERCISE       { exerciseEnabled?, exerciseSeconds? }
 *   REBOOT             { target: "nano"|"esp2"|"esp1" }
 *   TEST_PULSE         { target, seconds: 1-15 }   Manual/Test only, dead-man timed on ESP2
 *   DIAG_SWEEP         { seconds? }
 *   SET_CLOCK          { clkY, clkMo, clkD, clkH, clkMi }   all required together; sets ESP1's RTC
 *   SET_THRESH         { thStart, thStop, thGap }   all required together; same clamps as the LCD
 *   RESTORE_DEFAULTS   {}   idle-only; runs the same reset function the LCD's Restore Defaults does
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
let liveRef = null;
let commandsRef = null;
let zonesRef = null;
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

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>'"]/g, char => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[char]));
}

function setText(id, text) {
  const element = document.getElementById(id);
  if (element) element.textContent = text;
}

function hasValue(value) {
  return value !== undefined && value !== null && value !== "" && !Number.isNaN(Number(value));
}

function numberText(value, digits = 1, unit = "") {
  if (!hasValue(value)) return "Unavailable";
  return `${Number(value).toFixed(digits)}${unit ? ` ${unit}` : ""}`;
}

function rawText(value, fallback = "Unavailable") {
  return value === undefined || value === null || value === "" ? fallback : String(value);
}

function booleanText(value, yes = "OK", no = "Not OK") {
  if (value === undefined || value === null) return "Unavailable";
  return value ? yes : no;
}

function formatAge(milliseconds) {
  const age = Number(milliseconds);
  if (!Number.isFinite(age) || age < 0 || age === 0xFFFFFFFF) return "Unavailable";
  if (age < 1000) return "under 1 second";
  if (age < 60000) return `${Math.floor(age / 1000)} seconds`;
  if (age < 3600000) return `${Math.floor(age / 60000)} minutes`;
  return `${Math.floor(age / 3600000)} hours`;
}

function snapshotAgeText() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  if (!stamp) return "Waiting for first snapshot";
  const age = Math.max(0, Date.now() - stamp);
  return `Snapshot received ${formatAge(age)} ago`;
}

function deviceIsFresh() {
  const stamp = Number(liveData.meta?.updatedAt || 0);
  const refreshMs = Number(liveData.meta?.refreshMs || 60000);
  return Boolean(stamp) && Date.now() - stamp < Math.max(refreshMs * 2 + 15000, 90000);
}

function currentUserIsSignedIn() {
  return Boolean(firebaseReady && auth?.currentUser);
}

/* User approval -----------------------------------------------------------------------------
 * A signed-in account is not automatically allowed to touch hardware -- see /users/{uid} in
 * firebase-rules.json. This constant and these two helpers are for UI purposes only (showing/
 * hiding the pending banner and the User Management tab, greying out buttons); the REAL
 * enforcement is the rules themselves, which check the identical literal UID server-side. */
// Two fixed operator identities -- primary (espoperator32@gmail.com) and a hidden backup/creator
// account (johnmichaelsugian123@gmail.com), both granted identically by firebase-rules.json's own
// matching literals. Client-side use is UI-only (which tab/actions to show); the real enforcement
// is server-side, keyed on these same two UIDs.
const OPERATOR_UIDS = ["KaiqrwlOHeVdebbokPxZfcFLZiu2", "C8V39t4EDgUgjOt9GA6OeMukMsw2"];
const BACKUP_OPERATOR_UID = "C8V39t4EDgUgjOt9GA6OeMukMsw2";
// Same literal UID firebase-rules.json's ESP1-only branches use -- referenced here only for the
// User Management defense-in-depth exclusion (renderUserManagement()), never for any gating logic.
const ESP1_DEVICE_UID = "6hNg56ldAaSmpnodG1Xla0vn61O2";
let myAccountStatus = undefined; // undefined = not loaded yet, null = no record, else the status string
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
function hasTempAccess() {
  return Date.now() < myTempAccessUntil;
}
// Sub-operator (2026-09-10): a SECOND, separate elevation from the 1h temp grant above -- persistent
// (no expiry clock) until an operator explicitly withholds it again, same on/off spirit as e.g. the
// preventive-exercise toggle. Stored as /users/{uid}/subOperator (boolean), settable only by either
// operator UID (see firebase-rules.json) -- both Main and Creator/backup accounts can grant/withhold
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

// Access-control revision (2026-09-09): renderZonesUI()'s editable-vs-read-only variant depends on
// isApprovedUser(), which can change mid-session (operator approves/restricts/blocks an account
// while its tab stays open -- see Part 6 of the access-control spec). Re-rendering on every single
// syncControlAvailability() tick (called every 15s and on every live-data push) would be wasteful
// and would fight the capture/restore form-state machinery for no reason, so this only re-renders
// on an actual flip, tracked here. Called from attachUserStatusListener()'s status callback (the
// only thing that can change isApprovedUser() while isOperator() stays fixed for the session).
let lastZoneEditCapability = undefined;
function syncZoneEditCapability() {
  const canEdit = isApprovedUser();
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

function showToast(message, tone = "") {
  const text = String(message || "").trim();
  if (!text) return;
  const stack = document.getElementById("toastStack");
  if (!stack) return;
  const id = ++toastSeq;
  const el = document.createElement("div");
  el.className = `toast${tone ? ` toast-${tone}` : ""}`;
  el.textContent = text;
  el.title = "Click to dismiss";
  el.addEventListener("click", () => dismissToast(id));
  stack.appendChild(el);
  activeToasts.set(id, { el, timer: null });
  // Force layout before adding the "in" class, or the transition never plays -- the browser would
  // otherwise coalesce "append with opacity:0" and "immediately set opacity:1" into one paint with no
  // visible transition at all.
  requestAnimationFrame(() => el.classList.add("toast-in"));
  rescheduleToasts();
}

function setCommandStatus(text, tone = "") {
  const element = document.getElementById("commandStatus");
  if (element) {
    element.textContent = text;
    element.className = `command-status ${tone}`.trim();
  }
  showToast(text, tone);
}

function syncControlAvailability() {
  const signedIn = currentUserIsSignedIn();
  const approved = isApprovedUser();
  // A signed-in-but-not-yet-approved account must see every actuating control as unavailable, same
  // as a signed-out one -- approval is a separate axis from freshness/cooldown, not a replacement
  // for it, so it is ANDed into every gate below rather than only checked at queueCommand() time.
  const notApprovedTitle = "Your account is awaiting operator approval before it can use this control.";
  const normalAllowed = signedIn && approved && deviceIsFresh();
  // Every actuating control needs a fresh snapshot: ESP1 will refuse anything that arrives while it
  // is not idle, and without live data the page cannot tell "idle" from "device offline".
  // The pulse and the ESP2 sweep both reach the rig, so they belong with the other actuating
  // controls rather than looking alive and then failing inside queueCommand().
  ["transferPumpBtn", "boosterPumpBtn", "mixerBtn", "pulseBtn", "mtSweepBtn", "sweepBtn", "exSaveBtn"].forEach(id => {
    const button = document.getElementById(id);
    if (!button) return;
    button.disabled = !normalAllowed;
    button.title = normalAllowed ? "" : (signedIn && !approved) ? notApprovedTitle : "Sign in and wait for a fresh ESP1 snapshot before starting a normal test.";
  });
  // Hold-to-run buttons: same normalAllowed gate as every other actuating Manual/Test control above.
  // If the rig drops out of "fresh" or the account loses approval WHILE a button is physically held,
  // the press must not keep silently running -- mtHoldStop() releases it immediately, same as losing
  // the Manual/Test hold itself does in renderManualHold().
  document.querySelectorAll(".hold-btn").forEach(button => {
    button.disabled = !normalAllowed;
    button.title = normalAllowed ? "" : (signedIn && !approved) ? notApprovedTitle : "Sign in and wait for a fresh ESP1 snapshot before using a hold-to-run test.";
  });
  if (!normalAllowed) mtHoldStop(true);

  const forceButton = document.querySelector("#forceRunForm button[type=submit]");
  if (forceButton) {
    // C-H2 workaround (audit): the CURRENTLY-FLASHED ESP1 firmware's FORCE_RUN idle-gate does not
    // check actuationsDisabled or an in-progress run at ARM time -- only at fire time, inside
    // forceTick()/sendForceWorkOrder(). That means a request submitted right now could be accepted
    // and armed (a live countdown shown) and then silently abort seconds later. Pre-checking the
    // same conditions here, from data ESP1 already publishes, stops the doomed request from ever
    // being sent instead of letting it fail invisibly downstream. This cannot see every condition
    // the firmware checks (the LCD menu state and another session's Manual/Test hold aren't
    // published anywhere) so it narrows the window without fully closing it -- only a reflash does.
    const fd = liveData.diagnostics || {};
    const busyState = liveData.system?.state && liveData.system.state !== "IDLE_STATE";
    const busy = Boolean(busyState) || Boolean(fd.actuationsDisabled) || Boolean(fd.fault?.held) ||
                 Boolean(fd.system?.workOrderActive) || Boolean(fd.system?.pendingRun);
    forceButton.disabled = !normalAllowed || busy;
    forceButton.title = !normalAllowed
      ? ((signedIn && !approved) ? notApprovedTitle : "Sign in and wait for a fresh ESP1 snapshot before forcing a run.")
      : busy
        ? "Blocked right now: the controller is not idle, a fault is held, or actuations are disabled -- starting would only be accepted and then silently fail. Resolve that first."
        : "";
  }

  // Emergency stop is intentionally independent of live-data freshness and the normal cooldown --
  // but NOT independent of approval, which is a distinct safety gate, not a staleness workaround.
  const emergency = document.getElementById("emergencyStop");
  if (emergency) {
    emergency.disabled = !signedIn || !approved;
    emergency.title = !signedIn ? "Sign in before sending an emergency-stop request." : !approved ? notApprovedTitle : "";
  }

  // Recovery + lockout + reboot: signed-in AND approved, deliberately NOT freshness-gated. A stale
  // snapshot is often exactly why you are reaching for these.
  ["disableActBtn2", "enableActBtn2", "rebootNanoBtn2", "rebootEsp2Btn2", "rebootEsp1Btn2"].forEach(id => {
    const b = document.getElementById(id);
    if (!b) return;
    b.disabled = !signedIn || !approved;
    b.title = !signedIn ? "Sign in to use the system controls." : !approved ? notApprovedTitle : "";
  });
  // Only one of disable/re-enable is meaningful at a time; show the one that applies.
  const locked = Boolean(liveData.diagnostics?.actuationsDisabled);
  const d2 = document.getElementById("disableActBtn2");
  const e2 = document.getElementById("enableActBtn2");
  if (d2) d2.hidden = locked;
  if (e2) e2.hidden = !locked;

  // Restore Defaults: signed-in + approved, like the reboot buttons, PLUS idle-gated -- ESP1
  // refuses this one outright unless sysState is IDLE_STATE, matching the LCD's own gate.
  const restoreBtn = document.getElementById("sysRestoreDefaultsBtn");
  if (restoreBtn) {
    const notIdle = liveData.system?.state && liveData.system.state !== "IDLE_STATE";
    restoreBtn.disabled = !signedIn || !approved || Boolean(notIdle);
    restoreBtn.title = !signedIn ? "Sign in to use the system controls."
                      : !approved ? notApprovedTitle
                      : notIdle ? "ESP1 must be idle to restore defaults, same as the LCD menu."
                      : "";
  }
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
        detachPresence();              // mark this session offline immediately -- don't wait for onDisconnect
        // liveData is about to be wiped below -- reset this FIRST so the updateDashboard() call just
        // after doesn't see (wasFaultActive=true, empty liveData) and announce a fabricated "The hold
        // cleared" from data loss, as if the fault/lockout had actually resolved.
        wasFaultActive = false;
        liveData = {};
        commandData = [];
        updateDashboard();
        renderCommandHistory();
        renderAccountStatus(null);
        renderBlockedScreen();
        if (loginScreen) loginScreen.hidden = false;
        showAuthForm("login");
        if (logoutButton) logoutButton.hidden = true;
        if (currentUserEmailEl) { currentUserEmailEl.hidden = true; currentUserEmailEl.textContent = ""; }
        setConnection(false, "Sign in required");
        syncControlAvailability();
        refreshOperatorUI();
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
  setConnection(true, "Connecting to Firebase…");
  liveRef = db.ref("irrigation/live");
  commandsRef = db.ref("irrigation/commands").limitToLast(25);

  liveRef.on("value", snapshot => {
    liveData = snapshot.val() || {};
    updateDashboard();
    const fresh = deviceIsFresh();
    setConnection(fresh, fresh ? "Live system connected" : "Waiting for a current ESP1 snapshot");
    syncControlAvailability();
  }, error => setConnection(false, `Live-data error: ${error.code || "unknown"}`));

  commandsRef.on("value", snapshot => {
    const raw = snapshot.val() || {};
    commandData = Object.entries(raw).map(([id, command]) => ({ id, ...command }));
    renderCommandHistory();
    renderPulseResult();          // the pulse verdict rides on its command status detail
    renderColumnCommandResults(); // each zone's own SET_COLUMN outcome, inline under its own button
  }, error => setCommandStatus(`Command status unavailable: ${error.code || "unknown"}`, "error"));

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
      const crop = cropDatabase[zone.crop] ? zone.crop : def.defaultCrop;
      const stage = cropDatabase[crop][zone.stage] ? zone.stage : def.defaultStage;
      // savedSettings: this column's own "Firmware save settings" library (see
      // saveColumnSettings()/renderZonesUI()) -- lives under the same dashboard-only config/zones
      // node as crop/stage, so no new Firebase rule or listener was needed; this one already
      // watches the whole subtree.
      return { id: def.id, name: zone.name || def.name, defaultCrop: crop, defaultStage: stage, savedSettings: zone.savedSettings || {} };
    });
    renderZonesUI();
    updateDashboard();
  }, error => console.warn("Could not read dashboard zone profiles", error));
}

function detachDatabaseListeners() {
  if (liveRef) liveRef.off();
  if (commandsRef) commandsRef.off();
  if (zonesRef) zonesRef.off();
  liveRef = null;
  commandsRef = null;
  zonesRef = null;
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
        .finally(() => auth.signOut());
      return;
    }
    const wasApproved = myAccountStatus === "approved";
    myAccountStatus = record ? record.status : null;
    myTempAccessUntil = Number(record?.tempAccessUntil) || 0;
    mySubOperator = Boolean(record?.subOperator);
    renderAccountStatus(record);
    renderBlockedScreen();
    syncControlAvailability();
    syncZoneEditCapability();
    refreshOperatorUI();   // an operator's temp-access/sub-operator grant/revoke must show/hide my tabs live
    // Block/Restrict must release an in-progress Manual/Test hold immediately, the same way Kick's
    // forced sign-out already does via onAuthStateChanged -- otherwise the rig stays out of
    // automatic for up to the full 60s lease after the operator believes access was cut off at once.
    if (wasApproved && myAccountStatus !== "approved") releaseManualHold();
  }, error => {
    console.warn("Could not read account status", error);
    myAccountStatus = null;
    myTempAccessUntil = 0;
    mySubOperator = false;
    renderAccountStatus(null);
    renderBlockedScreen();
    syncControlAvailability();
    syncZoneEditCapability();
    refreshOperatorUI();
  });
}
function detachUserStatusListener() {
  if (myAccountRef) myAccountRef.off();
  myAccountRef = null;
  myAccountStatus = undefined;
}

const ACCOUNT_STATUS_TEXT = {
  pending: {
    title: "Waiting for operator approval",
    detail: "Your account has been created, but the operator has not approved your access yet. You can see the read-only dashboard while you wait, but every control that would affect real hardware stays blocked until you're approved."
  },
  rejected: {
    title: "Access not approved",
    detail: "The operator reviewed this account and did not approve it for hardware access. You can still see the read-only dashboard."
  },
  restricted: {
    title: "Restricted access",
    detail: "This account can view the irrigation system but cannot operate physical equipment. The operator can unrestrict it to restore full access at any time."
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
  if (!currentUserIsSignedIn() || isOperator() || myAccountStatus === "approved" || myAccountStatus === "disabled") { banner.hidden = true; return; }
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
}
document.getElementById("blockedSignOutBtn")?.addEventListener("click", () => {
  auth?.signOut().catch(error => console.error(error));
});

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
  if (!privileged && document.querySelector('.tab[data-view="system"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
  const manualTestTab = document.getElementById("manualtestTab");
  if (manualTestTab) manualTestTab.hidden = !privileged;
  if (!privileged && document.querySelector('.tab[data-view="manualtest"]')?.classList.contains("active")) {
    document.querySelector('.tab[data-view="dashboard"]')?.click();
  }
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
 * still live. Upgrade to the counter form if that ever actually causes confusion. */
let presenceRef = null;
let presenceData = {};
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

/* Per-account recent activity (2026-09-10) -- operator-viewing-only (see firebase-rules.json's
 * activity/.read: only the two operator UIDs can read ANY of it, not even a user their own trail).
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

let myPresenceRef = null;
let myConnectedRef = null;
function attachPresence(uid) {
  if (!db) return;
  detachPresence();
  myPresenceRef = db.ref(`presence/${uid}`);
  myConnectedRef = db.ref(".info/connected");
  myConnectedRef.on("value", snap => {
    if (snap.val() !== true) return;
    // Register the disconnect write FIRST -- if the connection drops between here and the .set()
    // just below landing, the server-side onDisconnect still leaves an accurate offline+timestamp
    // record instead of a stuck "online" one.
    myPresenceRef.onDisconnect().set({ online: false, lastChanged: firebase.database.ServerValue.TIMESTAMP })
      .then(() => myPresenceRef.set({ online: true, lastChanged: firebase.database.ServerValue.TIMESTAMP }))
      .catch(error => console.warn("Could not set presence", error));
  });
}
function detachPresence() {
  if (myConnectedRef) myConnectedRef.off();
  if (myPresenceRef) {
    myPresenceRef.onDisconnect().cancel();
    myPresenceRef.set({ online: false, lastChanged: firebase.database.ServerValue.TIMESTAMP }).catch(() => {});
  }
  myConnectedRef = null;
  myPresenceRef = null;
}

const USER_STATUS_ORDER = { pending: 0, approved: 1, restricted: 2, disabled: 3, rejected: 4 };
// UI labels only -- the backend values are what Firebase rules actually enforce everywhere; this
// just controls what word appears on screen, per the explicit ask not to show "disabled" to a
// normal user when "Blocked" (or "Restricted") is what's actually meant.
const STATUS_DISPLAY_LABEL = { disabled: "BLOCKED", restricted: "RESTRICTED" };
function renderUserManagement() {
  const container = document.getElementById("usersContainer");
  if (!container) return;
  const users = lastKnownUsers;
  // The backup/creator operator is deliberately invisible here -- filtered out of the list itself,
  // not merely stripped of actions like the primary operator's own row is (see isAnyOperator
  // below). Nothing in this app ever needs to manage the backup account as if it were a normal user.
  const rows = Object.entries(users).filter(([uid]) => uid !== BACKUP_OPERATOR_UID);
  if (!rows.length) { container.innerHTML = `<p class="muted">No registered accounts yet.</p>`; return; }
  rows.sort((a, b) => (USER_STATUS_ORDER[a[1]?.status] ?? 9) - (USER_STATUS_ORDER[b[1]?.status] ?? 9));
  container.innerHTML = rows.map(([uid, u]) => {
    const status = u?.status || "unknown";
    const created = u?.createdAt ? new Date(u.createdAt).toLocaleString() : "Unknown";
    // Either operator's own row (if a /users record happens to exist at all) gets zero actions --
    // real enforcement is the rules' own guard refusing either operator write to either operator's
    // record; this is the UI half, so there is nothing to accidentally click in the first place.
    // Same exclusion for ESP1's device UID as defense-in-depth, though nothing today ever creates a
    // /users record for it. Kept separate from "is this row the person actually viewing the screen"
    // below -- with two operators, a row is not automatically "you" just because it's *an* operator.
    const isAnyOperator = OPERATOR_UIDS.includes(uid) || uid === ESP1_DEVICE_UID;
    const isViewerSelf = uid === auth.currentUser?.uid;
    // Temporary Manual/Test + System access (see canAccessPrivilegedTabs() in the capability-check
    // section above) -- only ever meaningful for an approved, non-operator account; an operator
    // already has full access, and a pending/restricted/blocked account can't be elevated without
    // first being approved.
    const tempActive = status === "approved" && !isAnyOperator && Number(u?.tempAccessUntil) > Date.now();
    // Sub-operator (2026-09-10): a SECOND, separate elevation to Manual/Test + System -- persistent
    // (no expiry) rather than the 1h temp grant above, toggled on/off like a switch until an operator
    // changes it again. Independent of tempActive -- an account can hold either, both, or neither;
    // canAccessPrivilegedTabs() ORs them together client-side.
    const subOpActive = status === "approved" && !isAnyOperator && Boolean(u?.subOperator);
    // Online/offline (see attachPresence()/attachPresenceWatch() above) -- also decides whether
    // Kick is even offered: kicking an already-offline account just wrote a fresh kickToken that
    // would immediately re-fire the moment they reconnect, which is exactly the "kicked N times"
    // symptom that clicking Kick repeatedly on an offline user produced before this gate existed.
    const pres = presenceData[uid];
    const isOnline = Boolean(pres?.online);
    const presenceLabel = isOnline ? "Online" : pres?.lastChanged ? `Offline (${formatAge(Date.now() - pres.lastChanged)} ago)` : "Offline (never seen)";
    // Recent activity (2026-09-10): last 5 commands this account actually queued (see
    // activitySummary()/queueCommand() above), newest first. Operator-viewing-only, same as presence
    // -- enforced server-side by activity/.read, not just by this tab being hidden from everyone else.
    const recent = recentActivityFor(uid);
    const actions = [];
    if (!isAnyOperator) {
      if (status === "pending")    actions.push(["approve", "Approve"], ["reject", "Reject"], ["restrict", "Restrict"], ["delete", "Delete"]);
      if (status === "approved") {
        actions.push(...(isOnline ? [["kick", "Kick"]] : []), ["restrict", "Restrict"], ["block", "Block"], ["delete", "Delete"]);
        // Suggestion (2026-09-11): these two grants look interchangeable at a glance (both say
        // "Manual/Test + System") -- unlike every other control on this page, neither had an info
        // icon explaining how they actually differ (one expires on its own, one doesn't). Added here,
        // not on the plainer actions (Kick/Restrict/Block/Delete), which don't have that ambiguity.
        actions.push(tempActive
          ? ["revoketemp", "Revoke temp access", "Temp access (1h)", "Ends this account's temporary Manual/Test + System access right now, instead of waiting out its own 1-hour clock."]
          : ["granttemp", "Grant 1h Manual/Test + System", "Temp access (1h)", "Gives this account full Manual/Test + System access for exactly 1 hour, then it expires on its own -- nothing to remember to take away later. Use this for a one-off task."]);
        actions.push(subOpActive
          ? ["withholdsubop", "Withhold Sub-operator", "Sub-operator", "Ends this account's Sub-operator access right now. Unlike the 1h grant above, Sub-operator has no expiry of its own -- withholding it here is the only way it ever ends."]
          : ["grantsubop", "Grant Sub-operator", "Sub-operator", "Gives this account the SAME full Manual/Test + System access as the 1h grant above, but with no expiry -- it stays in effect until an operator withholds it again. Use this for someone who needs ongoing access, not just a one-off task."]);
      }
      if (status === "restricted") actions.push(["unrestrict", "Unrestrict"], ...(isOnline ? [["kick", "Kick"]] : []), ["block", "Block"], ["delete", "Delete"]);
      if (status === "rejected")   actions.push(["approve", "Approve"], ["restrict", "Restrict"], ["block", "Block"], ["delete", "Delete"]);
      if (status === "disabled")   actions.push(["unblock", "Unblock"], ["delete", "Delete"]);
    }
    const tone = status === "approved" ? "active" : (status === "pending" || status === "restricted") ? "off" : "danger";
    const tones = { delete: " danger", block: " warn", revoketemp: " warn", withholdsubop: " warn" };
    const buttons = actions.map(([action, label, infoTitle, infoText]) => {
      const btn = `<button type="button" class="user-action${tones[action] || ""}" data-user-action="${action}" data-uid="${escapeHtml(uid)}">${escapeHtml(label)}</button>`;
      // Same "icon as a sibling, never nested inside the actuating button" pattern used everywhere
      // else on this page (see renderZonesUI()'s own local info() helper) -- kept together via
      // .btn-with-info rather than sharing one info() closure across two separately-scoped render
      // functions.
      return infoTitle
        ? `<span class="btn-with-info">${btn}<button type="button" class="info-icon" aria-haspopup="dialog" aria-expanded="false" aria-label="About ${infoTitle}" data-info-title="${infoTitle}" data-info-text="${infoText}">i</button></span>`
        : btn;
    }).join("");
    return `<article class="user-row" data-email="${escapeHtml(u?.email || "")}">
      <div class="user-row-info">
        <strong>${escapeHtml(u?.name || "(no name)")}${isViewerSelf ? " (you, the operator)" : ""}</strong>
        <span class="muted">${escapeHtml(u?.email || "(no email)")}</span>
        <span class="muted">Registered: ${escapeHtml(created)}</span>
        <span class="muted">${escapeHtml(presenceLabel)}</span>
        ${tempActive ? `<span class="muted">Temp Manual/Test + System access until ${escapeHtml(new Date(u.tempAccessUntil).toLocaleTimeString())}</span>` : ""}
        ${subOpActive ? `<span class="muted">Sub-operator — Manual/Test + System access until withheld</span>` : ""}
        ${recent.length
          ? `<details class="user-activity" data-uid="${escapeHtml(uid)}"${expandedActivityUids.has(uid) ? " open" : ""}>
              <summary>Recent: ${escapeHtml(rawText(recent[0]?.what, "?"))} — ${escapeHtml(formatAge(Date.now() - Number(recent[0]?.at || 0)))} ago${recent.length > 1 ? ` (${recent.length} shown)` : ""}</summary>
              <ul class="user-activity-list">${recent.map(a =>
                `<li>${escapeHtml(rawText(a?.what, "?"))} — ${escapeHtml(formatAge(Date.now() - Number(a?.at || 0)))} ago</li>`
              ).join("")}</ul>
            </details>`
          : `<span class="muted">No recent activity.</span>`}
      </div>
      <span class="device-status ${tone}">${escapeHtml(STATUS_DISPLAY_LABEL[status] || status.toUpperCase())}</span>
      <div class="user-row-actions">${buttons}</div>
    </article>`;
  }).join("");
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
  if (!response.ok) throw new Error(result.error || `Request failed (${response.status})`);
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
  const button = event.target.closest("[data-user-action]");
  if (!button) return;
  const uid = button.dataset.uid;
  const action = button.dataset.userAction;
  if (!uid || !action || OPERATOR_UIDS.includes(uid) || uid === ESP1_DEVICE_UID) return; // matches renderUserManagement()'s own guard
  const row = button.closest(".user-row");
  const name = row?.querySelector(".user-row-info strong")?.textContent || "this account";
  const email = row?.dataset.email || "";
  const run = write => { button.disabled = true; write().catch(error => showToast(`Could not update this account: ${error.message}`, "error")).finally(() => { button.disabled = false; }); };

  if (action === "delete") {
    // Deliberately the strongest confirmation of the three (a plain confirm() for the others) --
    // this is the one truly irreversible action. Routed through delete-user-api/ (a separate
    // Vercel project), not a direct database write: no client-side API can delete another uid's
    // Firebase Auth identity, only the Admin SDK can, which only runs there. That endpoint deletes
    // both the Auth account and /users/{uid} in one trusted call.
    const typed = prompt(`Delete ${name}? Their account (login and all) will be permanently deleted -- they would need to sign up again as a brand-new account. This cannot be undone.\n\nType the account's email to confirm:\n${email}`);
    if (typed === null || typed.trim() !== email) return;
    run(() => deleteUserAccount(uid));
    return;
  }
  if (action === "kick") {
    // Defense-in-depth re-check: renderUserManagement() already omits this button once offline,
    // but re-verify against the live presence data at click time too, in case this row was mid-
    // re-render (a stale "Kick" button briefly clickable is otherwise possible in that gap).
    if (!presenceData[uid]?.online) { showToast(`${name} is not currently online -- nothing to kick.`, "error"); return; }
    if (!confirm(`Force ${name} to sign in again?`)) return;
    run(() => db.ref(`users/${uid}`).update({ kickToken: firebase.database.ServerValue.TIMESTAMP }));
    return;
  }
  if (action === "granttemp") {
    // Time-bounded elevation, not a role change -- status stays "approved" throughout. Enforced
    // server-side too: firebase-rules.json's tempAccessUntil validator caps this at the same
    // TEMP_ACCESS_DURATION_MS from the moment the write actually lands, so a slow request can't
    // grant longer than intended. grantTempAccess() anchors to Firebase's own server clock rather
    // than this browser's Date.now() -- see its own comment (bug report, 2026-09-10) for why.
    if (!confirm(`Grant ${name} temporary access to Manual/Test and System for 1 hour?`)) return;
    run(() => grantTempAccess(uid));
    return;
  }
  if (action === "revoketemp") {
    run(() => db.ref(`users/${uid}`).update({ tempAccessUntil: null }));
    return;
  }
  if (action === "grantsubop") {
    // Persistent elevation, not a role change -- status stays "approved" throughout, same as temp
    // access above. No expiry: stays in effect until an operator withholds it again.
    if (!confirm(`Grant ${name} Sub-operator access to Manual/Test and System? This stays in effect until you withhold it again.`)) return;
    run(() => db.ref(`users/${uid}`).update({ subOperator: true }));
    return;
  }
  if (action === "withholdsubop") {
    run(() => db.ref(`users/${uid}`).update({ subOperator: null }));
    return;
  }
  const statusMap = { approve: "approved", reject: "rejected", block: "disabled", unblock: "approved", restrict: "restricted", unrestrict: "approved" };
  const newStatus = statusMap[action];
  if (!newStatus) return;
  if (action === "block" && !confirm(`Block ${name}? They will immediately lose access.`)) return;
  if (action === "restrict" && !confirm(`Remove ${name}'s hardware-control access while keeping dashboard access?`)) return;
  // Deep-scan finding (2026-09-10): reject/block/restrict previously left any outstanding
  // tempAccessUntil grant untouched -- re-approving the account before that grant's own clock ran
  // out (up to an hour later) would silently hand privileged access straight back with no fresh
  // grant from an operator. Clear it alongside any status change that revokes approval. Sub-operator
  // (2026-09-10) is persistent rather than time-limited, so this matters even more for it -- without
  // this, re-approving a once-blocked sub-operator would silently hand the elevation straight back
  // with no fresh grant, indefinitely, not just within some grant's remaining hour.
  const updates = { status: newStatus };
  if (newStatus !== "approved") { updates.tempAccessUntil = null; updates.subOperator = null; }
  run(() => db.ref(`users/${uid}`).update(updates));
});

function writeZoneProfile(zone) {
  if (!currentUserIsSignedIn()) return;
  // Audit fix (2026-09-08): every other write path (queueCommand, writeManualHold) gates on
  // isApprovedUser(), not just sign-in. The Firebase rule already rejects this write for a
  // non-approved account either way, so there was no security gap -- but a pending/rejected account
  // changing a crop/stage dropdown got a raw PERMISSION_DENIED error instead of the same friendly
  // "awaiting approval" message shown everywhere else.
  if (!isApprovedUser()) {
    setCommandStatus("Your account is awaiting operator approval before it can send commands to the rig.", "error");
    return;
  }
  db.ref(`irrigation/config/zones/${zone.id}`).set({
    name: zone.name,
    crop: zone.defaultCrop,
    stage: zone.defaultStage,
    updatedAt: firebase.database.ServerValue.TIMESTAMP
  }).catch(error => setCommandStatus(`Could not save dashboard-only zone profile: ${error.message}`, "error"));
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
  // Access-control revision (2026-09-09): a handful of command types (Manual/Test's pump exercise/
  // timed pulse/flow sweep, System's Set Clock/Thresholds/Restore Defaults) are meant for the two
  // privileged operator accounts -- or a normal approved account an operator has temporarily
  // elevated (canAccessPrivilegedTabs() = isOperator() || hasTempAccess()) -- never a plain approved
  // account on its own. Centralized in this one choke point rather than duplicated per call site,
  // matching every other gate in this function. As of the 2026-09-09 rules tightening, this is no
  // longer just a client-side convenience: firebase-rules.json's commands/$commandId write rule
  // independently enforces the identical operator-or-temp-access check for these six command types.
  if (options.operatorOnly && !canAccessPrivilegedTabs()) {
    setCommandStatus("This control is limited to the main/creator operator account.", "error");
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
  setCommandStatus(queuedStatusText(type, payload, emergency), "pending");
  return db.ref("irrigation/commands").push(command)
    .then(reference => {
      if (!emergency) lastCommandAt = Date.now();
      // Per-account activity trail (2026-09-10), operator-viewing-only -- see renderUserManagement().
      // Best-effort: a failure here must never affect the real command above, which already
      // succeeded by this point, so this is deliberately fire-and-forget with its own silent catch.
      // Deep-scan finding (2026-09-10): auth.currentUser can legitimately be null here if the user
      // signed out in the brief window between the push above starting and this callback running --
      // an unguarded .uid access would throw SYNCHRONOUSLY inside this .then(), which the .catch()
      // below would then report as "Could not queue command", falsely, for a command that had
      // already succeeded. Skip the log entirely rather than risk that.
      const myUid = auth.currentUser?.uid;
      if (myUid) {
        db.ref(`activity/${myUid}`).push({
          what: activitySummary(type, payload),
          at: firebase.database.ServerValue.TIMESTAMP
        }).catch(error => console.warn("Could not log activity", error));
      }
      return reference.key;
    })
    .catch(error => {
      if (!emergency) lastCommandAt = 0;
      setCommandStatus(`Could not queue command: ${error.message}`, "error");
      return false;
    });
}

// Short, human-readable "what did they do" label for the activity trail above -- deliberately NOT
// the full command detail (that lives in irrigation/commands itself, and needs ESP1's own reply to
// mean anything); just enough context to recognise the action at a glance in User Management.
function activitySummary(type, payload) {
  const p = payload || {};
  switch (type) {
    case "FORCE_RUN":     return `FORCE_RUN (${rawText(p.columns, "?")}, ${numberText(p.liters, 1, "L")})`;
    case "SET_COLUMN":    return `SET_COLUMN (col ${rawText(p.col, "?")})`;
    case "TEST_PULSE":    return `TEST_PULSE (${rawText(p.target, "?")}, ${rawText(p.seconds, "?")}s)`;
    case "RUN_PUMP_TEST": return `RUN_PUMP_TEST (${rawText(p.pump, "?")})`;
    case "RECOVER":       return `RECOVER (${rawText(p.action, "?")})`;
    case "REBOOT":        return `REBOOT (${rawText(p.target, "?")})`;
    case "SET_EXERCISE":  return `SET_EXERCISE (${p.exerciseEnabled ? "on" : "off"})`;
    case "DIAG_SWEEP":    return "DIAG_SWEEP";
    default:              return type;
  }
}

// The status line shown the instant a command is queued -- before ESP1 has even seen it, let alone
// acted on it. Every branch must say "queued", never claim an outcome. Previously this only branched
// on the {emergency:true} flag, so RECOVER and ESTOP_RECOVER -- which also need that flag, to bypass
// the freshness/cooldown gate during a fault -- displayed the literal text "Emergency stop queued"
// regardless of which recovery button (Hold/Release/Only irrigate/Resume normal/Return to normal) was
// actually pressed.
function queuedStatusText(type, payload, emergency) {
  if (type === "EMERGENCY_STOP") return "Emergency stop queued. This is not physical-stop confirmation; wait for ESP1 status.";
  if (type === "RECOVER") return `Recovery (${rawText(payload.action, "?")}) queued. This is not confirmation the rig has resumed; wait for ESP1 status.`;
  if (type === "ESTOP_RECOVER") return "Return to normal queued. This is not confirmation ESP1 has recovered; wait for ESP1 status.";
  if (emergency) return `${type.replaceAll("_", " ")} queued. This is not confirmation it has taken effect; wait for ESP1 status.`;
  return `${type.replaceAll("_", " ")} queued. Waiting for ESP1 validation.`;
}

// Audit fix (2026-09-08): renderZonesUI()'s full teardown runs on every irrigation/config/zones
// change -- including a DIFFERENT zone's own crop/stage write -- and previously discarded whatever
// the operator had typed into any zone's not-yet-sent "Firmware settings" form (targets, schedule
// window, preset) with no warning, since those inputs have no backing data model, only DOM state.
// Capture the current values before the teardown and restore them onto the freshly rebuilt inputs.
function captureZoneFormState() {
  const state = {};
  const val = id => document.getElementById(id)?.value;
  activeZones.forEach(zone => {
    const id = zone.id;
    state[id] = {
      cfgMode: val(`cfgMode${id}`), cfgEnabled: val(`cfgEnabled${id}`), cfgSched: val(`cfgSched${id}`),
      cfgWinStart: val(`cfgWinStart${id}`), cfgWinEnd: val(`cfgWinEnd${id}`),
      cfgN: val(`cfgN${id}`), cfgP: val(`cfgP${id}`), cfgK: val(`cfgK${id}`), cfgPH: val(`cfgPH${id}`),
      cfgSaveName: val(`cfgSaveName${id}`)
    };
  });
  return state;
}

function restoreZoneFormState(zoneId, saved) {
  if (!saved) return;
  const set = (id, v) => { const el = document.getElementById(id); if (el && v) el.value = v; };
  set(`cfgMode${zoneId}`, saved.cfgMode); set(`cfgEnabled${zoneId}`, saved.cfgEnabled); set(`cfgSched${zoneId}`, saved.cfgSched);
  set(`cfgWinStart${zoneId}`, saved.cfgWinStart); set(`cfgWinEnd${zoneId}`, saved.cfgWinEnd);
  set(`cfgN${zoneId}`, saved.cfgN); set(`cfgP${zoneId}`, saved.cfgP); set(`cfgK${zoneId}`, saved.cfgK); set(`cfgPH${zoneId}`, saved.cfgPH);
  set(`cfgSaveName${zoneId}`, saved.cfgSaveName);
}

function renderZonesUI() {
  const container = document.getElementById("dynamic-zones-container");
  if (!container) return;
  const savedFormState = captureZoneFormState();
  container.innerHTML = "";
  // Access-control revision (2026-09-09): a pending/restricted account (isApprovedUser() false)
  // previously got the exact same "Firmware settings" block as an approved one -- quick-action
  // buttons, dropdowns, N/P/K/pH inputs, preset selector, Send to ESP1, all fully rendered and
  // clickable -- relying entirely on queueCommand()'s isApprovedUser() check (and the Firebase
  // rules behind it) to reject the write after the fact. That is still the REAL security boundary
  // and is unchanged below; this only stops the UI from offering a control that is "simply expected
  // to fail" (per the access-control spec). The sensor-reading matrix-grid and the live "Current
  // configuration" summary are identical either way -- only the editing surface (crop/stage
  // selects, and the whole zone-config block) is swapped for plain read-only text.
  const canEdit = isApprovedUser();
  activeZones.forEach(zone => {
    const block = document.createElement("article");
    block.className = "zone-block";
    const info = (title, text) => `<button type="button" class="info-icon" aria-haspopup="dialog" aria-expanded="false" aria-label="About ${title}" data-info-title="${title}" data-info-text="${text}">i</button>`;

    const cropStageHtml = canEdit
      ? `<div class="zone-selectors">
          <label><span class="label-row">Crop${info("Crop selection", "The crop growing here — a planning note only. To change how the rig runs, fill targets from it below and press Send to ESP1.")}</span><select id="cropSelect${zone.id}"></select></label>
          <label><span class="label-row">Growth stage${info("Growth stage", "The crop's current stage. Like Crop, this only updates reference numbers here — it doesn't change the rig by itself.")}</span><select id="growthStage${zone.id}"></select></label>
        </div>`
      : `<div class="zone-selectors">
          <label><span class="label-row">Crop</span><strong>${escapeHtml(readableCropNames[zone.defaultCrop] || zone.defaultCrop || "--")}</strong></label>
          <label><span class="label-row">Growth stage</span><strong>${escapeHtml(zone.defaultStage ? zone.defaultStage[0].toUpperCase() + zone.defaultStage.slice(1) : "--")}</strong></label>
        </div>`;

    const firmwareConfigHtml = canEdit
      ? `<div class="zone-config">
        <h4>Firmware settings for column ${zone.id}${info("Firmware settings", "The real settings ESP1 uses for this column. Blank fields are left unchanged — only what you fill in is updated.")}</h4>
        <p class="field-note">Unlike the crop profile above, these are sent to ESP1 and change how it runs. Use "Fill targets from crop profile" to copy values in, then review and send.</p>
        <p class="field-note" id="cfgCurrent${zone.id}">Current configuration: Unavailable</p>
        <div class="force-row zone-quick-actions">
          <span class="btn-with-info"><button type="button" id="zoneAuto${zone.id}" class="secondary">Auto</button>${info("Auto", "Sets this column to Auto (irrigation + fertigation) immediately — same as the LCD's Column Mode screen.")}</span>
          <span class="btn-with-info"><button type="button" id="zoneIrrOnly${zone.id}" class="secondary">Irrigation only</button>${info("Irrigation only", "Sets this column to Irrigation only (water, no dosing) immediately — same as the LCD's Column Mode screen.")}</span>
          <span class="btn-with-info"><button type="button" id="zoneOff${zone.id}" class="secondary">Off</button>${info("Off", "Disables this column immediately — same as the LCD's Column Mode screen. Its stored mode is kept, so switching back later needs its own click.")}</span>
        </div>
        <p class="field-note">The three buttons above act immediately. Everything below is the detailed form — still needs "Send to ESP1".</p>
        <div class="force-row">
          <label><span class="label-row">Operation${info("Operation", "Auto runs irrigation and dosing on schedule. Irrigation only keeps the schedule but skips dosing — plain water.")}</span><select id="cfgMode${zone.id}">
            <option value="">(unchanged)</option>
            <option value="AUTO">Auto — irrigation + fertigation</option>
            <option value="IRRIGATION_ONLY">Irrigation only</option>
          </select></label>
          <label>Column enabled<select id="cfgEnabled${zone.id}">
            <option value="">(unchanged)</option><option value="1">Enabled</option><option value="0">Disabled</option>
          </select></label>
          <label><span class="label-row">Schedule${info("Schedule", "Automatic lets the rig decide timing. Manual runs only within the start/end time you set below.")}</span><select id="cfgSched${zone.id}">
            <option value="">(unchanged)</option><option value="0">Automatic window</option><option value="1">Manual window</option>
          </select></label>
        </div>
        <div id="cfgWinRow${zone.id}" class="force-row" hidden>
          <label>Window start<input id="cfgWinStart${zone.id}" type="time"></label>
          <label>Window end<input id="cfgWinEnd${zone.id}" type="time"></label>
        </div>
        <p id="cfgWinNote${zone.id}" class="field-note" hidden>Window start/end only apply — and are only sent — when Schedule is set to Manual window.</p>
        <div class="force-row">
          <label>Target N (ppm)<input id="cfgN${zone.id}" type="number" min="0" max="2000" step="1"></label>
          <label>Target P (ppm)<input id="cfgP${zone.id}" type="number" min="0" max="2000" step="1"></label>
          <label>Target K (ppm)<input id="cfgK${zone.id}" type="number" min="0" max="2000" step="1"></label>
          <label>Target pH<input id="cfgPH${zone.id}" type="number" min="3" max="9" step="0.1"></label>
        </div>
        <div class="force-row">
          <label><span class="label-row">Firmware save settings${info("Firmware save settings", "Your saved settings for this column, most recent first. Selecting one fills the boxes above — review, then press Send to ESP1.")}</span><select id="cfgSavedList${zone.id}">
            <option value="">(none -- select a recent save)</option>
          </select></label>
        </div>
        <div class="force-row">
          <label>Save name (optional)<input id="cfgSaveName${zone.id}" type="text" maxlength="40" placeholder="e.g. Vegetative high-N"></label>
          <span class="btn-with-info"><button type="button" id="cfgSaveSettingsBtn${zone.id}" class="secondary">Save current settings</button>${info("Save current settings", "Saves what's filled in above as a named preset for this column, for the list above. Doesn't send to ESP1. Unnamed saves get a timestamp.")}</span>
        </div>
        <div class="config-actions">
          <span class="btn-with-info"><button type="button" id="cfgFromCrop${zone.id}" class="secondary">Fill targets from crop profile</button>${info("Fill targets from crop profile", "Copies the selected crop and stage's reference N/P/K/pH into the boxes above to review before sending. Doesn't change the rig by itself.")}</span>
          <span class="btn-with-info"><button type="button" id="cfgSave${zone.id}">Send to ESP1</button>${info("Send to ESP1", "Sends the settings above to ESP1. Each value is range-checked before it's accepted; blank fields are left unchanged.")}</span>
        </div>
        <p id="cfgResult${zone.id}" class="control-result" aria-live="polite"></p>
      </div>`
      : `<div class="zone-config">
        <h4>Firmware settings for column ${zone.id}${info("Firmware settings", "The real settings ESP1 uses for this column. Editing requires an approved account.")}</h4>
        <p class="field-note" id="cfgCurrent${zone.id}">Current configuration: Unavailable</p>
        <p class="field-note">Read-only access — editing firmware settings requires operator approval.</p>
      </div>`;

    block.innerHTML = `
      <div class="zone-header">
        <div><p class="eyebrow">Physical zone ${zone.id}</p><h3>${escapeHtml(zone.name)}</h3></div>
        ${cropStageHtml}
      </div>
      <div class="card-grid matrix-grid">
        <article class="card matrix-card"><h3>Nitrogen${info("Nitrogen", "Nitrogen measured in this zone's soil, compared to the selected crop's target. Red means below target.")}</h3><p id="nitrogen${zone.id}">Unavailable</p><small id="targetN${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Phosphorus${info("Phosphorus", "Phosphorus measured in this zone's soil, compared to the selected crop's target.")}</h3><p id="phosphorus${zone.id}">Unavailable</p><small id="targetP${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Potassium${info("Potassium", "Potassium measured in this zone's soil, compared to the selected crop's target.")}</h3><p id="potassium${zone.id}">Unavailable</p><small id="targetK${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil pH${info("Soil pH", "How acidic or alkaline this zone's soil is, from the 7-in-1 probe.")}</h3><p id="soilPH${zone.id}">Unavailable</p><small id="targetPH${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil EC${info("Soil EC", "How concentrated this zone's soil nutrients are, from the probe.")}</h3><p id="soilEC${zone.id}">Unavailable</p><small id="targetEC${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil moisture${info("Soil moisture", "How damp this zone's soil is. The schedule compares it to a threshold to decide when to run.")}</h3><p id="soil${zone.id}">Unavailable</p><small id="targetMoisture${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>NPK probe moisture${info("NPK probe moisture", "A second moisture reading from the NPK probe, blended into the main figure when the two agree.")}</h3><p id="npkMoist${zone.id}">Unavailable</p><small>Blended into the figure at left when it agrees</small></article>
        <article class="card matrix-card"><h3>Soil temperature${info("Soil temperature", "Root-zone soil temperature, from the 7-in-1 probe.")}</h3><p id="soilTemp${zone.id}">Unavailable</p><small>Root zone, from the 7-in-1 probe</small></article>
      </div>
      ${firmwareConfigHtml}
      <p class="zone-note">Actuator/solenoid feedback: not reported by the current ESP1 Firebase snapshot.</p>`;
    container.appendChild(block);

    if (canEdit) {
      block.querySelector(`#cfgSave${zone.id}`)?.addEventListener("click", () => submitColumnConfig(zone.id));
      block.querySelector(`#cfgFromCrop${zone.id}`)?.addEventListener("click", () => fillTargetsFromCrop(zone));
      block.querySelector(`#zoneAuto${zone.id}`)?.addEventListener("click", () => quickSetColumnMode(zone.id, "AUTO"));
      block.querySelector(`#zoneIrrOnly${zone.id}`)?.addEventListener("click", () => quickSetColumnMode(zone.id, "IRRIGATION_ONLY"));
      block.querySelector(`#zoneOff${zone.id}`)?.addEventListener("click", () => quickSetColumnMode(zone.id, null));

      block.querySelector(`#cfgSaveSettingsBtn${zone.id}`)?.addEventListener("click", () => saveColumnSettings(zone.id));

      // Newest first, capped at 10 -- a convenience list, not a full archive; every entry ever
      // saved stays in Firebase regardless, this just doesn't grow the dropdown unbounded.
      const savedSelect = block.querySelector(`#cfgSavedList${zone.id}`);
      const savedEntries = Object.entries(zone.savedSettings || {})
        .sort((a, b) => (b[1]?.savedAt || 0) - (a[1]?.savedAt || 0))
        .slice(0, 10);
      savedEntries.forEach(([key, s]) => {
        const label = `${s?.name || "Untitled"} (N${rawText(s?.targetN, "-")}/P${rawText(s?.targetP, "-")}/K${rawText(s?.targetK, "-")}/pH${rawText(s?.targetPH, "-")})`;
        savedSelect?.add(new Option(label, key));
      });
      savedSelect?.addEventListener("change", () => {
        const chosen = zone.savedSettings?.[savedSelect.value];
        savedSelect.value = "";   // revert to the placeholder -- re-picking the same entry must still fire "change"
        if (!chosen) return;
        const setVal = (id, v) => { const el = document.getElementById(id); if (el) el.value = v ?? ""; };
        setVal(`cfgMode${zone.id}`, chosen.mode);
        setVal(`cfgEnabled${zone.id}`, chosen.enabled);
        setVal(`cfgSched${zone.id}`, chosen.schedMode);
        setVal(`cfgWinStart${zone.id}`, chosen.winStart);
        setVal(`cfgWinEnd${zone.id}`, chosen.winEnd);
        setVal(`cfgN${zone.id}`, chosen.targetN);
        setVal(`cfgP${zone.id}`, chosen.targetP);
        setVal(`cfgK${zone.id}`, chosen.targetK);
        setVal(`cfgPH${zone.id}`, chosen.targetPH);
        updateWindowVisibility(zone.id);
      });

      const cropSelect = block.querySelector(`#cropSelect${zone.id}`);
      const stageSelect = block.querySelector(`#growthStage${zone.id}`);
      Object.keys(cropDatabase).forEach(crop => {
        const option = new Option(readableCropNames[crop], crop, false, crop === zone.defaultCrop);
        cropSelect.add(option);
      });
      populateStageOptions(zone, stageSelect);
      cropSelect.addEventListener("change", () => {
        zone.defaultCrop = cropSelect.value;
        zone.defaultStage = Object.keys(cropDatabase[zone.defaultCrop])[0];
        populateStageOptions(zone, stageSelect);
        updateZoneTargets(zone);
        writeZoneProfile(zone);
      });
      stageSelect.addEventListener("change", () => {
        zone.defaultStage = stageSelect.value;
        updateZoneTargets(zone);
        writeZoneProfile(zone);
      });
      restoreZoneFormState(zone.id, savedFormState[zone.id]);

      // Window start/end only mean anything under Manual window -- hide them otherwise rather than
      // deleting whatever value they hold, and re-check on every change so switching back to Manual
      // later shows the value again instead of forcing it to be retyped.
      const schedSelect = block.querySelector(`#cfgSched${zone.id}`);
      schedSelect?.addEventListener("change", () => updateWindowVisibility(zone.id));
      updateWindowVisibility(zone.id);
    }
    updateZoneTargets(zone);
  });
}

function updateWindowVisibility(zoneId) {
  const schedSelect = document.getElementById(`cfgSched${zoneId}`);
  const row = document.getElementById(`cfgWinRow${zoneId}`);
  const note = document.getElementById(`cfgWinNote${zoneId}`);
  const manual = schedSelect?.value === "1";
  if (row) row.hidden = !manual;
  if (note) note.hidden = !manual;
}

function populateStageOptions(zone, stageSelect) {
  stageSelect.innerHTML = "";
  const stages = Object.keys(cropDatabase[zone.defaultCrop]);
  if (!stages.includes(zone.defaultStage)) zone.defaultStage = stages[0];
  stages.forEach(stage => stageSelect.add(new Option(stage[0].toUpperCase() + stage.slice(1), stage, false, stage === zone.defaultStage)));
}

function updateZoneTargets(zone) {
  const target = cropDatabase[zone.defaultCrop]?.[zone.defaultStage];
  if (!target) return;
  setText(`targetN${zone.id}`, `Target: ${target.n} ppm`);
  setText(`targetP${zone.id}`, `Target: ${target.p} ppm`);
  setText(`targetK${zone.id}`, `Target: ${target.k} ppm`);
  setText(`targetPH${zone.id}`, `Target: ${target.ph}`);
  setText(`targetEC${zone.id}`, `Target: ${target.ec} mS/cm`);
  setText(`targetMoisture${zone.id}`, `Target: ${target.moisture}%`);
}

// The crop profile above is dashboard-side bookkeeping; the rig only ever learns a target through
// SET_COLUMN. This bridges the two by filling the firmware-settings inputs from the selected crop
// and stage -- it deliberately does NOT send. The operator sees the numbers, can adjust them, and
// presses "Send to ESP1", so the same validated path and the same confirmation apply as for any
// other column edit. EC and moisture have no SET_COLUMN field and stay display-only.
function fillTargetsFromCrop(zone) {
  const id = zone.id;
  const result = document.getElementById(`cfgResult${id}`);
  const show = (text, error = true) => {
    if (!result) return;
    result.textContent = text;
    result.className = `control-result${error ? " error" : ""}`;
  };
  const target = cropDatabase[zone.defaultCrop]?.[zone.defaultStage];
  if (!target) { show("That crop and stage has no stored profile. Nothing was filled in."); return; }
  const set = (elId, value) => { const el = document.getElementById(elId); if (el) el.value = String(value); };
  set(`cfgN${id}`, target.n);
  set(`cfgP${id}`, target.p);
  set(`cfgK${id}`, target.k);
  set(`cfgPH${id}`, target.ph);
  const crop = readableCropNames[zone.defaultCrop] || zone.defaultCrop;
  show(`Filled from ${crop} / ${zone.defaultStage}: N ${target.n}, P ${target.p}, K ${target.k} ppm, pH ${target.ph}. ` +
       `Press "Send to ESP1" to apply them to column ${id}.`, false);
}

// "Firmware save settings" (replaces the old ESP1-preset-table dropdown): stores whatever is
// currently filled into this column's form -- mode/enabled/schedule/window/targets -- as a named
// snapshot under irrigation/config/zones/{id}/savedSettings, purely dashboard-side bookkeeping like
// the crop/stage profile above it (ESP1 never reads this node). Does NOT send anything to ESP1 by
// itself -- selecting a saved entry back in renderZonesUI() just refills the same boxes this reads
// from, and the existing "Send to ESP1" button is what actually applies them.
function saveColumnSettings(id) {
  const result = document.getElementById(`cfgResult${id}`);
  const show = (text, error = false) => {
    if (!result) return;
    result.textContent = text;
    result.className = `control-result${error ? " error" : ""}`;
  };
  if (!isApprovedUser()) {
    show("Your account is awaiting operator approval before it can send commands to the rig.", true);
    return;
  }
  const nameInput = document.getElementById(`cfgSaveName${id}`);
  const typedName = (nameInput?.value || "").trim();
  const val = elId => document.getElementById(elId)?.value ?? "";
  const snapshot = {
    name: typedName || `Saved ${new Date().toLocaleString()}`,
    savedAt: firebase.database.ServerValue.TIMESTAMP,
    mode: val(`cfgMode${id}`), enabled: val(`cfgEnabled${id}`), schedMode: val(`cfgSched${id}`),
    winStart: val(`cfgWinStart${id}`), winEnd: val(`cfgWinEnd${id}`),
    targetN: val(`cfgN${id}`), targetP: val(`cfgP${id}`), targetK: val(`cfgK${id}`), targetPH: val(`cfgPH${id}`)
  };
  db.ref(`irrigation/config/zones/${id}/savedSettings`).push(snapshot)
    .then(() => { show(`Saved as "${snapshot.name}".`); if (nameInput) nameInput.value = ""; })
    .catch(error => show(`Could not save settings: ${error.message}`, true));
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
  const target = cropDatabase[zone.defaultCrop]?.[zone.defaultStage]?.[targetKey];
  element.classList.toggle("lacking-nutrient", Number.isFinite(Number(target)) && Number(value) < Number(target));
}

function updateDashboard() {
  const sensors = liveData.sensors || {};
  const system = liveData.system || {};
  const actuators = liveData.actuators || {};
  const diagnostics = liveData.diagnostics || {};
  setText("reservoirLevel", numberText(sensors.reservoirLevel, 1, "%"));
  setText("mixingLevel", numberText(sensors.mixingLevel, 1, "%"));
  setText("flowRate", numberText(sensors.flowRate, 1, "L/min"));
  setText("temperature", numberText(sensors.temperature, 1, "°C"));
  setText("humidity", numberText(sensors.humidity, 1, "%"));
  setText("lightLevel", numberText(sensors.lightLevel, 0, "lux"));
  setText("waterPH", numberText(sensors.waterPH, 2));
  setText("waterEC", numberText(sensors.waterEC, 2, "mS/cm"));
  setText("batteryVoltage", numberText(sensors.batteryVoltage, 2, "V"));
  setText("batteryPercent", numberText(sensors.batteryPercent, 0, "%"));
  setText("batteryCurrent", numberText(sensors.batteryCurrent, 2, "A"));
  // Watts comes from the diagnostics tree, not sensors -- ESP1 publishes battP there alongside the
  // low/critical flags. Also shown under Diagnostics > Power; this is the front-page copy.
  setText("batteryPower", numberText(diagnostics.power?.batteryPower, 1, "W"));
  setText("powerSource", rawText(system.powerSource));
  setText("liveAge", snapshotAgeText());

  const fresh = deviceIsFresh();
  setDeviceStatus("systemState", system.state || "WAITING FOR DATA", fresh ? "active" : "off");
  updateRunProgress(diagnostics.runProgress || {});

  // Pump lamps. A stale snapshot must never be drawn as a live "ON" -- if ESP1 has gone quiet we
  // do not know what the pumps are doing, and "OFF" is the only claim the page can defend.
  setPumpLamp("transferPumpStatus", fresh && Boolean(actuators.transferRunning));
  setPumpLamp("boosterPumpStatus", fresh && Boolean(actuators.boosterRunning));
  setPumpLamp("mixerPumpStatus", fresh && Boolean(actuators.mixerRunning));

  activeZones.forEach(zone => {
    zoneMetric(zone, "nitrogen", `nitrogen${zone.id}`, 1, "ppm", "n");
    zoneMetric(zone, "phosphorus", `phosphorus${zone.id}`, 1, "ppm", "p");
    zoneMetric(zone, "potassium", `potassium${zone.id}`, 1, "ppm", "k");
    zoneMetric(zone, "ph", `soilPH${zone.id}`, 2, "", "ph");
    zoneMetric(zone, "ec", `soilEC${zone.id}`, 2, "mS/cm", "ec");
    zoneMetric(zone, "moisture", `soil${zone.id}`, 0, "%", "moisture");
    // No target key for these two: the probe's own moisture is shown for comparison against the
    // blended figure, and soil temperature has no configured target to fall short of.
    const z = liveData.sensors?.zones?.[zone.id] || {};
    setText(`npkMoist${zone.id}`, numberText(z.npkMoisture, 1, "%"));
    setText(`soilTemp${zone.id}`, numberText(z.soilTemperature, 1, "°C"));
    updateZoneConfigDisplay(zone);
  });

  renderDiagnostics();
  renderRawSensors();
  renderFlowMeters();
  renderExercise();
  renderManualHold();
  updateFaultBanner();
  updateForceArmed();
  updateSystemTab();
  syncControlAvailability();
}

function setPumpLamp(id, on) {
  setDeviceStatus(id, on ? "ON" : "OFF", on ? "active" : "off");
}

function updateRunProgress(run) {
  const active = Boolean(run.active);
  const phase = rawText(run.phase, "IDLE");
  setDeviceStatus("sideRunState", active ? phase : "NO ACTIVE RUN", active ? "active" : "off");
  setDeviceStatus("runStateBadge", active ? phase : "IDLE", active ? "active" : "off");
  if (!active) {
    setText("runSummary", "No active irrigation or fertigation run reported by ESP1.");
    setText("runStage", "Unavailable");
    setText("runStageProgress", "Unavailable");
    setText("runWaterProgress", "Unavailable");
    setText("runDoseProgress", "Unavailable");
    return;
  }
  const stageOrder = hasValue(run.stageOrdinal) && hasValue(run.stageTotal) ? `Stage ${run.stageOrdinal} of ${run.stageTotal}` : "Stage order unavailable";
  setText("runSummary", `${run.operation || "Run"} for Zone ${run.zone || "?"} — ${stageOrder}.`);
  setText("runStage", rawText(run.stage));
  setText("runStageProgress", hasValue(run.stageTargetLiters)
    ? `${numberText(run.stageLiters, 1, "L")} of ${numberText(run.stageTargetLiters, 1, "L")}`
    : stageOrder);
  setText("runWaterProgress", hasValue(run.waterTargetLiters)
    ? `${numberText(run.waterDeliveredLiters, 1, "L")} of ${numberText(run.waterTargetLiters, 1, "L")}`
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

function renderDiagnostics() {
  const container = document.getElementById("diagnosticsGrid");
  if (!container) return;
  const d = liveData.diagnostics || {};
  container.innerHTML = "";
  const current = deviceIsFresh();
  const diagnosticsAvailable = Object.keys(d).length > 0;
  const groups = [
    ["Live snapshot", [["Snapshot", current ? "Current" : "Stale or unavailable"], ["Received", snapshotAgeText()], ["ESP1 state", liveData.system?.state || "Unavailable"], ["Payload used", liveData.meta?.docUsed ? `${liveData.meta.docUsed} / ${liveData.meta.docCapacity} B` : "Unavailable"]]],
    ["Network", [["Wi-Fi enabled", booleanText(d.network?.wifiEnabled, "Enabled", "Disabled")], ["Wi-Fi link", booleanText(d.network?.wifiConnected, "Connected", "Disconnected")], ["Wi-Fi RSSI", hasValue(d.network?.wifiRssi) ? `${d.network.wifiRssi} dBm` : "Unavailable"]]],
    ["Firebase", [["Enabled", booleanText(d.firebase?.enabled, "Enabled", "Disabled")], ["RTDB URL", booleanText(d.firebase?.urlConfigured, "Configured", "Not configured")], ["Device account", booleanText(d.firebase?.deviceCredentialsConfigured, "Configured", "Not configured")], ["Signed in", booleanText(d.firebase?.signedIn, "Yes", "No")], ["Last upload", Number(d.firebase?.attempts || 0) > 0 ? booleanText(d.firebase?.lastUploadOk, "OK", "Failed") : "Not attempted"], ["Last HTTP", Number(d.firebase?.attempts || 0) > 0 ? rawText(d.firebase?.lastHttp) : "Not attempted"], ["TLS validation", booleanText(d.firebase?.tlsValidationEnabled, "Enabled", "Disabled")], ["Last auth issue", rawText(d.firebase?.lastAuthError, "None")]]],
    ["ThingSpeak", [["Configured", booleanText(d.thingspeak?.configured, "Configured", "Not configured")], ["Last upload", d.thingspeak?.attempted ? booleanText(d.thingspeak?.lastUploadOk, "OK", "Failed") : "Not attempted"]]],
    ["Supabase logs", [["Configured", booleanText(d.supabase?.configured, "Configured", "Not configured")], ["Upload", booleanText(d.supabase?.uploadBusy, "In progress", "Idle")], ["Last uploaded day", Number(d.supabase?.lastUploadedDay || 0) > 0 ? rawText(d.supabase?.lastUploadedDay) : "No completed upload"]]],
    ["System", [["Work order", booleanText(d.system?.workOrderActive, "Active", "Inactive")], ["Pending run", booleanText(d.system?.pendingRun, "Yes", "No")], ["Last fault", rawText(d.system?.lastFault, "None") + (explainFaultCode(d.system?.lastFault) ? ` — ${explainFaultCode(d.system?.lastFault)}` : "")], ["Fault time", rawText(d.system?.lastFaultTime, "None")]]],
    ["ESP2", [["Available", booleanText(d.esp2?.available, "Available", "Unavailable")], ["Power", booleanText(d.esp2?.powered, "On", "Off")], ["Communication", booleanText(d.esp2?.communicationLost, "Lost", "OK")], ["Last response age", formatAge(d.esp2?.lastResponseAgeMs)]]],
    ["Nano & sensors", [["Last sample age", formatAge(d.nano?.lastSampleAgeMs)], ["Environment", booleanText(d.nano?.environmentValid, "Valid", "Invalid")], ["Tank", booleanText(d.nano?.tankValid, "Valid", "Invalid")], ["Light", booleanText(d.nano?.lightValid, "Valid", "Invalid")]]],
    ["RTC / SD", [["RTC", booleanText(d.peripherals?.rtcOk, "OK", "Not OK")], ["SD card", booleanText(d.peripherals?.sdOk, "OK", "Not OK")], ["Battery sensor", booleanText(d.peripherals?.batterySensorOk, "OK", "Not OK")]]],
    ["GSM", [["SIM", booleanText(d.gsm?.simReady, "Ready", "Not ready")], ["Network", booleanText(d.gsm?.networkRegistered, "Registered", "Not registered")], ["RSSI", hasValue(d.gsm?.rssi) ? String(d.gsm.rssi) : "Unavailable"], ["CREG", rawText(d.gsm?.creg)], ["Last health age", formatAge(d.gsm?.lastHealthAgeMs)]]],
    ["Power", [["Battery low", booleanText(d.power?.batteryLow, "Yes", "No")], ["Battery critical", booleanText(d.power?.batteryCritical, "Yes", "No")], ["Current", numberText(d.power?.batteryCurrent, 2, "A")], ["Power", numberText(d.power?.batteryPower, 1, "W")]]],
    ["Actuator status", [["Relay feedback", d.actuator?.relayFeedback === "notReported" ? "Not reported by ESP1" : rawText(d.actuator?.relayFeedback)], ["Work order", booleanText(d.actuator?.workOrderActive, "Active", "Inactive")], ["Pump test", booleanText(d.actuator?.pumpTestActive, "Active", "Inactive")], ["Pump under test", rawText(d.actuator?.pumpUnderTest)]]]
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
  if (Boolean(d.actuationsDisabled)) return "actuations are currently disabled";
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
  COL_DISABLED:   "the target column was disabled"
};

function renderCommandHistory() {
  const container = document.getElementById("commandHistory");
  if (!container) return;
  container.innerHTML = "";
  const commands = commandData.slice().sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0));
  if (!commands.length) {
    container.innerHTML = '<p class="muted">No command history received yet.</p>';
    return;
  }
  commands.slice(0, 8).forEach(command => {
    const row = document.createElement("article");
    row.className = "command-row";
    const top = document.createElement("div");
    const name = document.createElement("strong");
    const badge = document.createElement("span");
    name.textContent = rawText(command.type, "Unknown command").replaceAll("_", " ");
    badge.className = `command-badge ${commandStatusTone(command.status)}`;
    badge.textContent = rawText(command.status, "unknown");
    top.append(name, badge);
    const detail = document.createElement("p");
    let detailText = rawText(command.detail, "Awaiting ESP1 status");
    // New-F1 (audit): DIAG_SWEEP shares the same generic "not safely idle" wording as FORCE_RUN used
    // to (diagRemoteStart() has since been given specific reasons in the dev-tree firmware, but the
    // OLD flashed firmware's DIAG_SWEEP path -- and any other command that ever reuses this literal
    // text -- still benefits from the same best-guess decoding). explainNotSafelyIdle() doesn't
    // reference either command by name, so it's safe to reuse for both.
    if ((command.type === "FORCE_RUN" || command.type === "DIAG_SWEEP") && /not safely idle/i.test(detailText)) {
      detailText += ` — likely reason: ${explainNotSafelyIdle()}`;
    }
    if (command.type === "FORCE_RUN") {
      // New-F2 (audit): forceTick()'s fire-time abort reports one of 8 short internal codes verbatim
      // (e.g. "cancelled before start: RES_LOW"). Swap in the matching phrase when recognised.
      detailText = detailText.replace(/cancelled before start: (\S+)/,
        (full, c) => `cancelled before start: ${FORCE_ABORT_REASONS[c] || c}`);
    }
    detail.textContent = `${formatTimestamp(command.requestedAt)} — ${detailText}`;
    row.append(top, detail);
    container.appendChild(row);
  });
}

/* ---- Fault / recovery banner ------------------------------------------------------------------
 * Rendered from diagnostics.fault, so the choices offered here are exactly the ones the LCD recovery
 * menu offers. Recovery commands are sent with {emergency:true}: they bypass the freshness gate and
 * the cooldown on purpose, because a stale snapshot is often *why* you are trying to recover, and a
 * dashboard that locks you out at that moment is worse than useless. */
let faultAckLocalUntil = 0;          // client-side half of the "ask me again in 2 minutes" snooze

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
  estop:           { text: "Cleared by an emergency stop.", tone: "" },
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
  const soil = text.match(/^SOIL_MISSING\s+(COL_[ABC])\|(\S+)\|raw=(-?\d+),(-?\d+)/);
  if (soil) {
    const [, col, reason, r1, r2] = soil;
    return reason === "PROBE_DIVERGE"
      ? `${col}'s two soil probes disagree beyond tolerance (raw ${r1}/${r2}) — the NPK sensor is covering the reading for now, but one probe likely needs attention.`
      : `${col} has no usable soil reading (raw ${r1}/${r2}) — check that column's probe wiring/connector.`;
  }
  // New-F3/F4/F5 (audit): same decode pattern, for the three fault codes that gained measured-value
  // detail in this pass (ESP2/src/main.cpp -- PWR_FAIL/SAFE_STOP,MIXER_OC/DOSE_TIMEOUT).
  const overcurrent = text.match(/^PWR_FAIL\s+OVERCURRENT\|i=([\d.]+)/);
  if (overcurrent) return `Current draw hit ${overcurrent[1]}A during this stage — check for a jammed pump/valve or a wiring fault.`;
  const voltage = text.match(/^PWR_FAIL\s+VOLTAGE\|v=([\d.]+)\|(LOW|HIGH)/);
  if (voltage) return voltage[2] === "LOW"
    ? `AC supply sagged to ${voltage[1]}V — check the mains/inverter output and connections.`
    : `AC supply spiked to ${voltage[1]}V — check the mains/inverter output.`;
  const noCurrent = text.match(/^PWR_FAIL\s+NO_CURRENT\|i=([\d.]+)/);
  if (noCurrent) return `The pump was commanded on but drew almost no current (${noCurrent[1]}A) for too long — likely a failed pump, tripped breaker, or loose wiring.`;
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
  const lockedOut = Boolean(d.actuationsDisabled);

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
  setText("faultTitle", held ? rawText(f.code, "Fault reported by ESP2")
                       : stopped ? "The system is stopped"
                       : "Monitoring only — nothing will run");
  const faultExplain = held ? explainFaultCode(f.code) : "";
  setText("faultDetail", held
    ? `Reported ${rawText(f.at, "at an unknown time")}. ESP2 is paused with the actuator bank de-energised; pick how to continue.` + (faultExplain ? ` ${faultExplain}` : "")
    : stopped
      ? "All actuators are off and ESP2 is unpowered. Returning to normal re-powers and re-validates ESP2 before anything runs."
      : "Sensors, logging and telemetry are still running. Scheduled runs, pump exercises and forced runs are all blocked until actuations are re-enabled.");
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

  const rec = document.getElementById("faultRecovery");
  if (rec) rec.hidden = !held;
  const estopBtn = document.getElementById("estopRecoverBtn");
  if (estopBtn) estopBtn.hidden = !stopped || held;
  const enableBtn = document.getElementById("enableActBtn");
  if (enableBtn) enableBtn.hidden = !lockedOut;
  const disableBtn = document.getElementById("disableActBtn");
  if (disableBtn) disableBtn.hidden = lockedOut;
}

/* ---- Armed forced-run countdown ---------------------------------------------------------------
 * Driven by diagnostics.forceArmed, NOT by whether this browser sent the request -- an LCD-armed run
 * must show here too. secondsLeft is re-seeded on every snapshot and ticked locally in between, so
 * the number stays smooth at a 20-60 s publish cadence without ever drifting past the truth. */
let armedSeed = null;                // { at: epoch ms, left: seconds } from the last snapshot

function updateForceArmed() {
  const panel = document.getElementById("forceArmedPanel");
  if (!panel) return;
  const a = liveData.diagnostics?.forceArmed || {};
  if (!a.armed || !deviceIsFresh()) { panel.hidden = true; armedSeed = null; return; }
  panel.hidden = false;
  const doses = a.doseMl || {};
  const fert = Number(doses.A || 0) + Number(doses.B || 0) + Number(doses.C || 0) > 0;
  setText("forceArmedDetail",
    `${fert ? "Fertigation" : "Irrigation"} · column ${rawText(a.columns, "?")} · ${numberText(a.liters, 1, "L")}`
    + (fert ? ` · A/B/C ${Number(doses.A || 0)}/${Number(doses.B || 0)}/${Number(doses.C || 0)} mL` : "")
    + ` · armed from the ${a.source === "web" ? "dashboard" : "LCD"}`);
  armedSeed = { at: Date.now(), left: Number(a.secondsLeft || 0) };
  tickArmedCountdown();
}

function tickArmedCountdown() {
  if (!armedSeed) return;
  const left = Math.max(0, armedSeed.left - Math.floor((Date.now() - armedSeed.at) / 1000));
  setDeviceStatus("forceArmedCountdown", left > 0 ? `STARTS IN ${left}s` : "STARTING…", "active");
}

/* ---- Raw sensor diagnostics -------------------------------------------------------------------- */
function ageFlag(ms) {
  const v = Number(ms);
  if (!Number.isFinite(v) || v === 0xFFFFFFFF) return "never";
  if (v > 90000) return "STALE";                   // mirrors the firmware's NANO_STALE_MS
  return "ok";
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
        ["Flow", numberText(r.tank?.flowLpm, 2, "L/min")],
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

// Column Mode website parity: a read-only summary of what ESP1 ACTUALLY has stored for this column
// right now, from live telemetry -- distinct from the edit form below it, which is write-only and
// always defaults to "(unchanged)"/blank. Never invents a value: shows "Unavailable" field-by-field
// exactly like every other live reading on this page when the snapshot hasn't reported it yet.
function updateZoneConfigDisplay(zone) {
  const el = document.getElementById(`cfgCurrent${zone.id}`);
  if (!el) return;
  const z = liveData.sensors?.zones?.[zone.id];
  if (!z || !hasValue(z.enabled)) { el.textContent = "Current configuration: Unavailable"; return; }
  const enabled = Boolean(z.enabled);
  const mode = rawText(z.mode, "Unavailable");
  const sched = z.schedMode === 1 ? `Manual ${minutesToHhmm(z.winStart)}-${minutesToHhmm(z.winEnd)}`
              : z.schedMode === 0 ? `Auto ${minutesToHhmm(z.winStart)}-${minutesToHhmm(z.winEnd)}`
              : "Unavailable";
  const targets = ["targetN", "targetP", "targetK", "targetPH"].every(k => hasValue(z[k]))
    ? `N ${z.targetN} / P ${z.targetP} / K ${z.targetK} ppm, pH ${z.targetPH}`
    : "Unavailable";
  el.textContent = `Current configuration: ${enabled ? "Enabled" : "Disabled"} — ${enabled ? mode : "n/a"} — ` +
                    `Schedule: ${enabled ? sched : "n/a"} — Targets: ${enabled ? targets : "n/a"}`;
}

// One-click Auto / Irrigation-only / Off, mirroring the LCD's Column Mode screen exactly: mode is
// null only for "Off", which -- like the LCD -- disables the column WITHOUT touching its stored
// mode (COLUMN_ENABLED and col[c].mode are independent on ESP1; re-enabling later needs its own
// click, same as the physical unit). Sends immediately through the exact same validated SET_COLUMN
// command and approval gate every other control on this page already uses -- not a new command,
// not a parallel path, just a one-step shortcut for the two dropdowns below it.
function quickSetColumnMode(id, mode) {
  const result = document.getElementById(`cfgResult${id}`);
  const show = (text, error = false) => {
    if (!result) return;
    result.textContent = text;
    result.className = `control-result${error ? " error" : ""}`;
  };
  const payload = { col: id, enabled: mode ? 1 : 0 };
  if (mode) payload.mode = mode;
  const label = mode === "AUTO" ? "Auto" : mode === "IRRIGATION_ONLY" ? "Irrigation only" : "Off";
  show(`Sending "${label}" to ESP1…`);
  queueCommand("SET_COLUMN", payload);
}

function submitColumnConfig(id) {
  const result = document.getElementById(`cfgResult${id}`);
  const show = (text, error = true) => {
    if (!result) return;
    result.textContent = text;
    result.className = `control-result${error ? " error" : ""}`;
  };
  // Only send what was actually filled in: the firmware treats absent fields as "leave unchanged",
  // so a partial edit cannot clobber the rest of the column's configuration.
  const payload = { col: id };
  const mode = document.getElementById(`cfgMode${id}`)?.value;
  if (mode) payload.mode = mode;
  const en = document.getElementById(`cfgEnabled${id}`)?.value;
  if (en !== "") payload.enabled = en === "1";
  const sm = document.getElementById(`cfgSched${id}`)?.value;
  if (sm !== "") payload.schedMode = Number(sm);
  // Window fields only apply -- and are only sent -- while Manual window is the value about to be
  // submitted; a leftover value from an earlier edit must not sneak in once switched back to
  // Automatic (or left at "(unchanged)"), matching updateWindowVisibility()'s identical "1" check.
  if (sm === "1") {
    const ws = hhmmToMinutes(document.getElementById(`cfgWinStart${id}`)?.value);
    const we = hhmmToMinutes(document.getElementById(`cfgWinEnd${id}`)?.value);
    if (ws !== null) payload.winStart = ws;
    if (we !== null) payload.winEnd = we;
    if (ws !== null && we !== null && ws === we) { show("Window start and end cannot be the same. Nothing was sent."); return; }
  }
  // Bounds mirror the firmware's own applyColumnTarget() (0-2000 ppm, pH 3-9) -- checked here too so
  // a mistyped value is refused before it round-trips to ESP1 and back, matching the pattern
  // submitForceRun() already uses for its own fields. The HTML min/max attributes alone never fire
  // here (this is a plain button, not a form submit), so this is the only real client-side check.
  const nums = {
    targetN:  { el: `cfgN${id}`,  label: "N",  min: 0, max: 2000 },
    targetP:  { el: `cfgP${id}`,  label: "P",  min: 0, max: 2000 },
    targetK:  { el: `cfgK${id}`,  label: "K",  min: 0, max: 2000 },
    targetPH: { el: `cfgPH${id}`, label: "pH", min: 3, max: 9 }
  };
  for (const [key, { el, label, min, max }] of Object.entries(nums)) {
    const raw = document.getElementById(el)?.value;
    if (raw === "" || raw === undefined) continue;
    const v = Number(raw);
    if (!Number.isFinite(v)) { show(`${label} is not a number. Nothing was sent.`); return; }
    if (v < min || v > max) { show(`${label} must be ${min}-${max}. Nothing was sent.`); return; }
    payload[key] = v;
  }
  if (Object.keys(payload).length < 2) { show("Nothing to change — fill in at least one field."); return; }
  show("Sending to ESP1…", false);
  queueCommand("SET_COLUMN", payload);
}

// Reflects each zone's own most recent SET_COLUMN outcome inline, under its own "Send to ESP1"
// button -- previously that message froze at "Sending to ESP1..." forever, so the real outcome
// (accepted/completed/rejected, and why) only ever showed up in the separate, shared Command History
// panel below. Mirrors renderPulseResult()'s pattern: re-scan commandData for the newest match each
// time the commands listener fires, keyed on both type and the column letter so three zones' results
// don't collide.
function renderColumnCommandResults() {
  activeZones.forEach(zone => {
    const result = document.getElementById(`cfgResult${zone.id}`);
    if (!result) return;
    const latest = commandData
      .filter(c => c.type === "SET_COLUMN" && c.payload && c.payload.col === zone.id)
      .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
    if (!latest) return;
    result.textContent = `${rawText(latest.status, "pending")} — ${rawText(latest.detail, "waiting for ESP1")}`;
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
  if (!Number.isFinite(delaySeconds) || delaySeconds < 0 || delaySeconds > 300) {
    show("Start delay must be between 0 and 300 seconds. Nothing was sent."); return;
  }

  const fertigation = doseMl.A > 0 || doseMl.B > 0 || doseMl.C > 0;
  show(`Queued ${fertigation ? "fertigation" : "irrigation"}: ${liters} L to ${columns}`
     + `${fertigation ? ` with A/B/C ${doseMl.A}/${doseMl.B}/${doseMl.C} mL` : ""}`
     + `, starting in ${delaySeconds}s. Watch the armed panel below — you can still cancel.`, false);
  queueCommand("FORCE_RUN", { columns, liters, doseMl, delaySeconds });
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
  releaseManualHold();
  mtHoldStop(true);
  auth?.signOut().catch(error => setCommandStatus(`Sign out failed: ${error.message}`, "error"));
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
  }
});
document.getElementById("transferPumpBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "transfer" }, { operatorOnly: true }));
document.getElementById("boosterPumpBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "booster" }, { operatorOnly: true }));
document.getElementById("mixerBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "mixer" }, { operatorOnly: true }));
document.getElementById("emergencyStop")?.addEventListener("click", () => queueCommand("EMERGENCY_STOP", {}, { emergency: true }));
document.getElementById("forceRunForm")?.addEventListener("submit", submitForceRun);

// Recovery controls all pass {emergency:true}. They must work when the snapshot is stale or the
// cooldown is armed -- being unable to recover the rig because the page thinks it is offline is the
// exact failure this whole feature exists to remove.
document.querySelectorAll("#faultRecovery button[data-recover]").forEach(btn =>
  btn.addEventListener("click", () => queueCommand("RECOVER", { action: btn.dataset.recover }, { emergency: true })));
document.getElementById("estopRecoverBtn")?.addEventListener("click", () => queueCommand("ESTOP_RECOVER", {}, { emergency: true }));

// These controls exist twice -- in the fault banner and in the Controls tab -- because the banner is
// hidden while the system is healthy, which is exactly when you might want to disable actuations or
// reboot a module. One handler each, bound to both ids, so the two copies cannot drift.
function bindAll(ids, handler) {
  ids.forEach(id => document.getElementById(id)?.addEventListener("click", handler));
}
bindAll(["enableActBtn", "enableActBtn2"],  () => queueCommand("ENABLE_ACTUATIONS", {}, { emergency: true }));
bindAll(["disableActBtn", "disableActBtn2"], () => {
  if (!confirm("Disable actuations? Any run in progress is stopped immediately, and nothing will run again until you re-enable. Monitoring continues.")) return;
  queueCommand("DISABLE_ACTUATIONS", {}, { emergency: true });
});
bindAll(["rebootNanoBtn", "rebootNanoBtn2"], () => queueCommand("REBOOT", { target: "nano" }, { emergency: true }));
bindAll(["rebootEsp2Btn", "rebootEsp2Btn2"], () => queueCommand("REBOOT", { target: "esp2" }, { emergency: true }));
bindAll(["rebootEsp1Btn", "rebootEsp1Btn2"], () => {
  // ESP1 owns the Firebase link, so this one goes quiet for ~15 s before it comes back.
  if (!confirm("Reboot ESP1? The dashboard will lose contact for about 15 seconds while it restarts.")) return;
  queueCommand("REBOOT", { target: "esp1" }, { emergency: true });
});
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
  if (!confirm(`Set ESP1's clock to ${dateVal} ${timeVal}? This affects every column's irrigation schedule.`)) return;
  show("Sending to ESP1…", false);
  queueCommand("SET_CLOCK", { clkY: y, clkMo: mo, clkD: d, clkH: h, clkMi: mi }, { operatorOnly: true });
});

document.getElementById("sysThreshSendBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysThreshResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  const start = Number(document.getElementById("sysThreshStart")?.value);
  const stop  = Number(document.getElementById("sysThreshStop")?.value);
  const gap   = Number(document.getElementById("sysThreshGap")?.value);
  if (![start, stop, gap].every(Number.isFinite)) { show("Fill in all three fields with numbers. Nothing was sent."); return; }
  if (start < 0 || start > 100 || stop < 0 || stop > 100) { show("Start/stop must be 0-100. Nothing was sent."); return; }
  if (gap < 0 || gap > 500) { show("Gap must be 0-500. Nothing was sent."); return; }
  show("Sending to ESP1…", false);
  queueCommand("SET_THRESH", { thStart: start, thStop: stop, thGap: gap }, { operatorOnly: true });
});

document.getElementById("sysRestoreDefaultsBtn")?.addEventListener("click", () => {
  const result = document.getElementById("sysRestoreResult");
  const show = (text, error = true) => { if (result) { result.textContent = text; result.className = `control-result${error ? " error" : ""}`; } };
  if (!confirm("Restore Defaults? This resets every column's mode, targets, name, and schedule window, " +
               "plus the global thresholds, to factory values. This CANNOT be undone. Calibration, " +
               "column-enabled wiring, and WiFi/ThingSpeak setup are kept.")) return;
  show("Sending to ESP1…", false);
  queueCommand("RESTORE_DEFAULTS", {}, { operatorOnly: true });
});

// Read-only: current device time/RTC health (for Set Clock) and LCD Lock status. No write path is
// offered for the lock -- see the info tooltip on that panel for why.
function updateSystemTab() {
  setText("sysClockCurrent", rawText(liveData.system?.deviceTime, "Unavailable"));
  setText("sysClockRtcOk", booleanText(liveData.system?.rtcOk, "OK", "Not OK"));
  setText("sysLockStatus", booleanText(liveData.diagnostics?.system?.lcdLocked, "Locked", "Unlocked"));
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
  // C-H4/R workaround (audit): the CURRENTLY-FLASHED ESP1 firmware parses this into a 32-bit `long`
  // (`long seq = doc["seq"] | -1`). A raw Date.now() (~1.7e12) doesn't fit a 32-bit long, so
  // ArduinoJson's `|` silently falls back to -1 on EVERY poll -- seq != webManualSeq then never
  // becomes newly true, and Manual/Test's "Proceed" gate can never re-arm on that hardware. Sending
  // whole seconds instead keeps a monotonically increasing value that fits a 32-bit long until 2038,
  // fixing this without a reflash. 1-second resolution is coarser than the 20 s keep-alive interval
  // this is used with, so it cannot collide in normal use. The unflashed dev-tree firmware already
  // reads this as int64_t and accepts either form, so this is compatible with both trees.
  const seq = Math.floor(Date.now() / 1000);
  return db.ref("irrigation/manual").set({ seq, want })
    .catch(error => { setCommandStatus(`Could not reach the rig: ${error.message}`, "error"); return false; });
}

/* Hold-to-run (2026-09-10): mirrors writeManualHold()'s own gate/seq pattern, but writes
 * irrigation/testHold = {seq, bit, want} -- ESP1's firebasePollTestHold() arms/holds/releases the
 * SAME ESP2 TEST,HOLD/TEST,RELEASE dead-man protocol the LCD Testing screen and the Timed-pulses
 * feature above already use, just driven by a held button instead of a physical one or a timer. */
function writeTestHold(want, bit) {
  if (!currentUserIsSignedIn() || !isApprovedUser()) return Promise.resolve(false);
  // Same acquire-only gate as writeManualHold(): only privileged-tier accounts may START a hold;
  // releasing always goes through regardless of tier, so a lost grant (e.g. an expiring temp-access
  // window) can never leave a relay stuck energised because the release write itself got refused.
  if (want && !canAccessPrivilegedTabs()) return Promise.resolve(false);
  const seq = Date.now();
  return db.ref("irrigation/testHold").set({ seq, bit, want })
    .catch(error => { setCommandStatus(`Could not reach the rig: ${error.message}`, "error"); return false; });
}

let mtHoldActiveBtn = null;   // the button currently mid-hold, or null
let mtHoldKeepAliveTimer = null;
let mtHoldArmedTarget = null; // a corrosive (pH) target the operator just confirmed, awaiting the hold press
let mtHoldArmedTimer = null;

function mtHoldStop(clearedByRelease) {
  const btn = mtHoldActiveBtn;
  mtHoldActiveBtn = null;
  if (mtHoldKeepAliveTimer) { clearInterval(mtHoldKeepAliveTimer); mtHoldKeepAliveTimer = null; }
  if (!btn) return;
  btn.classList.remove("holding");
  const target = btn.dataset.holdTarget;
  if (clearedByRelease) writeTestHold(false, target);
  const startedMs = Number(btn.dataset.holdStartMs || 0);
  if (startedMs) reportHoldResult(target, Date.now() - startedMs);
  delete btn.dataset.holdStartMs;
}

function mtHoldStart(btn) {
  const target = btn.dataset.holdTarget;
  writeTestHold(true, target);
  mtHoldActiveBtn = btn;
  btn.classList.add("holding");
  btn.dataset.holdStartMs = String(Date.now());
  // A confirmed pH arm is consumed by the press it was meant for -- releasing and pressing again
  // needs a fresh confirm(), same one-shot spirit as the Timed-pulses confirm above.
  if (mtHoldArmedTarget === target) {
    mtHoldArmedTarget = null;
    if (mtHoldArmedTimer) { clearTimeout(mtHoldArmedTimer); mtHoldArmedTimer = null; }
    btn.textContent = btn.dataset.holdLabel || btn.textContent;
  }
  // Refresh well inside ESP1's own TEST_HOLD_BROWSER_TIMEOUT_MS (8 s) -- comfortably clears its
  // 3 s command-poll cadence even if one poll or one write round-trip is slow.
  mtHoldKeepAliveTimer = setInterval(() => writeTestHold(true, target), 1200);
}

document.querySelectorAll(".hold-btn").forEach(btn => {
  const isCorrosive = btn.classList.contains("danger");
  const start = ev => {
    ev.preventDefault();
    if (btn.disabled || mtHoldActiveBtn) return;
    const target = btn.dataset.holdTarget;
    if (isCorrosive && mtHoldArmedTarget !== target) return;   // needs the confirm click first, below
    mtHoldStart(btn);
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
      if (btn.disabled || mtHoldActiveBtn) return;
      const target = btn.dataset.holdTarget;
      if (mtHoldArmedTarget === target) return;   // already armed from a previous click
      if (!confirm(`Hold this button to dispense pH adjuster (${target === "phUp" ? "UP" : "DOWN"})? This is corrosive and goes into the mixing tank. It stops the instant you release it.`)) return;
      mtHoldArmedTarget = target;
      btn.textContent = `${btn.dataset.holdLabel || btn.textContent} — press & hold now`;
      if (mtHoldArmedTimer) clearTimeout(mtHoldArmedTimer);
      mtHoldArmedTimer = setTimeout(() => { mtHoldArmedTarget = null; btn.textContent = btn.dataset.holdLabel || btn.textContent; }, 8000);
    });
    btn.dataset.holdLabel = btn.textContent;
  }
});

// The pulse verdict comes back as the command's own status detail (see renderPulseResult() above);
// hold-to-run has no command entry to read back (testHold isn't a queued command, just a live node),
// so this just confirms locally that a hold ran and for roughly how long -- ESP2's own TEST,FLOW
// reply already surfaces through the LCD/logs for anyone who needs the metered volume.
function reportHoldResult(target, ms) {
  const el = document.getElementById("mtHoldResult");
  if (!el) return;
  el.textContent = `Ran "${target}" for ~${(ms / 1000).toFixed(1)}s.`;
  el.className = "control-result";
}

function requestManualHold() {
  writeManualHold(true);
  if (mtHoldTimer) clearInterval(mtHoldTimer);
  // 20 s against a 60 s lease: two keep-alives may be lost before the rig takes the hold back.
  mtHoldTimer = setInterval(() => writeManualHold(true), 20000);
}

function releaseManualHold() {
  if (mtHideTimer) { clearTimeout(mtHideTimer); mtHideTimer = null; }
  if (mtHoldTimer) { clearInterval(mtHoldTimer); mtHoldTimer = null; }
  writeManualHold(false);
}

// Reflect what ESP1 decided. The controls stay hidden until it actually says "held" -- the gate's
// Proceed button reveals them, but only once the rig has agreed to hand over control.
function renderManualHold() {
  const wm = liveData.diagnostics?.webManual || {};
  const state = String(wm.state || "idle");
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
    // Checked ahead of every state branch, including "held" -- a stale snapshot can't be trusted to
    // mean the rig is still there, and telling the operator "Manual mode active" from stale data would
    // be actively misleading.
    else if (!deviceIsFresh()) note.textContent = `ESP1 appears offline — ${snapshotAgeText()}. Your request is queued but cannot be granted until it reconnects.`;
    else if (state === "held")    note.textContent = `Manual mode active — the rig is out of automatic${wm.secondsLeft ? ` (renews, ${wm.secondsLeft}s left)` : ""}.`;
    else if (state === "refused") note.textContent = `The rig refused manual mode: ${rawText(wm.reason, "not idle")}. Wait for it to finish, then reopen this tab.`;
    else if (state === "revoked") note.textContent = "The rig operator took control at the LCD.";
    else note.textContent = "Requesting manual control from the rig…";
    note.className = (state === "held") ? "field-note" : "control-result error";
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
  // at all. Now requires the same privileged tier as the tab itself (Main/Creator operator, an active
  // 1h temp grant, or a persistent Sub-operator grant).
  if (proceed) {
    const privileged = canAccessPrivilegedTabs();
    proceed.disabled = !privileged;
    proceed.title = privileged ? "" : !isApprovedUser()
      ? "Your account is awaiting operator approval before it can use this control."
      : "This control is limited to the main/creator operator account, or an account with an active temporary/Sub-operator grant.";
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
      controlsNote.textContent = `Waiting for the rig to grant manual mode (currently: ${state})…`;
      controlsNote.className = "control-result error";
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
  if (on && !canAccessPrivilegedTabs()) return;
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
  } else if (t === "mixer") {
    note.textContent = "The mixer has no flow meter, so this reports no flow reading — only that the relay ran.";
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
  const latest = commandData
    .filter(c => c.type === "TEST_PULSE")
    .sort((a, b) => Number(b.requestedAt || 0) - Number(a.requestedAt || 0))[0];
  if (!latest) return;
  const detail = String(latest.detail || "");
  el.textContent = `${rawText(latest.status, "pending")} — ${detail || "waiting for ESP1"}`;
  el.className = "control-result"
    + (/NO FLOW/i.test(detail) || latest.status === "failed" || latest.status === "rejected" ? " error" : "");
}

// Flow-meter table. Shares the sweep with the Diagnostics tab -- one DIAG_SWEEP fills both.
const MT_FLOW_LABEL = {
  FLOW_RESMIX: "Reservoir → mix", FLOW_MIXIRR: "Mix → column",
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
  const seconds = Number(document.getElementById("exSeconds")?.value || 5);
  const out = document.getElementById("exResult");
  const show = (t, err = true) => { if (out) { out.textContent = t; out.className = `control-result${err ? " error" : ""}`; } };
  if (!Number.isFinite(seconds) || seconds < 1 || seconds > 10) { show("Duration must be 1-10 seconds. Nothing was sent."); return; }
  show(on ? `Enabling the exercise at ${seconds}s per pump...` : "Turning the preventive exercise off...", false);
  exTouched = false;                       // let the next snapshot confirm what ESP1 actually stored
  queueCommand("SET_EXERCISE", { exerciseEnabled: on, exerciseSeconds: seconds }, { operatorOnly: true });
});

document.getElementById("mtProceed")?.addEventListener("click", () => setManualTestArmed(true));
document.getElementById("mtBack")?.addEventListener("click", () => {
  setManualTestArmed(false);
  document.querySelector('.tab[data-view="dashboard"]')?.click();
});
document.getElementById("pulseTarget")?.addEventListener("change", updatePulseNote);
document.getElementById("pulseSeconds")?.addEventListener("input", updatePulseNote);
document.getElementById("pulseBtn")?.addEventListener("click", () => {
  const target = document.getElementById("pulseTarget")?.value || "transfer";
  const seconds = Number(document.getElementById("pulseSeconds")?.value || 5);
  if (!Number.isFinite(seconds) || seconds < 1 || seconds > 15) {
    setCommandStatus("Pulse not sent: duration must be 1-15 seconds.", "error"); return;
  }
  if ((target === "phUp" || target === "phDn") &&
      !confirm(`Dispense pH adjuster for ${seconds} s? This is corrosive and goes into the mixing tank.`)) return;
  queueCommand("TEST_PULSE", { target, seconds }, { operatorOnly: true });
});
// The Diagnostics tab has its own sweep button; both queue the same DIAG_SWEEP and fill both tables.
// Both are plain buttons (not form submits), so the HTML min/max on the paired input never fires --
// same reason every other actuating numeric field on this page validates in JS before sending.
function sweepSecondsOrReject(inputId) {
  const seconds = Number(document.getElementById(inputId)?.value);
  if (!Number.isFinite(seconds) || seconds < 10 || seconds > 120) {
    setCommandStatus("Sweep not sent: length must be 10-120 seconds.", "error");
    return null;
  }
  return seconds;
}
document.getElementById("sweepBtn")?.addEventListener("click", () => {
  const seconds = sweepSecondsOrReject("sweepSeconds");
  if (seconds !== null) queueCommand("DIAG_SWEEP", { seconds });
});
document.getElementById("mtSweepBtn")?.addEventListener("click", () => {
  const seconds = sweepSecondsOrReject("mtSweepSeconds");
  if (seconds !== null) queueCommand("DIAG_SWEEP", { seconds }, { operatorOnly: true });
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
  // Leaving Manual/Test re-arms its gate, so you can never land back on live hardware controls
  // already unlocked from a previous visit.
  // Entering Manual/Test asks the rig for the hold; leaving gives it straight back rather than
  // waiting out the 60 s lease.
  if (view === "manualtest") requestManualHold();
  else { setManualTestArmed(false); releaseManualHold(); mtHoldStop(true); }
  document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
  document.querySelectorAll(".view").forEach(v => v.classList.toggle("active", v.id === view));
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

// The snapshot ages between pushes, so the freshness gate has to be re-evaluated on a timer as well
// as on each update -- otherwise a device that goes silent leaves the controls enabled indefinitely.
setInterval(() => {
  setText("liveAge", snapshotAgeText());
  syncControlAvailability();
  // A temporary Manual/Test + System grant (see canAccessPrivilegedTabs()) expires by clock alone --
  // no Firebase write happens at the exact expiry moment, so nothing else would re-check this. 15 s
  // granularity matches the existing staleness-recheck cadence this tick already runs.
  refreshOperatorUI();
}, 15000);

// 1 s tick: the armed-run countdown has to move between snapshots (ESP1 publishes every 20-60 s),
// and the fault banner has to re-raise itself the moment a "do nothing" snooze expires -- that
// re-prompt is the whole point of the option.
setInterval(() => {
  tickArmedCountdown();
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
}

renderZonesUI();
updateDashboard();
renderCommandHistory();
syncControlAvailability();
initInfoIcons();
initializeFirebase();
