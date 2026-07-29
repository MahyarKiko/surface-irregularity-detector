"use strict";

/* 
   Configuration
  */

const DEMO_MODE = false;

const ESP32_WEBSOCKET_URL = "ws://192.168.4.1:81/";

const SERVO_1_ENABLED = true;
const SERVO_2_ENABLED = false;

const RECONNECT_DELAY_MS = 1500;
const COMMAND_TIMEOUT_MS = 4000;
const SLIDER_SEND_DELAY_MS = 180;

/* 
   DOM elements */

const connectionIndicator = document.getElementById("connectionIndicator");

const connectionText = document.getElementById("connectionText");

const servo1Slider = document.getElementById("servo1Slider");

const servo2Slider = document.getElementById("servo2Slider");

const servo1Value = document.getElementById("servo1Value");

const servo2Value = document.getElementById("servo2Value");

const servo1Status = document.getElementById("servo1Status");

const servo2Status = document.getElementById("servo2Status");

const patternButtons = document.querySelectorAll(".pattern-button");

const neutralButton = document.getElementById("neutralButton");

const stopButton = document.getElementById("stopButton");

const systemStateBadge = document.getElementById("systemStateBadge");

const statusMessage = document.getElementById("statusMessage");

const activePattern = document.getElementById("activePattern");

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

    setPatternControlsDisabled(false);

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

function handleSocketOpen() {
  console.log("[WEBSOCKET] Connected");

  appState.connecting = false;

  setConnectionState(true);

setPatternControlsDisabled(false);

  setSystemState("Ready", "WebSocket connected to ESP32");

  /*
   * Ask for the latest state.
   * The ESP32 also sends status automatically
   * immediately after connection.
   */
  sendCommand("STATUS").catch((error) => {
    console.warn("[STATUS REQUEST FAILED]", error);
  });
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

/* 
   Servo intensity */

async function sendServoIntensity(servoNumber, intensity) {
  const enabled = servoNumber === 1 ? SERVO_1_ENABLED : SERVO_2_ENABLED;

  if (!enabled) {
    setSystemState("Info", `Servo ${servoNumber} is not connected`);

    return;
  }

  if (!appState.connected) {
    setSystemState("Offline", "ESP32 is not connected");

    return;
  }

  if (appState.running || appState.busy) {
    return;
  }

  const safeIntensity = clampPercentage(intensity);

  try {
    const response = await sendCommand(`I${servoNumber}:${safeIntensity}`);

    if (response.success === false) {
      throw new Error(
        `ESP32 rejected intensity command for Servo ${servoNumber}`,
      );
    }

    setSystemState(
      "Ready",
      `Servo ${servoNumber} intensity set to ${safeIntensity}%`,
    );
  } catch (error) {
    handleCommunicationError(error);
  }
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

/* 
   Pattern execution */

/* 
   Pattern execution
*/

function runPattern(patternId) {
  if (!appState.connected) {
    setSystemState("Offline", "ESP32 is not connected");
    return;
  }

  const patternName = patternNames[patternId];

  if (!patternName) {
    setSystemState("Error", "Unknown pattern");
    return;
  }

  if (!socket || socket.readyState !== WebSocket.OPEN) {
    setSystemState("Offline", "WebSocket is not connected");
    return;
  }

  clearTimeout(servo1Timer);
  servo1Timer = null;

  const currentIntensity =
    clampPercentage(appState.servo1Intensity);

  appState.activePattern = patternId;

  activePattern.textContent = patternName;

  highlightPatternButton(patternId);

  setPatternControlsDisabled(false);

  socket.send(`I1:${currentIntensity}`);
  console.log(
    "[WEBSOCKET SENT]",
    `I1:${currentIntensity}`
  );

  socket.send(`P:${patternId}`);
  console.log(
    "[WEBSOCKET SENT]",
    `P:${patternId}`
  );

  setSystemState(
    "Running",
    `${patternName} command sent`
  );
}

patternButtons.forEach((button) => {
  button.addEventListener("click", () => {
    const patternId = Number(button.dataset.pattern);
    runPattern(patternId);
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
