#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESP32Servo.h>

// ============================================================
// Haptic Defect Prototype
// ESP32 + One Servo + Serial + HTTP + WebSocket
//
// WebSocket address:
//   ws://192.168.4.1:81/
//
// Commands:
//   P:1   = Scratch
//   P:2   = Crack
//   P:3   = Dent
//   P:4   = Bump
//   P:0   = Neutral
//   STOP  = Emergency stop
//   I1:75 = Servo 1 intensity 75%
// ============================================================


// ============================================================
// Wi-Fi and servers
// ============================================================

const char* WIFI_NAME = "aptic-Control";
const char* WIFI_PASSWORD = "";

WebServer server(80);
WebSocketsServer webSocket(81);


// ============================================================
// Servo configuration
// ============================================================

Servo hapticServo;

constexpr uint8_t SERVO_PIN = 21;

constexpr int SERVO_MIN_ANGLE = 35;
constexpr int SERVO_NEUTRAL_ANGLE = 90;
constexpr int SERVO_MAX_ANGLE = 145;

constexpr uint16_t SERVO_MIN_PULSE_US = 500;
constexpr uint16_t SERVO_MAX_PULSE_US = 2400;
constexpr uint16_t SERVO_FREQUENCY_HZ = 50;

constexpr uint32_t SERIAL_BAUD_RATE = 115200;

unsigned long patternCommandReceivedUs = 0;
bool latencyMeasurementActive = false;

String latencySource = "";

// ============================================================
// Pattern definition
// ============================================================

struct HapticStep {
  int8_t relativePosition;
  uint16_t durationMs;
};

enum class DefectType : uint8_t {
  None,
  Scratch,
  Crack,
  Dent,
  Bump
};


// ============================================================
// Haptic patterns
// ============================================================

// 1: Scratch
const HapticStep PATTERN_1[] = {
  { 0, 35 },
  { 18, 30 },
  { -8, 25 },
  { 20, 30 },
  { -6, 25 },
  { 16, 30 },
  { -10, 25 },
  { 22, 30 },
  { -8, 25 },
  { 18, 30 },
  { -5, 25 },
  { 20, 30 },
  { 0, 120 }
};

// 2: Crack
const HapticStep PATTERN_2[] = {
  { 0, 100 },
  { 90, 90 },
  { 0, 160 },
  { 90, 90 },
  { 0, 160 },
  { 90, 90 },
  { 0, 200 }
};

// 3: Dent
const HapticStep PATTERN_3[] = {
  { 0, 120 },
  { -20, 120 },
  { -40, 120 },
  { -60, 120 },
  { -85, 400 },
  { -60, 120 },
  { -40, 120 },
  { -20, 120 },
  { 0, 220 }
};

// 4: Bump
const HapticStep PATTERN_4[] = {
  { 0, 120 },
  { 35, 80 },
  { 70, 80 },
  { 90, 300 },
  { 45, 100 },
  { 0, 220 }
};


// ============================================================
// Runtime state
// ============================================================

const HapticStep* activePattern = nullptr;

size_t activePatternLength = 0;
size_t currentStepIndex = 0;

DefectType activeDefect = DefectType::None;

uint32_t stepStartedAtMs = 0;

bool patternRunning = false;

uint8_t servo1IntensityPercent = 75;

String serialInputBuffer;


// ============================================================
// Forward declarations
// ============================================================

void broadcastStatus();
void sendStatusToClient(uint8_t clientNumber);


// ============================================================
// Utility functions
// ============================================================

const char* defectTypeToText(DefectType defect) {
  switch (defect) {
    case DefectType::Scratch:
      return "Scratch";

    case DefectType::Crack:
      return "Crack";

    case DefectType::Dent:
      return "Dent";

    case DefectType::Bump:
      return "Bump";

    default:
      return "None";
  }
}


String createStatusJson() {
  String json = "{";

  json += "\"type\":\"status\",";
  json += "\"connected\":true,";

  json += "\"patternRunning\":";
  json += patternRunning ? "true" : "false";
  json += ",";

  json += "\"activePattern\":\"";
  json += defectTypeToText(activeDefect);
  json += "\",";

  json += "\"servo1Intensity\":";
  json += servo1IntensityPercent;

  json += "}";

  return json;
}


String createCommandResultJson(
  bool success,
  const String& command) {
  String json = "{";

  json += "\"type\":\"commandResult\",";

  json += "\"success\":";
  json += success ? "true" : "false";
  json += ",";

  json += "\"command\":\"";
  json += command;
  json += "\",";

  json += "\"patternRunning\":";
  json += patternRunning ? "true" : "false";
  json += ",";

  json += "\"activePattern\":\"";
  json += defectTypeToText(activeDefect);
  json += "\",";

  json += "\"servo1Intensity\":";
  json += servo1IntensityPercent;

  json += "}";

  return json;
}


// ============================================================
// WebSocket messages
// ============================================================

void sendStatusToClient(uint8_t clientNumber) {
  String json = createStatusJson();

  webSocket.sendTXT(
    clientNumber,
    json);
}


void broadcastStatus() {
  String json = createStatusJson();

  webSocket.broadcastTXT(json);
}


// ============================================================
// Servo control
// ============================================================

int relativePositionToAngle(
  int8_t relativePosition,
  uint8_t intensityPercent) {
  int availableRange;

  if (relativePosition < 0) {
    availableRange =
      SERVO_NEUTRAL_ANGLE - SERVO_MIN_ANGLE;
  } else {
    availableRange =
      SERVO_MAX_ANGLE - SERVO_NEUTRAL_ANGLE;
  }

  int offset =
    (availableRange * relativePosition) / 100;

  offset =
    (offset * intensityPercent) / 100;

  return constrain(
    SERVO_NEUTRAL_ANGLE + offset,
    SERVO_MIN_ANGLE,
    SERVO_MAX_ANGLE);
}


void moveServo(int8_t relativePosition) {
  const int targetAngle =
    relativePositionToAngle(
      relativePosition,
      servo1IntensityPercent);

  hapticServo.write(targetAngle);

  Serial.print("SERVO_1_ANGLE:");
  Serial.println(targetAngle);

  if (
    latencyMeasurementActive && relativePosition != 0) {

    const unsigned long firstMovementUs =
      micros();

    const unsigned long latencyUs =
      firstMovementUs - patternCommandReceivedUs;

    const float latencyMs =
      latencyUs / 1000.0f;

    Serial.print(latencySource);
    Serial.print(
      "_TO_FIRST_SERVO_COMMAND_LATENCY_MS:");

    Serial.println(latencyMs, 3);

    latencyMeasurementActive = false;
    latencySource = "";
  }
}


void setServo1Intensity(int intensity) {
  intensity = constrain(intensity, 0, 100);

  servo1IntensityPercent =
    static_cast<uint8_t>(intensity);

  Serial.print("SERVO_1_INTENSITY:");
  Serial.println(servo1IntensityPercent);

  broadcastStatus();
}


// ============================================================
// Pattern engine
// ============================================================

void stopPattern(
  bool returnToNeutral = true,
  bool notifyClients = true) {
  patternRunning = false;

  activePattern = nullptr;
  activePatternLength = 0;
  currentStepIndex = 0;

  activeDefect = DefectType::None;

  if (returnToNeutral) {
    hapticServo.write(SERVO_NEUTRAL_ANGLE);

    Serial.print("SERVO_1_ANGLE:");
    Serial.println(SERVO_NEUTRAL_ANGLE);
  }

  if (notifyClients) {
    broadcastStatus();
  }
}


void startPattern(
  DefectType defect,
  const HapticStep* pattern,
  size_t patternLength) {
  stopPattern(false, false);

  activeDefect = defect;
  activePattern = pattern;
  activePatternLength = patternLength;

  currentStepIndex = 0;
  patternRunning = true;
  stepStartedAtMs = millis();

  Serial.print("PATTERN_STARTED:");
  Serial.println(defectTypeToText(defect));

  moveServo(
    activePattern[currentStepIndex].relativePosition);

  broadcastStatus();
}


void updatePattern() {
  if (
    !patternRunning || activePattern == nullptr) {
    return;
  }

  const uint32_t now = millis();

  const HapticStep& currentStep =
    activePattern[currentStepIndex];

  if (
    now - stepStartedAtMs < currentStep.durationMs) {
    return;
  }

  currentStepIndex++;

  if (
    currentStepIndex >= activePatternLength) {
    Serial.println("PATTERN_FINISHED");

    stopPattern(true, true);

    return;
  }

  moveServo(
    activePattern[currentStepIndex].relativePosition);

  stepStartedAtMs = now;
}


// ============================================================
// Pattern selection
// ============================================================

bool playPatternNumber(char command) {
  switch (command) {
    case '1':
      startPattern(
        DefectType::Scratch,
        PATTERN_1,
        sizeof(PATTERN_1) / sizeof(PATTERN_1[0]));
      return true;

    case '2':
      startPattern(
        DefectType::Crack,
        PATTERN_2,
        sizeof(PATTERN_2) / sizeof(PATTERN_2[0]));
      return true;

    case '3':
      startPattern(
        DefectType::Dent,
        PATTERN_3,
        sizeof(PATTERN_3) / sizeof(PATTERN_3[0]));
      return true;

    case '4':
      startPattern(
        DefectType::Bump,
        PATTERN_4,
        sizeof(PATTERN_4) / sizeof(PATTERN_4[0]));
      return true;

    case '0':
      stopPattern(true, true);
      Serial.println("NEUTRAL");
      return true;

    case '9':
      stopPattern(true, true);
      Serial.println("EMERGENCY_STOP");
      return true;

    default:
      return false;
  }
}


// ============================================================
// Shared command processing
// ============================================================

bool processCommand(
  String command,
  const char* source,
  uint8_t clientNumber = 0) {
  command.trim();
  command.toUpperCase();

  if (command.length() == 0) {
    return false;
  }

  Serial.print("COMMAND_FROM_");
  Serial.print(source);
  Serial.print(":");
  Serial.println(command);


  if (command.startsWith("PING:")) {

    String id = command.substring(5);

    unsigned long receiveTime = micros();

    webSocket.sendTXT(
      clientNumber,
      "PONG:" + id + ":" + String(receiveTime));

    return true;
  }


  // Serial commands: 1, 2, 3, 4, 0, 9
  if (command.length() == 1) {
    return playPatternNumber(
      command.charAt(0));
  }


  // Web commands: P:1 ... P:4 and P:0
  if (
    command.startsWith("P:") && command.length() >= 3) {
    return playPatternNumber(
      command.charAt(2));
  }


  // Servo 1 intensity
  if (command.startsWith("I1:")) {
    String intensityText =
      command.substring(3);

    if (intensityText.length() == 0) {
      Serial.println(
        "ERROR:INVALID_INTENSITY");

      return false;
    }

    const int intensity =
      intensityText.toInt();

    setServo1Intensity(intensity);

    return true;
  }


  // Servo 2 is currently unavailable
  if (command.startsWith("I2:")) {
    Serial.println(
      "ERROR:SERVO_2_NOT_CONFIGURED");

    return false;
  }


  if (
    command == "STOP" || command == "EMERGENCY_STOP") {
    stopPattern(true, true);

    Serial.println("EMERGENCY_STOP");

    return true;
  }


  if (command == "NEUTRAL") {
    stopPattern(true, true);

    Serial.println("NEUTRAL");

    return true;
  }


  if (command == "STATUS") {
    broadcastStatus();

    return true;
  }


  Serial.print("UNKNOWN_COMMAND:");
  Serial.println(command);

  return false;
}


// ============================================================
// Serial Monitor input
// ============================================================

void updateSerialInput() {
  while (Serial.available() > 0) {
    const char incomingCharacter =
      static_cast<char>(Serial.read());

    if (
      incomingCharacter == '\n' || incomingCharacter == '\r') {
      if (serialInputBuffer.length() > 0) {

        String command = serialInputBuffer;

        serialInputBuffer = "";

        command.trim();
        command.toUpperCase();

        if (
          command == "1" || command == "2" || command == "3" || command == "4") {

          patternCommandReceivedUs = micros();
          latencyMeasurementActive = true;
          latencySource = "SERIAL";

          Serial.print(
            "SERIAL_LATENCY_TIMER_STARTED_US:");

          Serial.println(
            patternCommandReceivedUs);
        }

        const bool success =
          processCommand(
            command,
            "SERIAL");

        if (!success) {
          latencyMeasurementActive = false;
          latencySource = "";
        }

        serialInputBuffer = "";
      }

      continue;
    }

    serialInputBuffer += incomingCharacter;

    if (serialInputBuffer.length() > 50) {
      serialInputBuffer = "";

      latencyMeasurementActive = false;
      latencySource = "";

      Serial.println(
        "ERROR:SERIAL_COMMAND_TOO_LONG");
    }
  }
}


// ============================================================
// WebSocket event handler
// ============================================================

void handleWebSocketEvent(
  uint8_t clientNumber,
  WStype_t eventType,
  uint8_t* payload,
  size_t payloadLength) {
  switch (eventType) {
    case WStype_DISCONNECTED:
      Serial.print(
        "WEBSOCKET_CLIENT_DISCONNECTED:");
      Serial.println(clientNumber);
      break;


    case WStype_CONNECTED:
      {
        IPAddress clientIp =
          webSocket.remoteIP(clientNumber);

        Serial.print(
          "WEBSOCKET_CLIENT_CONNECTED:");
        Serial.println(clientNumber);

        Serial.print(
          "WEBSOCKET_CLIENT_IP:");
        Serial.println(clientIp);

        sendStatusToClient(clientNumber);

        break;
      }

    case WStype_TEXT:
      {
        String command;

        for (size_t i = 0; i < payloadLength; i++) {
          command += static_cast<char>(payload[i]);
        }

        command.trim();
        command.toUpperCase();

        if (command.startsWith("PING:")) {
          processCommand(command, "WEBSOCKET", clientNumber);
          return;
        }

        Serial.print("WEBSOCKET_RECEIVED:");
        Serial.println(command);


        if (
          command == "P:1" || command == "P:2" || command == "P:3" || command == "P:4") {
          patternCommandReceivedUs = micros();
          latencyMeasurementActive = true;

          latencySource = "WEBSOCKET";

          Serial.print(
            "LATENCY_TIMER_STARTED_US:");

          Serial.println(
            patternCommandReceivedUs);
        }

        const bool success =
          processCommand(
            command,
            "WEBSOCKET",
            clientNumber);
        if (!success) {
          latencyMeasurementActive = false;
          latencySource = "";
        }

        break;
      }

    case WStype_ERROR:
      Serial.print(
        "WEBSOCKET_ERROR_CLIENT:");
      Serial.println(clientNumber);
      break;


    case WStype_PING:
      Serial.print(
        "WEBSOCKET_PING_CLIENT:");
      Serial.println(clientNumber);
      break;


    case WStype_PONG:
      Serial.print(
        "WEBSOCKET_PONG_CLIENT:");
      Serial.println(clientNumber);
      break;


    default:
      break;
  }
}


// ============================================================
// HTTP and CORS
// ============================================================

void addCorsHeaders() {
  server.sendHeader(
    "Access-Control-Allow-Origin",
    "*");

  server.sendHeader(
    "Access-Control-Allow-Methods",
    "GET, OPTIONS");

  server.sendHeader(
    "Access-Control-Allow-Headers",
    "Content-Type");
}


void handleOptions() {
  addCorsHeaders();

  server.send(
    204,
    "text/plain",
    "");
}


void handleStatus() {
  addCorsHeaders();

  String json =
    createStatusJson();

  server.send(
    200,
    "application/json",
    json);
}


void handleCommand() {
  addCorsHeaders();

  if (!server.hasArg("value")) {
    server.send(
      400,
      "application/json",
      "{\"success\":false,"
      "\"message\":\"Missing value parameter\"}");

    return;
  }

  String command =
    server.arg("value");

  const bool success =
    processCommand(
      command,
      "HTTP");

  String json =
    createCommandResultJson(
      success,
      command);

  server.send(
    success ? 200 : 400,
    "application/json",
    json);
}


// ============================================================
// Wi-Fi, HTTP and WebSocket setup
// ============================================================

void setupWiFiAndServers() {
  WiFi.mode(WIFI_AP);

  const bool accessPointStarted =
    WiFi.softAP(
      WIFI_NAME,
      WIFI_PASSWORD);

  if (!accessPointStarted) {
    Serial.println(
      "ERROR:WIFI_ACCESS_POINT_FAILED");

    return;
  }

  Serial.println();
  Serial.println(
    "WIFI_ACCESS_POINT_READY");

  Serial.print("WIFI_NAME:");
  Serial.println(WIFI_NAME);

  Serial.print("WIFI_IP:");
  Serial.println(WiFi.softAPIP());


  // HTTP fallback
  server.on(
    "/api/status",
    HTTP_GET,
    handleStatus);

  server.on(
    "/api/status",
    HTTP_OPTIONS,
    handleOptions);

  server.on(
    "/api/command",
    HTTP_GET,
    handleCommand);

  server.on(
    "/api/command",
    HTTP_OPTIONS,
    handleOptions);

  server.onNotFound([]() {
    addCorsHeaders();

    server.send(
      404,
      "application/json",
      "{\"success\":false,"
      "\"message\":\"Endpoint not found\"}");
  });

  server.begin();

  Serial.println(
    "HTTP_SERVER_READY_PORT_80");


  // WebSocket server
  webSocket.begin();

  webSocket.onEvent(
    handleWebSocketEvent);

  webSocket.enableHeartbeat(
    15000,
    3000,
    2);

  Serial.println(
    "WEBSOCKET_SERVER_READY_PORT_81");
}


// ============================================================
// Arduino lifecycle
// ============================================================

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

  serialInputBuffer.reserve(64);

  hapticServo.setPeriodHertz(
    SERVO_FREQUENCY_HZ);

  const int servoChannel =
    hapticServo.attach(
      SERVO_PIN,
      SERVO_MIN_PULSE_US,
      SERVO_MAX_PULSE_US);

  if (servoChannel < 0) {
    Serial.println(
      "ERROR:SERVO_ATTACH_FAILED");
  } else {
    Serial.print(
      "SERVO_ATTACHED_CHANNEL:");
    Serial.println(servoChannel);
  }

  hapticServo.write(
    SERVO_NEUTRAL_ANGLE);

  delay(500);

  setupWiFiAndServers();

  Serial.println();
  Serial.println("READY");
  Serial.println("-------------------------");
  Serial.println("Serial commands:");
  Serial.println("1 = Scratch");
  Serial.println("2 = Crack");
  Serial.println("3 = Dent");
  Serial.println("4 = Bump");
  Serial.println("0 = Neutral");
  Serial.println("9 = Stop");
  Serial.println(
    "I1:75 = Intensity 75%");
  Serial.println("-------------------------");
  Serial.println("HTTP:");
  Serial.println(
    "http://192.168.4.1/api/status");
  Serial.println(
    "http://192.168.4.1/api/command?value=P:1");
  Serial.println("-------------------------");
  Serial.println("WebSocket:");
  Serial.println(
    "ws://192.168.4.1:81/");
  Serial.println("-------------------------");
}


void loop() {
  webSocket.loop();
  server.handleClient();

  updateSerialInput();
  updatePattern();

  delay(1);
}