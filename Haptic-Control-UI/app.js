"use strict";

/* 
   Configuration
  */

const DEMO_MODE = false;

const ESP32_WEBSOCKET_URL = "ws://192.168.4.1:81/";

const SERVO_1_ENABLED = true;
const SERVO_2_ENABLED = true;

const RECONNECT_DELAY_MS = 1500;
const COMMAND_TIMEOUT_MS = 4000;
const SLIDER_SEND_DELAY_MS = 180;

const SYNC_SAMPLE_COUNT = 10;
const SYNC_TIMEOUT_MS = 2000;

/* 
   DOM elements */

const connectionIndicator = document.getElementById("connectionIndicator");

const connectionText = document.getElementById("connectionText");

const servo1Slider = document.getElementById("servo1Slider");

const servo2Slider = document.getElementById("servo2Slider");

const servo1StartSlider = document.getElementById("servo1StartSlider");
const servo2StartSlider = document.getElementById("servo2StartSlider");

const servo1StartValue = document.getElementById("servo1StartValue");
const servo2StartValue = document.getElementById("servo2StartValue");

const servo1EnabledCheckbox = document.getElementById("servo1Enabled");

const servo2EnabledCheckbox = document.getElementById("servo2Enabled");

const servo1Value = document.getElementById("servo1Value");

const servo2Value = document.getElementById("servo2Value");

const servo1Status = document.getElementById("servo1Status");

const servo2Status = document.getElementById("servo2Status");

const patternButtons = document.querySelectorAll(".pattern-button");

const editPatternButton = document.getElementById("editPatternButton");
const patternEditor = document.getElementById("patternEditor");
const patternEditorSelect = document.getElementById("patternEditorSelect");

const patternSteps = document.getElementById("patternSteps");
const addPatternStepButton = document.getElementById("addPatternStepButton");
const resetPatternButton = document.getElementById("resetPatternButton");
const applyPatternButton = document.getElementById("applyPatternButton");

const neutralButton = document.getElementById("neutralButton");

const stopButton = document.getElementById("stopButton");

const systemStateBadge = document.getElementById("systemStateBadge");

const statusMessage = document.getElementById("statusMessage");

const activePattern = document.getElementById("activePattern");

const communicationLatency = document.getElementById("communicationLatency");

const internalLatency = document.getElementById("internalLatency");

const totalLatency = document.getElementById("totalLatency");

const syncLatency = document.getElementById("syncLatency");

/* 
   Pattern information */

const patternNames = {
  1: "Scratch",
  2: "Crack",
  3: "Dent",
  4: "Bump",
};

const patternIdsByName = {
  Scratch: 1,
  Crack: 2,
  Dent: 3,
  Bump: 4,
};

let currentEditorPatternNumber = Number(patternEditorSelect.value);

const patternEditorDrafts = {
  1: [], // Scratch
  2: [], // Crack
  3: [], // Dent
  4: [], // Bump
};

/* 
   Application state */

const appState = {
  connected: false,
  connecting: false,

  running: false,
  activePattern: null,

  /*
   * busy only means that a command is currently
   * waiting for acknowledgement.
   */
  busy: false,

  servo1Intensity: Number(servo1Slider.value),

  servo2Intensity: Number(servo2Slider.value),

  servo1StartAngle: Number(servo1StartSlider.value),
  servo2StartAngle: Number(servo2StartSlider.value),
};

/* 
   WebSocket state */

let socket = null;

let reconnectTimer = null;

let servo1Timer = null;
let servo2Timer = null;

/*
 * Only one command is sent at a time.
 * This avoids mixing command responses.
 */
let commandSequence = Promise.resolve();

let pendingCommand = null;

let pendingPing = null;

let clockOffsetUs = null;
let bestSyncRttUs = null;

let webCommandSequence = 1;
let activeWebMeasurement = null;

const webLatencyResults = [];

/* 
   General utilities*/

function wait(milliseconds) {
  return new Promise((resolve) => {
    setTimeout(resolve, milliseconds);
  });
}

function clampPercentage(value) {
  const numericValue = Number(value);

  if (!Number.isFinite(numericValue)) {
    return 0;
  }

  return Math.max(0, Math.min(100, Math.round(numericValue)));
}

function getErrorMessage(error) {
  if (error instanceof Error) {
    return error.message;
  }

  return String(error);
}

/* 
   UI helper functions*/

function setConnectionState(isConnected) {
  appState.connected = isConnected;

  connectionIndicator.classList.toggle("status-connected", isConnected);

  connectionIndicator.classList.toggle("status-disconnected", !isConnected);

  connectionText.textContent = isConnected ? "Connected" : "Disconnected";
}

function setSystemState(state, message) {
  systemStateBadge.textContent = state;

  systemStateBadge.dataset.state = state.toLowerCase();

  statusMessage.textContent = message;
}

function setPatternControlsDisabled(disabled) {
  patternButtons.forEach((button) => {
    button.disabled = disabled;
  });

  neutralButton.disabled = disabled;

  /*
   * Emergency Stop should remain available
   * whenever the WebSocket is connected.
   */
  stopButton.disabled = !appState.connected;
}

function clearActivePatternButton() {
  patternButtons.forEach((button) => {
    button.classList.remove("active");
  });
}

function highlightPatternButton(patternId) {
  clearActivePatternButton();

  const selectedButton = document.querySelector(
    `.pattern-button[data-pattern="${patternId}"]`,
  );

  if (selectedButton) {
    selectedButton.classList.add("active");
  }
}

function updateServoDisplay(servoNumber, value) {
  const safeValue = clampPercentage(value);

  if (servoNumber === 1) {
    appState.servo1Intensity = safeValue;

    servo1Slider.value = safeValue;

    servo1Value.textContent = `${safeValue}%`;

    servo1Status.textContent = `${safeValue}%`;
  }

  if (servoNumber === 2) {
    appState.servo2Intensity = safeValue;

    servo2Slider.value = safeValue;

    servo2Value.textContent = `${safeValue}%`;

    servo2Status.textContent = `${safeValue}%`;
  }
}

function updateStartPositionDisplay(servoNumber, angle) {
  const safeAngle = Math.max(35, Math.min(170, Math.round(Number(angle))));

  if (servoNumber === 1) {
    appState.servo1StartAngle = safeAngle;
    servo1StartSlider.value = safeAngle;
    servo1StartValue.textContent = `${safeAngle}°`;
  }

  if (servoNumber === 2) {
    appState.servo2StartAngle = safeAngle;
    servo2StartSlider.value = safeAngle;
    servo2StartValue.textContent = `${safeAngle}°`;
  }
}

function sendServoStartPosition(servoNumber, angle) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  if (appState.running) {
    return;
  }

  const safeAngle = Math.max(35, Math.min(170, Math.round(Number(angle))));

  socket.send(`S${servoNumber}:${safeAngle}`);

  console.log("[WEBSOCKET SENT]", `S${servoNumber}:${safeAngle}`);

  setSystemState(
    "Ready",
    `Servo ${servoNumber} start position set to ${safeAngle}°`,
  );
}

function configureServoAvailability() {
  servo1Slider.disabled = !SERVO_1_ENABLED;

  servo2Slider.disabled = !SERVO_2_ENABLED;

  if (!SERVO_1_ENABLED) {
    servo1Value.textContent = "Disabled";

    servo1Status.textContent = "Not configured";
  }

  if (!SERVO_2_ENABLED) {
    servo2Value.textContent = "Planned";

    servo2Status.textContent = "Not connected";

    servo2Slider.title = "Servo 2 is not connected yet";
  }
}

function showDisconnectedUi() {
  setConnectionState(false);

  appState.running = false;
  appState.activePattern = null;
  appState.busy = false;

  activePattern.textContent = "None";

  clearActivePatternButton();

  setPatternControlsDisabled(false);

  setSystemState("Offline", "Connecting to ESP32...");
}

/*
   Status processing */

function applyStatus(status) {
  const wasRunning = appState.running;

  if (typeof status.servo1Intensity === "number") {
    updateServoDisplay(1, status.servo1Intensity);
  }

  if (SERVO_2_ENABLED && typeof status.servo2Intensity === "number") {
    updateServoDisplay(2, status.servo2Intensity);
  }

  appState.running = Boolean(status.patternRunning);

  const receivedPatternName = status.activePattern || "None";

  if (appState.running) {
    const patternId = patternIdsByName[receivedPatternName] || null;

    appState.activePattern = patternId;

    activePattern.textContent = receivedPatternName;

    if (patternId !== null) {
      highlightPatternButton(patternId);
    }

    setPatternControlsDisabled(true);

    setSystemState("Running", `${receivedPatternName} pattern is running`);

    return;
  }

  appState.activePattern = null;

  activePattern.textContent = "None";

  clearActivePatternButton();

  setPatternControlsDisabled(false);

  if (wasRunning) {
    setSystemState("Ready", "Pattern completed successfully");
  } else {
    setSystemState("Ready", "ESP32 is connected");
  }
}

/* 
   Pending command handling */

function rejectPendingCommand(error) {
  if (!pendingCommand) {
    return;
  }

  clearTimeout(pendingCommand.timeoutTimer);

  const reject = pendingCommand.reject;

  pendingCommand = null;

  reject(error);
}

function resolvePendingCommand(response) {
  if (!pendingCommand) {
    console.warn(
      "[WEBSOCKET] Command response received without a pending command",
      response,
    );

    return;
  }

  clearTimeout(pendingCommand.timeoutTimer);

  const resolve = pendingCommand.resolve;

  pendingCommand = null;

  resolve(response);
}

function computerTimeUs() {
  return performance.now() * 1000;
}

function handlePongMessage(rawMessage) {
  const parts = rawMessage.split(":");

  if (parts.length !== 4 || parts[0] !== "PONG") {
    return false;
  }

  if (!pendingPing || pendingPing.id !== parts[1]) {
    console.warn("[SYNC] Unexpected PONG", rawMessage);

    return true;
  }

  const computerReceiveUs = computerTimeUs();

  const espReceiveUs = Number(parts[2]);
  const espSendUs = Number(parts[3]);

  if (!Number.isFinite(espReceiveUs) || !Number.isFinite(espSendUs)) {
    pendingPing.reject(new Error("Invalid ESP32 timestamp"));

    pendingPing = null;
    return true;
  }

  clearTimeout(pendingPing.timeoutTimer);

  const roundTripUs =
    computerReceiveUs - pendingPing.computerSendUs - (espSendUs - espReceiveUs);

  const clockOffset =
    (espReceiveUs -
      pendingPing.computerSendUs +
      (espSendUs - computerReceiveUs)) /
    2;

  const resolve = pendingPing.resolve;

  pendingPing = null;

  resolve({
    roundTripUs,
    clockOffsetUs: clockOffset,
  });

  return true;
}

function sendSyncPing(sampleNumber) {
  return new Promise((resolve, reject) => {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      reject(new Error("WebSocket is not connected"));
      return;
    }

    const id = `WS${sampleNumber}`;

    const timeoutTimer = setTimeout(() => {
      pendingPing = null;

      reject(new Error(`Sync timeout: ${id}`));
    }, SYNC_TIMEOUT_MS);

    pendingPing = {
      id,
      computerSendUs: computerTimeUs(),
      resolve,
      reject,
      timeoutTimer,
    };

    socket.send(`PING:${id}`);
  });
}

async function synchronizeWebSocketClocks() {
  const samples = [];

  syncLatency.textContent = "Synchronizing...";

  for (
    let sampleNumber = 1;
    sampleNumber <= SYNC_SAMPLE_COUNT;
    sampleNumber++
  ) {
    const sample = await sendSyncPing(sampleNumber);

    samples.push(sample);

    console.log(
      `[SYNC ${sampleNumber}] RTT: ` +
        `${(sample.roundTripUs / 1000).toFixed(3)} ms`,
    );

    await wait(100);
  }

  const bestSample = samples.reduce((best, sample) =>
    sample.roundTripUs < best.roundTripUs ? sample : best,
  );

  clockOffsetUs = bestSample.clockOffsetUs;
  bestSyncRttUs = bestSample.roundTripUs;

  syncLatency.textContent = `${(bestSyncRttUs / 1000).toFixed(3)} ms`;

  console.log(
    "[SYNC] Ready. Best RTT:",
    `${(bestSyncRttUs / 1000).toFixed(3)} ms`,
  );
}

function handleLatencyResult(message) {
  if (!activeWebMeasurement) {
    console.warn("[LATENCY] Result without active measurement", message);
    return;
  }

  if (message.id !== activeWebMeasurement.commandId) {
    console.warn("[LATENCY] Command ID does not match", message);
    return;
  }

  if (clockOffsetUs === null) {
    console.error("[LATENCY] Clocks are not synchronized");
    return;
  }

  const espReceivedUs = Number(message.receivedUs);

  const firstPwmUs = Number(message.firstPwmUs);

  const internalLatencyUs = Number(message.internalLatencyUs);

  if (
    !Number.isFinite(espReceivedUs) ||
    !Number.isFinite(firstPwmUs) ||
    !Number.isFinite(internalLatencyUs)
  ) {
    console.error("[LATENCY] Invalid result", message);
    return;
  }

  const receivedOnBrowserClockUs = espReceivedUs - clockOffsetUs;

  const pwmOnBrowserClockUs = firstPwmUs - clockOffsetUs;

  const communicationUs =
    receivedOnBrowserClockUs - activeWebMeasurement.userActionUs;

  const totalUs = pwmOnBrowserClockUs - activeWebMeasurement.userActionUs;

  const result = {
    recordedAt: new Date().toISOString(),
    commandId: activeWebMeasurement.commandId,
    source: "WEBSOCKET",
    patternId: activeWebMeasurement.patternId,
    patternName: activeWebMeasurement.patternName,
    servo1Intensity: activeWebMeasurement.servo1Intensity,
    servo2Intensity: activeWebMeasurement.servo2Intensity,
    communicationMs: communicationUs / 1000,
    internalMs: internalLatencyUs / 1000,
    totalMs: totalUs / 1000,
    syncRttMs: bestSyncRttUs / 1000,
  };

  webLatencyResults.push(result);

  localStorage.setItem(
    "hapticWebSocketLatencyResults",
    JSON.stringify(webLatencyResults),
  );

  communicationLatency.textContent = `${result.communicationMs.toFixed(3)} ms`;

  internalLatency.textContent = `${result.internalMs.toFixed(3)} ms`;

  totalLatency.textContent = `${result.totalMs.toFixed(3)} ms`;

  syncLatency.textContent = `${result.syncRttMs.toFixed(3)} ms`;

  console.log("[LATENCY RESULT]", result);
  console.table(result);

  const serialResultMessage = [
    "WEB_RESULT",
    result.commandId,
    result.patternName,
    result.servo1Intensity,
    result.servo2Intensity,
    result.communicationMs.toFixed(3),
    result.internalMs.toFixed(3),
    result.totalMs.toFixed(3),
    result.syncRttMs.toFixed(3),
  ].join(":");

  socket.send(serialResultMessage);

  setSystemState(
    "Measured",
    `${result.patternName}: ` + `${result.totalMs.toFixed(3)} ms total latency`,
  );

  activeWebMeasurement = null;
}

/* 
   WebSocket connection */

function scheduleReconnect() {
  if (DEMO_MODE || reconnectTimer !== null) {
    return;
  }

  console.log(`[WEBSOCKET] Reconnecting in ${RECONNECT_DELAY_MS} ms`);

  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;

    connectWebSocket();
  }, RECONNECT_DELAY_MS);
}

function connectWebSocket() {
  if (DEMO_MODE) {
    return;
  }

  if (
    socket &&
    (socket.readyState === WebSocket.OPEN ||
      socket.readyState === WebSocket.CONNECTING)
  ) {
    return;
  }

  appState.connecting = true;

  setConnectionState(false);

  setSystemState("Connecting", "Connecting to ESP32...");

  console.log("[WEBSOCKET] Connecting:", ESP32_WEBSOCKET_URL);

  try {
    socket = new WebSocket(ESP32_WEBSOCKET_URL);
  } catch (error) {
    console.error("[WEBSOCKET] Creation failed", error);

    appState.connecting = false;

    scheduleReconnect();

    return;
  }

  socket.addEventListener("open", handleSocketOpen);

  socket.addEventListener("message", handleSocketMessage);

  socket.addEventListener("close", handleSocketClose);

  socket.addEventListener("error", handleSocketError);
}
async function handleSocketOpen() {
  console.log("[WEBSOCKET] Connected");

  appState.connecting = false;

  setConnectionState(true);
  setPatternControlsDisabled(true);

  setSystemState("Synchronizing", "Synchronizing clocks with ESP32...");

  try {
    await synchronizeWebSocketClocks();

    sendCurrentServoConfiguration();

    // Give the ESP32 time to process configuration.
    await wait(100);

    socket.send("STATUS");

    setPatternControlsDisabled(false);

    setSystemState("Ready", "WebSocket synchronized with ESP32");
  } catch (error) {
    console.error("[SYNC ERROR]", error);

    clockOffsetUs = null;
    bestSyncRttUs = null;

    syncLatency.textContent = "Synchronization failed";

    setPatternControlsDisabled(true);

    setSystemState("Error", "Clock synchronization failed");
  }
}

function handleSocketClose(event) {
  console.warn("[WEBSOCKET] Closed", {
    code: event.code,
    reason: event.reason,
    wasClean: event.wasClean,
  });

  appState.connecting = false;

  rejectPendingCommand(new Error("WebSocket connection was closed"));

  showDisconnectedUi();

  scheduleReconnect();
}

function handleSocketError(event) {
  console.error("[WEBSOCKET] Error", event);

  /*
   * The close event normally follows this event
   * and performs the reconnect.
   */
  setSystemState("Error", "WebSocket communication error");
}

function handleSocketMessage(event) {
  console.log("[WEBSOCKET RECEIVED]", event.data);

  if (typeof event.data === "string" && event.data.startsWith("PONG:")) {
    handlePongMessage(event.data);
    return;
  }

  let message;

  try {
    message = JSON.parse(event.data);
  } catch (error) {
    console.error("[WEBSOCKET] Invalid JSON", event.data, error);

    return;
  }

  /*
   * Every valid message confirms that the ESP32
   * connection is alive.
   */
  setConnectionState(true);

  if (message.type === "status") {
    applyStatus(message);
    return;
  }

  if (message.type === "latencyResult") {
    handleLatencyResult(message);
    return;
  }

  if (message.type === "commandResult") {
    resolvePendingCommand(message);

    /*
     * Also apply fields included in the
     * command response.
     */
    applyStatus(message);

    return;
  }

  console.warn("[WEBSOCKET] Unknown message type", message);
}

/* =========================================================
   WebSocket command sending
   ========================================================= */

function sendCommandNow(command) {
  console.log("[COMMAND]", command);

  if (DEMO_MODE) {
    return wait(100).then(() => ({
      type: "commandResult",
      success: true,
      command,
      patternRunning: appState.running,

      activePattern: appState.activePattern
        ? patternNames[appState.activePattern]
        : "None",

      servo1Intensity: appState.servo1Intensity,
    }));
  }

  return new Promise((resolve, reject) => {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      reject(new Error("WebSocket is not connected"));

      return;
    }

    if (pendingCommand) {
      reject(new Error("Another command is still pending"));

      return;
    }

    const timeoutTimer = setTimeout(() => {
      if (!pendingCommand || pendingCommand.command !== command) {
        return;
      }

      pendingCommand = null;

      reject(new Error(`Command timeout: ${command}`));
    }, COMMAND_TIMEOUT_MS);

    pendingCommand = {
      command,
      resolve,
      reject,
      timeoutTimer,
    };

    try {
      socket.send(command);

      console.log("[WEBSOCKET SENT]", command);
    } catch (error) {
      clearTimeout(timeoutTimer);

      pendingCommand = null;

      reject(error);
    }
  });
}

function sendCommand(command) {
  const operation = () => sendCommandNow(command);

  const result = commandSequence.then(operation, operation);

  /*
   * Keep the queue alive after a failed command.
   */
  commandSequence = result.catch(() => undefined);

  return result;
}

function sendCurrentServoConfiguration() {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    return;
  }

  const servo1Selected = servo1EnabledCheckbox.checked;

  const servo2Selected = servo2EnabledCheckbox.checked;

  const servo1Intensity = clampPercentage(appState.servo1Intensity);

  const servo2Intensity = clampPercentage(appState.servo2Intensity);

  socket.send(`E1:${servo1Selected ? 1 : 0}`);

  socket.send(`E2:${servo2Selected ? 1 : 0}`);

  socket.send(`I1:${servo1Intensity}`);

  socket.send(`I2:${servo2Intensity}`);

  socket.send(`S1:${appState.servo1StartAngle}`);
  socket.send(`S2:${appState.servo2StartAngle}`);

  console.log("[SERVO CONFIGURATION SENT]", {
    servo1Selected,
    servo2Selected,
    servo1Intensity,
    servo2Intensity,
    servo1StartAngle: appState.servo1StartAngle,
    servo2StartAngle: appState.servo2StartAngle,
  });
}

/* 
   Servo intensity */

function sendServoIntensity(servoNumber, intensity) {
  const enabled = servoNumber === 1 ? SERVO_1_ENABLED : SERVO_2_ENABLED;

  if (!enabled) {
    setSystemState("Info", `Servo ${servoNumber} is not connected`);
    return;
  }

  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  if (appState.running) {
    return;
  }

  const safeIntensity = clampPercentage(intensity);

  socket.send(`I${servoNumber}:${safeIntensity}`);

  console.log("[WEBSOCKET SENT]", `I${servoNumber}:${safeIntensity}`);

  setSystemState(
    "Ready",
    `Servo ${servoNumber} intensity set to ` + `${safeIntensity}%`,
  );
}

function scheduleServoUpdate(servoNumber, intensity) {
  if (servoNumber === 1 && SERVO_1_ENABLED) {
    clearTimeout(servo1Timer);

    servo1Timer = setTimeout(() => {
      sendServoIntensity(1, intensity);
    }, SLIDER_SEND_DELAY_MS);
  }

  if (servoNumber === 2 && SERVO_2_ENABLED) {
    clearTimeout(servo2Timer);

    servo2Timer = setTimeout(() => {
      sendServoIntensity(2, intensity);
    }, SLIDER_SEND_DELAY_MS);
  }
}

servo1Slider.addEventListener("input", (event) => {
  const intensity = clampPercentage(event.target.value);

  updateServoDisplay(1, intensity);

  scheduleServoUpdate(1, intensity);
});

servo2Slider.addEventListener("input", (event) => {
  if (!SERVO_2_ENABLED) {
    return;
  }

  const intensity = clampPercentage(event.target.value);

  updateServoDisplay(2, intensity);

  scheduleServoUpdate(2, intensity);
});

servo1StartSlider.addEventListener("input", (event) => {
  const angle = Number(event.target.value);

  updateStartPositionDisplay(1, angle);

  sendServoStartPosition(1, angle);
});

servo2StartSlider.addEventListener("input", (event) => {
  const angle = Number(event.target.value);

  updateStartPositionDisplay(2, angle);

  sendServoStartPosition(2, angle);
});

servo1EnabledCheckbox.addEventListener("change", () => {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(`E1:${servo1EnabledCheckbox.checked ? 1 : 0}`);
  }
});

servo2EnabledCheckbox.addEventListener("change", () => {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(`E2:${servo2EnabledCheckbox.checked ? 1 : 0}`);
  }
});

editPatternButton.addEventListener("click", () => {
  patternEditor.hidden = !patternEditor.hidden;

  editPatternButton.textContent = patternEditor.hidden
    ? "Edit Pattern"
    : "Close Pattern Editor";
});

function addPatternStep(
  servo1Position = 0,
  servo2Position = 0,
  duration = 400,
) {
  const stepNumber = patternSteps.children.length + 1;

  const step = document.createElement("div");
  step.className = "pattern-step";

  step.innerHTML = `
    <label>
      Servo 1 Position
      <input
        type="number"
        class="step-servo1"
        min="-100"
        max="100"
        value="${servo1Position}"
      >
    </label>

    <label>
      Servo 2 Position
      <input
        type="number"
        class="step-servo2"
        min="-100"
        max="100"
        value="${servo2Position}"
      >
    </label>

    <label>
      Duration (ms)
      <input
        type="number"
        class="step-duration"
        min="1"
        value="${duration}"
      >
    </label>

    <button
  type="button"
  class="remove-step-button"
  aria-label="Remove step"
  title="Remove step"
>
  <svg
    viewBox="0 0 24 24"
    fill="none"
    xmlns="http://www.w3.org/2000/svg"
    aria-hidden="true"
  >
    <path
      d="M3 6H21"
      stroke-width="2"
      stroke-linecap="round"
    />
    <path
      d="M8 6V4C8 3.44772 8.44772 3 9 3H15C15.5523 3 16 3.44772 16 4V6"
      stroke-width="2"
      stroke-linecap="round"
    />
    <path
      d="M19 6L18.1333 18.142C18.0584 19.1893 17.1871 20 16.1371 20H7.86294C6.8129 20 5.94164 19.1893 5.86667 18.142L5 6"
      stroke-width="2"
      stroke-linecap="round"
      stroke-linejoin="round"
    />
    <path
      d="M10 11V16"
      stroke-width="2"
      stroke-linecap="round"
    />
    <path
      d="M14 11V16"
      stroke-width="2"
      stroke-linecap="round"
    />
  </svg>
</button>
  `;

  step.dataset.step = stepNumber;

  patternSteps.appendChild(step);
}

function readPatternEditorSteps() {
  const steps = patternSteps.querySelectorAll(".pattern-step");

  return Array.from(steps).map((step) => ({
    servo1Position: Number(step.querySelector(".step-servo1").value),
    servo2Position: Number(step.querySelector(".step-servo2").value),
    duration: Number(step.querySelector(".step-duration").value),
  }));
}

function saveCurrentPatternDraft() {
  patternEditorDrafts[currentEditorPatternNumber] = readPatternEditorSteps();
}

function showPatternDraft(patternNumber) {
  patternSteps.innerHTML = "";

  const steps = patternEditorDrafts[patternNumber];

  for (const step of steps) {
    addPatternStep(step.servo1Position, step.servo2Position, step.duration);
  }
}

addPatternStepButton.addEventListener("click", () => {
  addPatternStep();
});

patternSteps.addEventListener("click", (event) => {
  const removeButton = event.target.closest(".remove-step-button");

  if (!removeButton) {
    return;
  }

  const step = removeButton.closest(".pattern-step");

  if (step) {
    step.remove();
  }
});

patternEditorSelect.addEventListener("change", (event) => {
  saveCurrentPatternDraft();

  currentEditorPatternNumber = Number(event.target.value);

  showPatternDraft(currentEditorPatternNumber);
});

resetPatternButton.addEventListener("click", () => {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  if (appState.running) {
    return;
  }

  patternEditorDrafts[currentEditorPatternNumber] = [];

  showPatternDraft(currentEditorPatternNumber);

  const command = `RESET_PATTERN:${currentEditorPatternNumber}`;

  socket.send(command);

  console.log("[PATTERN RESET]", command);

  setSystemState(
    "Ready",
    `${patternNames[currentEditorPatternNumber]} reset to default`,
  );
});

applyPatternButton.addEventListener("click", () => {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  if (appState.running) {
    return;
  }

  const patternNumber = Number(patternEditorSelect.value);

  const steps = patternSteps.querySelectorAll(".pattern-step");

  if (steps.length === 0) {
    setSystemState("Ready", "Add at least one pattern step");
    return;
  }

  const patternData = [];

  for (const step of steps) {
    const servo1Position = Number(step.querySelector(".step-servo1").value);

    const servo2Position = Number(step.querySelector(".step-servo2").value);

    const duration = Number(step.querySelector(".step-duration").value);

    if (
      servo1Position < -100 ||
      servo1Position > 100 ||
      servo2Position < -100 ||
      servo2Position > 100 ||
      duration < 1
    ) {
      setSystemState("Ready", "Invalid pattern values");
      return;
    }

    patternData.push(`${servo1Position},${servo2Position},${duration}`);
  }

  const command = `SET_PATTERN:${patternNumber}:${patternData.join(";")}`;

  socket.send(command);

  console.log("[PATTERN SENT]", command);

  setSystemState("Ready", `Pattern ${patternNumber} updated`);
});

/* 
   Pattern execution
*/

function runPattern(patternId, userActionUs) {
  if (!appState.connected) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "WebSocket is not connected");
    return;
  }

  if (clockOffsetUs === null) {
    setSystemState("Error", "Clocks are not synchronized");
    return;
  }

  if (activeWebMeasurement !== null) {
    setSystemState("Busy", "Another measurement is active");
    return;
  }

  const patternName = patternNames[patternId];

  if (!patternName) {
    setSystemState("Error", "Unknown pattern");
    return;
  }

  const servo1Selected = servo1EnabledCheckbox.checked;

  const servo2Selected = servo2EnabledCheckbox.checked;

  if (!servo1Selected && !servo2Selected) {
    setSystemState("Error", "Select at least one servo");
    return;
  }

  const commandId = `W${webCommandSequence}`;

  webCommandSequence += 1;

  activeWebMeasurement = {
    commandId,
    patternId,
    patternName,
    userActionUs,
    servo1Intensity: clampPercentage(appState.servo1Intensity),
    servo2Intensity: clampPercentage(appState.servo2Intensity),
  };

  appState.activePattern = patternId;

  activePattern.textContent = patternName;

  highlightPatternButton(patternId);

  setPatternControlsDisabled(true);

  const command = `RUN:${commandId}:${patternId}`;

  socket.send(command);

  console.log("[WEBSOCKET SENT]", command);

  setSystemState("Running", `${patternName} measurement started`);
}

patternButtons.forEach((button) => {
  button.addEventListener("click", () => {
    // T0: the pattern button was activated
    // by mouse, touch, keyboard or trackpad.
    const userActionUs = computerTimeUs();

    const patternId = Number(button.dataset.pattern);

    runPattern(patternId, userActionUs);
  });
});

/* 
   Neutral
*/

neutralButton.addEventListener("click", async () => {
  if (!appState.connected) {
    setSystemState("Offline", "ESP32 is not connected");

    return;
  }

  if (appState.busy) {
    return;
  }

  appState.busy = true;

  setSystemState("Moving", "Moving servo to neutral position");

  try {
    const response = await sendCommand("P:0");

    if (response.success === false) {
      throw new Error("ESP32 rejected the neutral command");
    }

    setSystemState("Ready", "Servo is in neutral position");
  } catch (error) {
    handleCommunicationError(error);
  } finally {
    appState.busy = false;
  }
});

/* 
   Emergency stop */

stopButton.addEventListener("click", async () => {
  if (!appState.connected) {
    setSystemState("Offline", "ESP32 is not connected");

    return;
  }

  /*
   * Update the interface immediately.
   */
  appState.running = false;
  appState.activePattern = null;

  activePattern.textContent = "None";

  clearActivePatternButton();

  setPatternControlsDisabled(false);

  setSystemState("Stopped", "Emergency stop activated");

  try {
    const response = await sendCommand("STOP");

    if (response.success === false) {
      throw new Error("ESP32 rejected the stop command");
    }

    setSystemState("Stopped", "Emergency stop activated");
  } catch (error) {
    handleCommunicationError(error);
  } finally {
    appState.busy = false;
  }
});

/* 
   Error handling */

function handleCommunicationError(error) {
  console.error("[COMMUNICATION ERROR]", error);

  const message = getErrorMessage(error);

  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setConnectionState(false);
  }

  setSystemState("Error", message || "Communication with ESP32 failed");
}

/* 
   Initialisation*/

async function initialiseApplication() {
  updateServoDisplay(1, servo1Slider.value);

  updateServoDisplay(2, servo2Slider.value);

  updateStartPositionDisplay(1, servo1StartSlider.value);
  updateStartPositionDisplay(2, servo2StartSlider.value);

  configureServoAvailability();

  activePattern.textContent = "None";

  clearActivePatternButton();

  if (DEMO_MODE) {
    setConnectionState(true);

    setPatternControlsDisabled(false);

    setSystemState("Ready", "Demo mode is active");

    return;
  }

  showDisconnectedUi();

  connectWebSocket();
}

document.addEventListener("DOMContentLoaded", initialiseApplication);

/*  Page lifecycle */

window.addEventListener("beforeunload", () => {
  clearTimeout(reconnectTimer);
  clearTimeout(servo1Timer);
  clearTimeout(servo2Timer);

  if (socket) {
    socket.close();
  }
});
