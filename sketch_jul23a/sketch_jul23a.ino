#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <esp_timer.h>


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
const char* WIFI_PASSWORD = "Haptic_26!Control#91";

WebServer server(80);
WebSocketsServer webSocket(81);


// ============================================================
// Servo configuration
// ============================================================

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

constexpr uint8_t I2C_SDA_PIN = 21;
constexpr uint8_t I2C_SCL_PIN = 22;

constexpr uint8_t SERVO_1_CHANNEL = 4;
constexpr uint8_t SERVO_2_CHANNEL = 5;

constexpr int SERVO_MIN_ANGLE = 35;
constexpr int SERVO_NEUTRAL_ANGLE = 90;
constexpr int SERVO_MAX_ANGLE = 170;

constexpr int SERVO_1_ANGLE_OFFSET = 0;
constexpr int SERVO_2_ANGLE_OFFSET = 0;

constexpr uint16_t SERVO_MIN_PULSE_US = 500;
constexpr uint16_t SERVO_MAX_PULSE_US = 2400;
constexpr uint16_t SERVO_FREQUENCY_HZ = 50;

constexpr uint32_t SERIAL_BAUD_RATE = 115200;

uint64_t patternCommandReceivedUs = 0;
bool latencyMeasurementActive = false;

String latencySource = "";

String latencyCommandId = "";

uint8_t latencyWebSocketClient = 0;
// ============================================================
// Pattern definition
// ============================================================

struct HapticStep {
  int8_t servo1Position;
  int8_t servo2Position;
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
  { 0, 0, 80 },

  { 30, 0, 90 },
  { 30, 20, 80 },
  { 18, 28, 80 },
  { -8, 18, 70 },
  { 0, -10, 70 },

  { 0, 0, 120 }
};

// 2: Crack
const HapticStep PATTERN_2[] = {

  { 0, 0, 150 },
  { 20, -10, 150 },
  { 45, -20, 150 },
  { 70, -30, 150 },
  { 45, -20, 150 },
  { 20, -10, 150 },
  { 0, 0, 250 }

};

// 3: Dent
const HapticStep PATTERN_3[] = {
  
  { 0, 0, 80 },

  { 80, -80, 110 },
  { -20, 20, 70 },

  { 0, 0, 180 }
 
};

// 4: Bump
const HapticStep PATTERN_4[] = {
    { 0, 0, 80 },

  // Servo 1 passes over the bump
  { 25, 0, 80 },
  { 60, 0, 110 },
  { 25, 0, 80 },
  { 0, 0, 100 },

  // Servo 2 passes over the same bump
  { 0, 25, 80 },
  { 0, 60, 110 },
  { 0, 25, 80 },
  { 0, 0, 150 }
};



// ============================================================
// Dynamic patterns received from Python
// ============================================================

constexpr size_t MAX_PATTERN_STEPS = 20;

HapticStep dynamicPattern1[MAX_PATTERN_STEPS];
HapticStep dynamicPattern2[MAX_PATTERN_STEPS];
HapticStep dynamicPattern3[MAX_PATTERN_STEPS];
HapticStep dynamicPattern4[MAX_PATTERN_STEPS];

size_t dynamicPattern1Length = 0;
size_t dynamicPattern2Length = 0;
size_t dynamicPattern3Length = 0;
size_t dynamicPattern4Length = 0;

bool dynamicPattern1Active = false;
bool dynamicPattern2Active = false;
bool dynamicPattern3Active = false;
bool dynamicPattern4Active = false;






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
uint8_t servo2IntensityPercent = 75;

int servo1StartAngle = SERVO_NEUTRAL_ANGLE;
int servo2StartAngle = SERVO_NEUTRAL_ANGLE;

bool servo1Enabled = true;
bool servo2Enabled = true;

String serialInputBuffer;


// ============================================================
// Forward declarations
// ============================================================

void broadcastStatus();
void sendStatusToClient(uint8_t clientNumber);
bool resetDynamicPattern(int patternNumber);


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





String uint64ToString(uint64_t value) {
  char buffer[24];

  snprintf(
    buffer,
    sizeof(buffer),
    "%llu",
    static_cast<unsigned long long>(value));

  return String(buffer);
}


void startLatencyMeasurement(
  const String& source,
  const String& commandId,
  uint8_t clientNumber,
  uint64_t receivedUs) {

  patternCommandReceivedUs = receivedUs;
  latencySource = source;
  latencyCommandId = commandId;
  latencyWebSocketClient = clientNumber;
  latencyMeasurementActive = true;
}


void finishLatencyMeasurement() {
  if (!latencyMeasurementActive) {
    return;
  }

  const uint64_t firstPwmSentUs =
    esp_timer_get_time();

  const uint64_t internalLatencyUs =
    firstPwmSentUs - patternCommandReceivedUs;

  String json = "{";

  json += "\"type\":\"latencyResult\",";
  json += "\"id\":\"";
  json += latencyCommandId;
  json += "\",";

  json += "\"source\":\"";
  json += latencySource;
  json += "\",";

  json += "\"receivedUs\":\"";
  json += uint64ToString(patternCommandReceivedUs);
  json += "\",";

  json += "\"firstPwmUs\":\"";
  json += uint64ToString(firstPwmSentUs);
  json += "\",";

  json += "\"internalLatencyUs\":\"";
  json += uint64ToString(internalLatencyUs);
  json += "\"";

  json += "}";

  if (latencySource == "WEBSOCKET") {
    webSocket.sendTXT(
      latencyWebSocketClient,
      json);
  }

  Serial.println(json);

  latencyMeasurementActive = false;
  latencySource = "";
  latencyCommandId = "";
}



// ============================================================
// Servo control
// ============================================================

int relativePositionToAngle(
  int8_t relativePosition,
  uint8_t intensityPercent,
  int startAngle) {

  int availableRange;

  if (relativePosition < 0) {
    availableRange =
      startAngle - SERVO_MIN_ANGLE;
  } else {
    availableRange =
      SERVO_MAX_ANGLE - startAngle;
  }

  int offset =
    (availableRange * relativePosition) / 100;

  offset =
    (offset * intensityPercent) / 100;

  return constrain(
    startAngle + offset,
    SERVO_MIN_ANGLE,
    SERVO_MAX_ANGLE);
}

uint16_t angleToPwmTicks(int angle) {
  angle = constrain(angle, 0, 180);

  uint32_t pulseUs = map(
    angle,
    0,
    180,
    SERVO_MIN_PULSE_US,
    SERVO_MAX_PULSE_US);

  return (pulseUs * SERVO_FREQUENCY_HZ * 4096UL) / 1000000UL;
}


void writeServoAngle(uint8_t channel, int angle) {
  pwm.setPWM(
    channel,
    0,
    angleToPwmTicks(angle));
}


void moveServo(
  int8_t servo1Position,
  int8_t servo2Position) {

  const int targetAngle1 =
    relativePositionToAngle(
      servo1Position,
      servo1IntensityPercent,
      servo1StartAngle);

  const int targetAngle2 =
    relativePositionToAngle(
      servo2Position,
      servo2IntensityPercent,
      servo2StartAngle);


  if (servo1Enabled) {
    writeServoAngle(
      SERVO_1_CHANNEL,
      targetAngle1 + SERVO_1_ANGLE_OFFSET);

    Serial.print("SERVO_1_ANGLE:");
    Serial.println(targetAngle1);

    if (
      latencyMeasurementActive && (servo1Position != 0 || servo2Position != 0)) {

      finishLatencyMeasurement();
    }
  }


  if (servo2Enabled) {
    writeServoAngle(
      SERVO_2_CHANNEL,
      targetAngle2 + SERVO_2_ANGLE_OFFSET);

    Serial.print("SERVO_2_ANGLE:");
    Serial.println(targetAngle2);

    if (
      latencyMeasurementActive && (servo1Position != 0 || servo2Position != 0)) {
      finishLatencyMeasurement();
    }
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

void setServo2Intensity(int intensity) {
  intensity = constrain(intensity, 0, 100);

  servo2IntensityPercent =
    static_cast<uint8_t>(intensity);

  Serial.print("SERVO_2_INTENSITY:");
  Serial.println(servo2IntensityPercent);

  broadcastStatus();
}


bool setDynamicPattern(
  int patternNumber,
  const String& patternData) {

  HapticStep* targetPattern = nullptr;
  size_t* targetLength = nullptr;
  bool* targetActive = nullptr;

  switch (patternNumber) {
    case 1:
      targetPattern = dynamicPattern1;
      targetLength = &dynamicPattern1Length;
      targetActive = &dynamicPattern1Active;
      break;

    case 2:
      targetPattern = dynamicPattern2;
      targetLength = &dynamicPattern2Length;
      targetActive = &dynamicPattern2Active;
      break;

    case 3:
      targetPattern = dynamicPattern3;
      targetLength = &dynamicPattern3Length;
      targetActive = &dynamicPattern3Active;
      break;

    case 4:
      targetPattern = dynamicPattern4;
      targetLength = &dynamicPattern4Length;
      targetActive = &dynamicPattern4Active;
      break;

    default:
      return false;
  }

  size_t stepCount = 0;
  int startPosition = 0;

  while (
    startPosition < patternData.length() && stepCount < MAX_PATTERN_STEPS) {

    int endPosition =
      patternData.indexOf(';', startPosition);

    String stepText;

    if (endPosition < 0) {
      stepText = patternData.substring(startPosition);
    } else {
      stepText = patternData.substring(
        startPosition,
        endPosition);
    }

    int comma1 = stepText.indexOf(',');
    int comma2 = stepText.indexOf(',', comma1 + 1);

    if (comma1 < 0 || comma2 < 0) {
      return false;
    }

    int servo1 =
      stepText.substring(0, comma1).toInt();

    int servo2 =
      stepText.substring(
                comma1 + 1,
                comma2)
        .toInt();

    int duration =
      stepText.substring(comma2 + 1).toInt();

    servo1 = constrain(servo1, -100, 100);
    servo2 = constrain(servo2, -100, 100);
    duration = constrain(duration, 1, 5000);

    targetPattern[stepCount] = {
      static_cast<int8_t>(servo1),
      static_cast<int8_t>(servo2),
      static_cast<uint16_t>(duration)
    };

    stepCount++;

    if (endPosition < 0) {
      break;
    }

    startPosition = endPosition + 1;
  }

  if (stepCount == 0) {
    return false;
  }

  *targetLength = stepCount;
  *targetActive = true;

  Serial.print("DYNAMIC_PATTERN_SET:");
  Serial.print(patternNumber);
  Serial.print(":STEPS:");
  Serial.println(stepCount);

  return true;
}

bool resetDynamicPattern(int patternNumber) {
  switch (patternNumber) {
    case 1:
      dynamicPattern1Length = 0;
      dynamicPattern1Active = false;
      break;

    case 2:
      dynamicPattern2Length = 0;
      dynamicPattern2Active = false;
      break;

    case 3:
      dynamicPattern3Length = 0;
      dynamicPattern3Active = false;
      break;

    case 4:
      dynamicPattern4Length = 0;
      dynamicPattern4Active = false;
      break;

    default:
      return false;
  }

  Serial.print("DYNAMIC_PATTERN_RESET:");
  Serial.println(patternNumber);

  return true;
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

    if (servo1Enabled) {
      writeServoAngle(
        SERVO_1_CHANNEL,
        servo1StartAngle + SERVO_1_ANGLE_OFFSET);
    }

    if (servo2Enabled) {
      writeServoAngle(
        SERVO_2_CHANNEL,
        servo2StartAngle + SERVO_2_ANGLE_OFFSET);
    }
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
    activePattern[currentStepIndex].servo1Position,
    activePattern[currentStepIndex].servo2Position);

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
    activePattern[currentStepIndex].servo1Position,
    activePattern[currentStepIndex].servo2Position);

  stepStartedAtMs = now;
}


// ============================================================
// Pattern selection
// ============================================================

bool playPatternNumber(char command) {
  switch (command) {

    case '1':
      if (dynamicPattern1Active) {
        startPattern(
          DefectType::Scratch,
          dynamicPattern1,
          dynamicPattern1Length);
      } else {
        startPattern(
          DefectType::Scratch,
          PATTERN_1,
          sizeof(PATTERN_1) / sizeof(PATTERN_1[0]));
      }
      return true;

    case '2':
      if (dynamicPattern2Active) {
        startPattern(
          DefectType::Crack,
          dynamicPattern2,
          dynamicPattern2Length);
      } else {
        startPattern(
          DefectType::Crack,
          PATTERN_2,
          sizeof(PATTERN_2) / sizeof(PATTERN_2[0]));
      }
      return true;

    case '3':
      if (dynamicPattern3Active) {
        startPattern(
          DefectType::Dent,
          dynamicPattern3,
          dynamicPattern3Length);
      } else {
        startPattern(
          DefectType::Dent,
          PATTERN_3,
          sizeof(PATTERN_3) / sizeof(PATTERN_3[0]));
      }
      return true;

    case '4':
      if (dynamicPattern4Active) {
        startPattern(
          DefectType::Bump,
          dynamicPattern4,
          dynamicPattern4Length);
      } else {
        startPattern(
          DefectType::Bump,
          PATTERN_4,
          sizeof(PATTERN_4) / sizeof(PATTERN_4[0]));
      }
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

  const uint64_t commandReceivedUs =
    esp_timer_get_time();

  command.trim();
  command.toUpperCase();

  if (command.length() == 0) {
    return false;
  }

  if (command.startsWith("PING:")) {
    const String id =
      command.substring(5);

    const uint64_t receivedUs =
      commandReceivedUs;

    const uint64_t responseSentUs =
      esp_timer_get_time();

    String response =
      "PONG:" + id + ":" + uint64ToString(receivedUs) + ":" + uint64ToString(responseSentUs);

    if (String(source) == "WEBSOCKET") {
      webSocket.sendTXT(
        clientNumber,
        response);
    } else {
      Serial.println(response);
    }

    return true;
  }


  if (command.startsWith("WEB_RESULT:")) {
    Serial.println();
    Serial.println("===== WEB SOCKET LATENCY RESULT =====");

    String resultData =
      command.substring(11);

    int startPosition = 0;
    int fieldNumber = 0;

    const char* labels[] = {
      "Command ID: ",
      "Pattern: ",
      "Servo 1 intensity: ",
      "Servo 2 intensity: ",
      "Browser to ESP32: ",
      "ESP32 to first PWM: ",
      "Total latency: ",
      "Sync RTT: "
    };

    while (fieldNumber < 8) {
      int separatorPosition =
        resultData.indexOf(':', startPosition);

      String value;

      if (separatorPosition < 0) {
        value =
          resultData.substring(startPosition);
      } else {
        value =
          resultData.substring(
            startPosition,
            separatorPosition);
      }

      Serial.print(labels[fieldNumber]);
      Serial.print(value);

      if (
        fieldNumber == 2 || fieldNumber == 3) {
        Serial.println("%");
      } else if (fieldNumber >= 4) {
        Serial.println(" ms");
      } else {
        Serial.println();
      }

      if (separatorPosition < 0) {
        break;
      }

      startPosition =
        separatorPosition + 1;

      fieldNumber++;
    }

    Serial.println("=====================================");
    Serial.println();

    return true;
  }


  Serial.print("COMMAND_FROM_");
  Serial.print(source);
  Serial.print(":");
  Serial.println(command);


  if (command.startsWith("PING:")) {
    const String id =
      command.substring(5);

    // T1: command received by ESP32
    const uint64_t receivedUs =
      commandReceivedUs;

    // T2: ESP32 is about to send the response
    const uint64_t responseSentUs =
      esp_timer_get_time();

    String response =
      "PONG:" + id + ":" + uint64ToString(receivedUs) + ":" + uint64ToString(responseSentUs);

    if (String(source) == "WEBSOCKET") {
      webSocket.sendTXT(
        clientNumber,
        response);
    } else {
      Serial.println(response);
    }

    return true;
  }


  // SET_PATTERN:<pattern-number>:<step-data>
  //
  // Example:
  // SET_PATTERN:1:0,0,100;80,0,100;-50,20,100;0,0,200
  if (command.startsWith("SET_PATTERN:")) {

    const int patternSeparator =
      command.indexOf(':', 12);

    if (patternSeparator < 0) {
      Serial.println("ERROR:INVALID_SET_PATTERN_COMMAND");
      return false;
    }

    const String patternNumberText =
      command.substring(12, patternSeparator);

    const String patternData =
      command.substring(patternSeparator + 1);

    if (
      patternNumberText.length() != 1 || patternNumberText.charAt(0) < '1' || patternNumberText.charAt(0) > '4' || patternData.length() == 0) {

      Serial.println("ERROR:INVALID_SET_PATTERN_COMMAND");
      return false;
    }

    const int patternNumber =
      patternNumberText.toInt();

    const bool success =
      setDynamicPattern(
        patternNumber,
        patternData);

    if (!success) {
      Serial.println("ERROR:SET_PATTERN_FAILED");
      return false;
    }

    Serial.print("SET_PATTERN_OK:");
    Serial.println(patternNumber);

    return true;
  }


  // RESET_PATTERN:<pattern-number>
  if (command.startsWith("RESET_PATTERN:")) {
    const String patternNumberText =
      command.substring(14);

    if (
      patternNumberText.length() != 1 || patternNumberText.charAt(0) < '1' || patternNumberText.charAt(0) > '4') {
      Serial.println("ERROR:INVALID_RESET_PATTERN_COMMAND");
      return false;
    }

    const int patternNumber =
      patternNumberText.toInt();

    const bool success =
      resetDynamicPattern(patternNumber);

    if (!success) {
      Serial.println("ERROR:RESET_PATTERN_FAILED");
      return false;
    }

    Serial.print("RESET_PATTERN_OK:");
    Serial.println(patternNumber);

    return true;
  }


  // RUN:<command-id>:<pattern-number>
  if (command.startsWith("RUN:")) {
    const int separatorPosition =
      command.indexOf(':', 4);

    if (separatorPosition < 0) {
      Serial.println("ERROR:INVALID_RUN_COMMAND");
      return false;
    }

    const String commandId =
      command.substring(4, separatorPosition);

    const String patternText =
      command.substring(separatorPosition + 1);

    if (
      commandId.length() == 0 || patternText.length() != 1 || patternText.charAt(0) < '1' || patternText.charAt(0) > '4') {

      Serial.println("ERROR:INVALID_RUN_COMMAND");
      return false;
    }

    startLatencyMeasurement(
      String(source),
      commandId,
      clientNumber,
      commandReceivedUs);

    const bool started =
      playPatternNumber(
        patternText.charAt(0));

    if (!started) {
      latencyMeasurementActive = false;
      latencySource = "";
      latencyCommandId = "";
    }

    return started;
  }


  // Old Serial commands: 1, 2, 3, 4, 0, 9
  if (command.length() == 1) {
    return playPatternNumber(
      command.charAt(0));
  }


  // Old WebSocket commands: P:1 ... P:4 and P:0
  if (
    command.startsWith("P:") && command.length() >= 3) {

    return playPatternNumber(
      command.charAt(2));
  }



  // START POSITION SERVO 1
  if (command.startsWith("S1:")) {
    String angleText = command.substring(3);

    if (angleText.length() == 0) {
      Serial.println("ERROR:INVALID_START_POSITION");
      return false;
    }

    int angle = angleText.toInt();

    angle = constrain(
      angle,
      SERVO_MIN_ANGLE,
      SERVO_MAX_ANGLE);

    servo1StartAngle = angle;

    writeServoAngle(
      SERVO_1_CHANNEL,
      servo1StartAngle + SERVO_1_ANGLE_OFFSET);

    Serial.print("SERVO_1_START_ANGLE:");
    Serial.println(servo1StartAngle);

    return true;
  }


  // START POSITION SERVO 2
  if (command.startsWith("S2:")) {
    String angleText = command.substring(3);

    if (angleText.length() == 0) {
      Serial.println("ERROR:INVALID_START_POSITION");
      return false;
    }

    int angle = angleText.toInt();

    angle = constrain(
      angle,
      SERVO_MIN_ANGLE,
      SERVO_MAX_ANGLE);

    servo2StartAngle = angle;

    writeServoAngle(
      SERVO_2_CHANNEL,
      servo2StartAngle + SERVO_2_ANGLE_OFFSET);

    Serial.print("SERVO_2_START_ANGLE:");
    Serial.println(servo2StartAngle);

    return true;
  }





  if (command.startsWith("I1:")) {
    String intensityText =
      command.substring(3);

    if (intensityText.length() == 0) {
      Serial.println("ERROR:INVALID_INTENSITY");
      return false;
    }

    setServo1Intensity(
      intensityText.toInt());

    return true;
  }


  if (command.startsWith("I2:")) {
    String intensityText =
      command.substring(3);

    if (intensityText.length() == 0) {
      Serial.println("ERROR:INVALID_INTENSITY");
      return false;
    }

    setServo2Intensity(
      intensityText.toInt());

    return true;
  }


  if (command == "E1:1") {
    servo1Enabled = true;
    return true;
  }

  if (command == "E1:0") {
    servo1Enabled = false;
    return true;
  }

  if (command == "E2:1") {
    servo2Enabled = true;
    return true;
  }

  if (command == "E2:0") {
    servo2Enabled = false;
    return true;
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

        /* if (
          command == "1" || command == "2" || command == "3" || command == "4") {

          patternCommandReceivedUs = micros();
          latencyMeasurementActive = true;
          latencySource = "SERIAL";

          Serial.print(
            "SERIAL_LATENCY_TIMER_STARTED_US:");

          Serial.println(
            patternCommandReceivedUs);
        } */

        const bool success =
          processCommand(
            command,
            "SERIAL");

        /*    if (!success) {
          latencyMeasurementActive = false;
          latencySource = "";
        }
*/
        serialInputBuffer = "";
      }

      continue;
    }

    serialInputBuffer += incomingCharacter;

    if (serialInputBuffer.length() > 500) {
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


        /*if (
          command == "P:1" || command == "P:2" || command == "P:3" || command == "P:4") {
          patternCommandReceivedUs = micros();
          latencyMeasurementActive = true;

          latencySource = "WEBSOCKET";

          Serial.print(
            "LATENCY_TIMER_STARTED_US:");

          Serial.println(
            patternCommandReceivedUs);
        }*/

        const bool success =
          processCommand(
            command,
            "WEBSOCKET",
            clientNumber);
        /*if (!success) {
          latencyMeasurementActive = false;
          latencySource = "";
        }*/

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

  serialInputBuffer.reserve(512);

  Wire.begin(
    I2C_SDA_PIN,
    I2C_SCL_PIN);

  pwm.begin();

  pwm.setPWMFreq(
    SERVO_FREQUENCY_HZ);

  writeServoAngle(
    SERVO_1_CHANNEL,
    servo1StartAngle + SERVO_1_ANGLE_OFFSET);

  writeServoAngle(
    SERVO_2_CHANNEL,
    servo2StartAngle + SERVO_2_ANGLE_OFFSET);

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