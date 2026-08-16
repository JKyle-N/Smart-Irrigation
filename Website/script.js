/*
 * Dashboard contract
 * ------------------
 * irrigation/live     ESP1 -> dashboard (latest verified snapshot)
 * irrigation/config   dashboard-only zone profile notes (ESP1 does not read these)
 * irrigation/commands dashboard -> ESP1 (ESP1 validates every supported command)
 *
 * Commands ESP1 actually implements, and the exact payload each one expects:
 *   RUN_PUMP_TEST   { pump: "transfer" | "booster" | "mixer" }   5 s preventive exercise
 *   FORCE_RUN       { columns: "A"|"B"|"C"|"AB", liters: <=20, doseMl: { A, B, C } (each <=500) }
 *   EMERGENCY_STOP  {}
 * Anything else is rejected by ESP1 as "not a remotely safe control", so this page does not offer
 * it. In particular there is no standalone valve command: valves are sequenced inside a work order.
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

let activeZones = [
  { id: "A", name: "SOIL ZONE A (Precise Node 01)", defaultCrop: "talong", defaultStage: "vegetative" },
  { id: "B", name: "SOIL ZONE B (Precise Node 02)", defaultCrop: "pechay", defaultStage: "vegetative" },
  { id: "C", name: "SOIL ZONE C (Precise Node 03)", defaultCrop: "kamatis", defaultStage: "fruiting" }
];

let db = null;
let auth = null;
let liveData = {};
let commandData = [];
let firebaseReady = false;
let databaseListenersAttached = false;
let liveRef = null;
let commandsRef = null;
let lastCommandAt = 0;
const COMMAND_COOLDOWN_MS = 10000;

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

function setCommandStatus(text, tone = "") {
  const element = document.getElementById("commandStatus");
  if (!element) return;
  element.textContent = text;
  element.className = `command-status ${tone}`.trim();
}

function syncControlAvailability() {
  const signedIn = currentUserIsSignedIn();
  const normalAllowed = signedIn && deviceIsFresh();
  // Every actuating control needs a fresh snapshot: ESP1 will refuse anything that arrives while it
  // is not idle, and without live data the page cannot tell "idle" from "device offline".
  ["transferPumpBtn", "boosterPumpBtn", "mixerBtn"].forEach(id => {
    const button = document.getElementById(id);
    if (!button) return;
    button.disabled = !normalAllowed;
    button.title = normalAllowed ? "" : "Sign in and wait for a fresh ESP1 snapshot before starting a normal test.";
  });

  const forceButton = document.querySelector("#forceRunForm button[type=submit]");
  if (forceButton) {
    forceButton.disabled = !normalAllowed;
    forceButton.title = normalAllowed ? "" : "Sign in and wait for a fresh ESP1 snapshot before forcing a run.";
  }

  // Emergency stop is intentionally independent of live-data freshness and the normal cooldown.
  const emergency = document.getElementById("emergencyStop");
  if (emergency) {
    emergency.disabled = !signedIn;
    emergency.title = signedIn ? "" : "Sign in before sending an emergency-stop request.";
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
      if (!user) {
        detachDatabaseListeners();
        liveData = {};
        commandData = [];
        updateDashboard();
        renderCommandHistory();
        if (loginScreen) loginScreen.hidden = false;
        if (logoutButton) logoutButton.hidden = true;
        setConnection(false, "Sign in required");
        syncControlAvailability();
        return;
      }
      if (loginScreen) loginScreen.hidden = true;
      if (logoutButton) logoutButton.hidden = false;
      attachDatabaseListeners();
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
  }, error => setCommandStatus(`Command status unavailable: ${error.code || "unknown"}`, "error"));

  db.ref("irrigation/config/zones").once("value").then(snapshot => {
    const saved = snapshot.val();
    if (!saved) return;
    activeZones = ["A", "B", "C"].map(id => {
      const zone = saved[id] || {};
      const crop = cropDatabase[zone.crop] ? zone.crop : "pechay";
      const stage = cropDatabase[crop][zone.stage] ? zone.stage : Object.keys(cropDatabase[crop])[0];
      return { id, name: zone.name || `ZONE ${id}`, defaultCrop: crop, defaultStage: stage };
    });
    renderZonesUI();
    updateDashboard();
  }).catch(error => console.warn("Could not read dashboard zone profiles", error));
}

function detachDatabaseListeners() {
  if (liveRef) liveRef.off();
  if (commandsRef) commandsRef.off();
  liveRef = null;
  commandsRef = null;
  databaseListenersAttached = false;
}

function writeZoneProfile(zone) {
  if (!currentUserIsSignedIn()) return;
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
  setCommandStatus(emergency
    ? "Emergency stop queued. This is not physical-stop confirmation; wait for ESP1 status."
    : `${type.replaceAll("_", " ")} queued. Waiting for ESP1 validation.`, "pending");
  return db.ref("irrigation/commands").push(command)
    .then(reference => {
      if (!emergency) lastCommandAt = Date.now();
      return reference.key;
    })
    .catch(error => {
      if (!emergency) lastCommandAt = 0;
      setCommandStatus(`Could not queue command: ${error.message}`, "error");
      return false;
    });
}

function renderZonesUI() {
  const container = document.getElementById("dynamic-zones-container");
  if (!container) return;
  container.innerHTML = "";
  activeZones.forEach(zone => {
    const block = document.createElement("article");
    block.className = "zone-block";
    block.innerHTML = `
      <div class="zone-header">
        <div><p class="eyebrow">Physical zone ${zone.id}</p><h3>${escapeHtml(zone.name)}</h3></div>
        <div class="zone-selectors">
          <label>Crop<select id="cropSelect${zone.id}"></select></label>
          <label>Growth stage<select id="growthStage${zone.id}"></select></label>
        </div>
      </div>
      <div class="card-grid matrix-grid">
        <article class="card matrix-card"><h3>Nitrogen</h3><p id="nitrogen${zone.id}">Unavailable</p><small id="targetN${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Phosphorus</h3><p id="phosphorus${zone.id}">Unavailable</p><small id="targetP${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Potassium</h3><p id="potassium${zone.id}">Unavailable</p><small id="targetK${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil pH</h3><p id="soilPH${zone.id}">Unavailable</p><small id="targetPH${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil EC</h3><p id="soilEC${zone.id}">Unavailable</p><small id="targetEC${zone.id}">Target: --</small></article>
        <article class="card matrix-card"><h3>Soil moisture</h3><p id="soil${zone.id}">Unavailable</p><small id="targetMoisture${zone.id}">Target: --</small></article>
      </div>
      <p class="zone-note">Actuator/solenoid feedback: not reported by the current ESP1 Firebase snapshot.</p>`;
    container.appendChild(block);

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
    updateZoneTargets(zone);
  });
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
  });

  renderDiagnostics();
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
    ["Live snapshot", [["Snapshot", current ? "Current" : "Stale or unavailable"], ["Received", snapshotAgeText()], ["ESP1 state", liveData.system?.state || "Unavailable"]]],
    ["Network", [["Wi-Fi enabled", booleanText(d.network?.wifiEnabled, "Enabled", "Disabled")], ["Wi-Fi link", booleanText(d.network?.wifiConnected, "Connected", "Disconnected")], ["Wi-Fi RSSI", hasValue(d.network?.wifiRssi) ? `${d.network.wifiRssi} dBm` : "Unavailable"]]],
    ["Firebase", [["Enabled", booleanText(d.firebase?.enabled, "Enabled", "Disabled")], ["RTDB URL", booleanText(d.firebase?.urlConfigured, "Configured", "Not configured")], ["Device account", booleanText(d.firebase?.deviceCredentialsConfigured, "Configured", "Not configured")], ["Signed in", booleanText(d.firebase?.signedIn, "Yes", "No")], ["Last upload", Number(d.firebase?.attempts || 0) > 0 ? booleanText(d.firebase?.lastUploadOk, "OK", "Failed") : "Not attempted"], ["Last HTTP", Number(d.firebase?.attempts || 0) > 0 ? rawText(d.firebase?.lastHttp) : "Not attempted"], ["TLS validation", booleanText(d.firebase?.tlsValidationEnabled, "Enabled", "Disabled")], ["Last auth issue", rawText(d.firebase?.lastAuthError, "None")]]],
    ["ThingSpeak", [["Configured", booleanText(d.thingspeak?.configured, "Configured", "Not configured")], ["Last upload", d.thingspeak?.attempted ? booleanText(d.thingspeak?.lastUploadOk, "OK", "Failed") : "Not attempted"]]],
    ["Supabase logs", [["Configured", booleanText(d.supabase?.configured, "Configured", "Not configured")], ["Upload", booleanText(d.supabase?.uploadBusy, "In progress", "Idle")], ["Last uploaded day", Number(d.supabase?.lastUploadedDay || 0) > 0 ? rawText(d.supabase?.lastUploadedDay) : "No completed upload"]]],
    ["System", [["Work order", booleanText(d.system?.workOrderActive, "Active", "Inactive")], ["Pending run", booleanText(d.system?.pendingRun, "Yes", "No")], ["Last fault", rawText(d.system?.lastFault, "None")], ["Fault time", rawText(d.system?.lastFaultTime, "None")]]],
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
    detail.textContent = `${formatTimestamp(command.requestedAt)} — ${rawText(command.detail, "Awaiting ESP1 status")}`;
    row.append(top, detail);
    container.appendChild(row);
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

  const fertigation = doseMl.A > 0 || doseMl.B > 0 || doseMl.C > 0;
  show(`Queued ${fertigation ? "fertigation" : "irrigation"}: ${liters} L to ${columns}${fertigation ? ` with A/B/C ${doseMl.A}/${doseMl.B}/${doseMl.C} mL` : ""}. Waiting for ESP1 to accept it.`, false);
  queueCommand("FORCE_RUN", { columns, liters, doseMl });
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
    await auth.signInWithEmailAndPassword(document.getElementById("loginEmail").value.trim(), document.getElementById("loginPassword").value);
    loginForm.reset();
  } catch (error) {
    if (errorBox) errorBox.textContent = "Sign-in failed. Check your email and password.";
    console.error(error);
  }
});

document.getElementById("logoutBtn")?.addEventListener("click", () => auth?.signOut());
document.getElementById("transferPumpBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "transfer" }));
document.getElementById("boosterPumpBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "booster" }));
document.getElementById("mixerBtn")?.addEventListener("click", () => queueCommand("RUN_PUMP_TEST", { pump: "mixer" }));
document.getElementById("emergencyStop")?.addEventListener("click", () => queueCommand("EMERGENCY_STOP", {}, { emergency: true }));
document.getElementById("forceRunForm")?.addEventListener("submit", submitForceRun);

document.querySelectorAll(".tab").forEach(tab => tab.addEventListener("click", () => {
  document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
  document.querySelectorAll(".view").forEach(view => view.classList.toggle("active", view.id === tab.dataset.view));
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
}, 15000);

renderZonesUI();
updateDashboard();
renderCommandHistory();
syncControlAvailability();
initializeFirebase();
