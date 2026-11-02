#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <VL53L1X.h>
#include <cstring>

const char* ssid = "Margaret_Hamilton";
const char* password = "12345678";  // TROCAR por senha forte antes do uso real.

ESP8266WebServer server(80);               

// Ordem configurável da esquerda para a direita: S0, S1, S2, S3.
// Mapeamento provisório de S0-S3; D5/D6 agora pertencem ao I2C do VL53.
// Revise esta lista e valide o shield antes de habilitar sensores de linha.
const uint8_t SENSOR_PINS[4] = {D5, D6, D7, D0};
// Troque para true somente depois de validar os quatro pinos fisicamente.
// Uma entrada digital aberta ainda pode ler HIGH/LOW; o ESP não detecta um
// cabo solto sem circuito externo de polarização ou diagnóstico.
const bool SENSOR_CONFIGURATION_VALID = false;
// true: sensor detecta linha em HIGH; false: detecta linha em LOW.
// Confirme a saída do módulo antes de alterar. Não usamos INPUT_PULLUP por padrão.
const bool SENSOR_ACTIVE_LEVEL_HIGH = true;
const uint8_t DISTANCE_SDA_PIN = D6;
const uint8_t DISTANCE_SCL_PIN = D5;
const uint32_t DISTANCE_STALE_TIMEOUT_MS = 1500;

VL53L0X vl53l0x;
VL53L1X vl53l1x;
enum class DistanceSensorModel : uint8_t { None, VL53L0X, VL53L1X };
DistanceSensorModel distanceSensorModel = DistanceSensorModel::None;
bool distanceSensorInitialized = false;
uint16_t lastDistanceMm = 0;
uint32_t lastDistanceReadAt = 0;

// Pinagem confirmada no teste do carrinho.
// D3/GPIO0 e D4/GPIO2 são pinos de boot: o shield não pode forçá-los
// para níveis incompatíveis durante reset. Teste primeiro sem motores.
const uint8_t MOTOR_A_PWM_PIN = D1;  // Motor A: direito
const uint8_t MOTOR_A_DIR_PIN = D3;
const uint8_t MOTOR_B_PWM_PIN = D2;  // Motor B: esquerdo
const uint8_t MOTOR_B_DIR_PIN = D4;

// Um valor diferente de -1 inverte a polaridade daquele motor.
// O teste mostrou que o Motor A precisa de sentido invertido para acompanhar o Motor B.
const int8_t MOTOR_A_POLARITY = -1;
const int8_t MOTOR_B_POLARITY = 1;
const uint32_t MANUAL_COMMAND_TIMEOUT_MS = 700;
const uint32_t TURN_PULSE_MS = 60;
const uint32_t AUTO_COMMAND_TIMEOUT_MS = 1200;
String direction = "stop";
uint8_t speedValue = 55;
bool automaticMode = false;
bool filesystemReady = false;
bool emergencyLatched = false;
uint32_t lastValidCommandAt = 0;
uint32_t automaticStartedAt = 0;
uint32_t turnStartedAt = 0;

bool sensorPinsAreUniqueAndSeparate() {
  for (uint8_t i = 0; i < 4; i++) {
    for (uint8_t j = i + 1; j < 4; j++) {
      if (SENSOR_PINS[i] == SENSOR_PINS[j]) return false;
    }

    if (SENSOR_PINS[i] == MOTOR_A_PWM_PIN ||
        SENSOR_PINS[i] == MOTOR_A_DIR_PIN ||
        SENSOR_PINS[i] == MOTOR_B_PWM_PIN ||
        SENSOR_PINS[i] == MOTOR_B_DIR_PIN ||
        SENSOR_PINS[i] == DISTANCE_SDA_PIN ||
        SENSOR_PINS[i] == DISTANCE_SCL_PIN) {
      return false;
    }
  }
  return true;
}

bool sensorsAreValidated() {
  return SENSOR_CONFIGURATION_VALID && sensorPinsAreUniqueAndSeparate();
}

void stopMotors();

void sendJson(int statusCode, JsonDocument& document) {
  String response;
  serializeJson(document, response);
  server.send(statusCode, "application/json; charset=utf-8", response);
}

void sendError(int statusCode, const char* message) {
  StaticJsonDocument<128> response;
  response["ok"] = false;
  response["error"] = message;
  sendJson(statusCode, response);
}

void rejectMoveAndStop(int statusCode, const char* message) {
  // Um comando inválido nunca mantém um movimento anterior ativo.
  automaticMode = false;
  automaticStartedAt = 0;
  stopMotors();
  sendError(statusCode, message);
}

void serveFile(const char* path, const char* contentType) {
  if (!filesystemReady) {
    server.send(503, "text/plain; charset=utf-8", "LittleFS indisponivel");
    return;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    server.send(404, "text/plain; charset=utf-8", "Arquivo nao encontrado");
    return;
  }

  server.streamFile(file, contentType);
  file.close();
}

void stopMotors() {
  analogWrite(MOTOR_A_PWM_PIN, 0);
  analogWrite(MOTOR_B_PWM_PIN, 0);
  direction = "stop";
  turnStartedAt = 0;
}

void driveMotor(uint8_t pwmPin, uint8_t directionPin, int motorDirection,
                int8_t polarity) {
  if (motorDirection == 0 || speedValue == 0) {
    analogWrite(pwmPin, 0);
    return;
  }

  const bool forwardLevel = (motorDirection * polarity) > 0;
  digitalWrite(directionPin, forwardLevel ? HIGH : LOW);
  const int pwmValue = map(speedValue, 0, 100, 0, 1023);
  analogWrite(pwmPin, pwmValue);
}

void moveRobot(const String& requestedDirection) {
  if (requestedDirection == "forward") {
    // Frente lógico corresponde ao sentido físico que antes era chamado de ré.
    driveMotor(MOTOR_A_PWM_PIN, MOTOR_A_DIR_PIN, -1, MOTOR_A_POLARITY);
    driveMotor(MOTOR_B_PWM_PIN, MOTOR_B_DIR_PIN, -1, MOTOR_B_POLARITY);
  } else if (requestedDirection == "backward") {
    // Ré lógico usa o sentido físico que antes era chamado de frente.
    driveMotor(MOTOR_A_PWM_PIN, MOTOR_A_DIR_PIN, 1, MOTOR_A_POLARITY);
    driveMotor(MOTOR_B_PWM_PIN, MOTOR_B_DIR_PIN, 1, MOTOR_B_POLARITY);
  } else if (requestedDirection == "left") {
    driveMotor(MOTOR_A_PWM_PIN, MOTOR_A_DIR_PIN, -1, MOTOR_A_POLARITY);
    driveMotor(MOTOR_B_PWM_PIN, MOTOR_B_DIR_PIN, 1, MOTOR_B_POLARITY);
  } else if (requestedDirection == "right") {
    driveMotor(MOTOR_A_PWM_PIN, MOTOR_A_DIR_PIN, 1, MOTOR_A_POLARITY);
    driveMotor(MOTOR_B_PWM_PIN, MOTOR_B_DIR_PIN, -1, MOTOR_B_POLARITY);
  } else {
    stopMotors();
    return;
  }

  direction = requestedDirection;
}

void readSensors(JsonArray sensors) {
  if (!sensorsAreValidated()) {
    for (uint8_t index = 0; index < 4; index++) sensors.add(0);
    return;
  }

  for (uint8_t index = 0; index < 4; index++) {
    const int rawValue = digitalRead(SENSOR_PINS[index]);
    const bool active = SENSOR_ACTIVE_LEVEL_HIGH ? rawValue == HIGH : rawValue == LOW;
    sensors.add(active ? 1 : 0);
  }
}

uint8_t readI2CRegister(uint16_t registerAddress, bool twoByteAddress) {
  Wire.beginTransmission(0x29);
  if (twoByteAddress) Wire.write(static_cast<uint8_t>(registerAddress >> 8));
  Wire.write(static_cast<uint8_t>(registerAddress & 0xFF));
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom(static_cast<uint8_t>(0x29), static_cast<uint8_t>(1)) != 1) return 0xFF;
  return static_cast<uint8_t>(Wire.read());
}

void initializeDistanceSensor() {
  Wire.begin(DISTANCE_SDA_PIN, DISTANCE_SCL_PIN);
  Wire.setClock(100000);

  if (readI2CRegister(0xC0, false) == 0xEE) {
    distanceSensorModel = DistanceSensorModel::VL53L0X;
    vl53l0x.setTimeout(80);
    distanceSensorInitialized = vl53l0x.init();
    if (distanceSensorInitialized) vl53l0x.startContinuous(50);
  } else if (readI2CRegister(0x010F, true) == 0xEA) {
    distanceSensorModel = DistanceSensorModel::VL53L1X;
    vl53l1x.setTimeout(80);
    distanceSensorInitialized = vl53l1x.init();
    if (distanceSensorInitialized) {
      vl53l1x.setDistanceMode(VL53L1X::Short);
      vl53l1x.setMeasurementTimingBudget(20000);
      vl53l1x.startContinuous(50);
    }
  }

  if (distanceSensorModel == DistanceSensorModel::None) {
    Serial.println(F("VL53 nao detectado; confira VIN, GND, SDA=D6 e SCL=D5."));
  } else if (!distanceSensorInitialized) {
    Serial.println(F("VL53 detectado, mas a inicializacao falhou."));
  } else {
    Serial.println(distanceSensorModel == DistanceSensorModel::VL53L0X
                       ? F("VL53L0X iniciado.")
                       : F("VL53L1X iniciado."));
  }
}

void updateDistanceSensor() {
  if (!distanceSensorInitialized) return;

  static uint32_t lastPollAt = 0;
  if (millis() - lastPollAt < 10) return;
  lastPollAt = millis();

  uint16_t measuredDistance = 0;
  bool measurementReady = false;

  if (distanceSensorModel == DistanceSensorModel::VL53L0X) {
    measurementReady = (readI2CRegister(0x13, false) & 0x07) != 0;
    if (measurementReady) {
      measuredDistance = vl53l0x.readRangeContinuousMillimeters();
      if (vl53l0x.timeoutOccurred() || measuredDistance == 65535) return;
    }
  } else if (distanceSensorModel == DistanceSensorModel::VL53L1X) {
    measurementReady = vl53l1x.dataReady();
    if (measurementReady) {
      measuredDistance = vl53l1x.readRangeContinuousMillimeters(false);
      if (vl53l1x.timeoutOccurred() ||
          vl53l1x.ranging_data.range_status != VL53L1X::RangeValid) return;
    }
  }

  if (measurementReady && measuredDistance > 0) {
    lastDistanceMm = measuredDistance;
    lastDistanceReadAt = millis();
  }
}

const char* distanceModelName() {
  if (distanceSensorModel == DistanceSensorModel::VL53L0X) return "VL53L0X";
  if (distanceSensorModel == DistanceSensorModel::VL53L1X) return "VL53L1X";
  return "not_detected";
}

void addDistanceState(JsonDocument& response) {
  const bool readingIsFresh = lastDistanceReadAt != 0 &&
      millis() - lastDistanceReadAt <= DISTANCE_STALE_TIMEOUT_MS;
  response["distanceAvailable"] = readingIsFresh;
  response["distanceMm"] = readingIsFresh ? lastDistanceMm : 0;
  response["distanceModel"] = distanceModelName();
}

bool readLineSensors(bool values[4]) {
  for (uint8_t index = 0; index < 4; index++) {
    const int rawValue = digitalRead(SENSOR_PINS[index]);
    if (rawValue != LOW && rawValue != HIGH) return false;
    values[index] = SENSOR_ACTIVE_LEVEL_HIGH ? rawValue == HIGH : rawValue == LOW;
  }
  return true;
}

void sendState() {
  StaticJsonDocument<384> response;
  response["ok"] = true;
  response["direction"] = direction;
  response["speed"] = speedValue;
  response["mode"] = automaticMode ? "auto" : "manual";
  response["emergency"] = emergencyLatched;
  response["sensorsValidated"] = sensorsAreValidated();
  readSensors(response["sensors"].to<JsonArray>());
  addDistanceState(response);
  sendJson(200, response);
}

bool isValidDirection(const char* value) {
  return strcmp(value, "forward") == 0 || strcmp(value, "backward") == 0 ||
         strcmp(value, "left") == 0 || strcmp(value, "right") == 0 ||
         strcmp(value, "stop") == 0;
}

void handleMove() {
  StaticJsonDocument<256> request;
  const DeserializationError error = deserializeJson(request, server.arg("plain"));
  if (error) {
    rejectMoveAndStop(400, "JSON invalido; motores parados");
    return;
  }

  if (!request["direction"].is<const char*>() ||
      !request["speed"].is<int>() ||
      !request["mode"].is<const char*>()) {
    rejectMoveAndStop(400, "Campos direction, speed e mode sao obrigatorios; motores parados");
    return;
  }

  const char* requestedDirection = request["direction"];
  const char* requestedMode = request["mode"];
  const int requestedSpeed = request["speed"];

  if (!isValidDirection(requestedDirection) || requestedSpeed < 0 ||
      requestedSpeed > 100 ||
      (strcmp(requestedMode, "manual") != 0 &&
       strcmp(requestedMode, "auto") != 0)) {
    rejectMoveAndStop(400, "Valores direction, speed ou mode invalidos; motores parados");
    return;
  }

  if (emergencyLatched) {
    sendError(423, "Parada de emergencia ativa; reinicie o ESP para liberar");
    return;
  }

  const bool requestedAuto = strcmp(requestedMode, "auto") == 0;
  if (requestedAuto && !sensorsAreValidated()) {
    sendError(409, "Modo automatico bloqueado: valide os pinos e a polaridade dos quatro sensores");
    return;
  }

  speedValue = static_cast<uint8_t>(requestedSpeed);

  const bool leavingAutomaticMode = automaticMode && !requestedAuto;
  if (leavingAutomaticMode) {
    automaticMode = false;
    automaticStartedAt = 0;
    stopMotors();
  }

  if (requestedAuto && !automaticMode) {
    // A primeira solicitação de automático apenas arma o modo e deixa parado.
    // Uma segunda solicitação explícita mantém o modo armado e inicia a leitura.
    automaticMode = true;
    stopMotors();
    lastValidCommandAt = millis();
    automaticStartedAt = 0;
  } else if (requestedAuto) {
    lastValidCommandAt = millis();
    if (automaticStartedAt == 0) {
      automaticStartedAt = millis();
    }
  } else if (leavingAutomaticMode) {
    // A troca de automático para manual sempre deixa os motores parados.
    lastValidCommandAt = millis();
  } else {
    automaticMode = false;
    lastValidCommandAt = millis();
    if (strcmp(requestedDirection, "stop") == 0) {
      stopMotors();
    } else {
      moveRobot(requestedDirection);
      turnStartedAt = (strcmp(requestedDirection, "left") == 0 ||
                       strcmp(requestedDirection, "right") == 0)
                          ? millis()
                          : 0;
    }
  }

  StaticJsonDocument<384> response;
  response["ok"] = true;
  response["direction"] = direction;
  response["speed"] = speedValue;
  response["mode"] = automaticMode ? "auto" : "manual";
  response["emergency"] = emergencyLatched;
  response["sensorsValidated"] = sensorsAreValidated();
  readSensors(response["sensors"].to<JsonArray>());
  addDistanceState(response);
  sendJson(200, response);
}

void handleEmergency() {
  automaticMode = false;
  emergencyLatched = true;
  stopMotors();

  StaticJsonDocument<128> response;
  response["ok"] = true;
  response["emergency"] = true;
  sendJson(200, response);
}

void handleNotFound() {
  sendError(404, "Rota nao encontrada");
}

void setup() {
  // Primeiro coloca todos os sinais no estado inativo (PWM=0 e direção LOW).
  // O intervalo antes de setup() ainda depende dos níveis elétricos do shield.
  digitalWrite(MOTOR_A_PWM_PIN, LOW);
  digitalWrite(MOTOR_B_PWM_PIN, LOW);
  digitalWrite(MOTOR_A_DIR_PIN, LOW);
  digitalWrite(MOTOR_B_DIR_PIN, LOW);
  pinMode(MOTOR_A_PWM_PIN, OUTPUT);
  pinMode(MOTOR_A_DIR_PIN, OUTPUT);
  pinMode(MOTOR_B_PWM_PIN, OUTPUT);
  pinMode(MOTOR_B_DIR_PIN, OUTPUT);
  analogWrite(MOTOR_A_PWM_PIN, 0);
  analogWrite(MOTOR_B_PWM_PIN, 0);
  stopMotors();

  Serial.begin(115200);

  // Os pinos de linha ficam intocados enquanto seu mapeamento não for validado.
  // D5/D6 pertencem ao barramento I2C do sensor VL53.
  if (sensorsAreValidated()) {
    for (uint8_t pin : SENSOR_PINS) pinMode(pin, INPUT);
  }

  if (!sensorPinsAreUniqueAndSeparate()) {
    Serial.println(F("ERRO: pinos de linha repetidos ou em conflito com motor/I2C."));
  }
  if (!SENSOR_CONFIGURATION_VALID) {
    Serial.println(F("Sensores nao validados; modo automatico bloqueado."));
  }

  initializeDistanceSensor();

  filesystemReady = LittleFS.begin();
  if (!filesystemReady) {
    Serial.println(F("ERRO: LittleFS.begin() falhou; verifique a imagem uploadfs."));
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(ssid, password)) {
    Serial.println(F("ERRO: nao foi possivel iniciar o ponto de acesso."));
  } else {
    Serial.printf("AP pronto: %s\n", WiFi.softAPIP().toString().c_str());
  }

  server.on("/", HTTP_GET, []() { serveFile("/index.html", "text/html; charset=utf-8"); });
  server.on("/style.css", HTTP_GET, []() { serveFile("/style.css", "text/css; charset=utf-8"); });
  server.on("/script.js", HTTP_GET, []() { serveFile("/script.js", "application/javascript; charset=utf-8"); });
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/move", HTTP_POST, handleMove);
  server.on("/api/emergency", HTTP_POST, handleEmergency);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("Servidor iniciado; motores permanecem parados no boot."));
}

void loop() {
  server.handleClient();
  updateDistanceSensor();

  if (!automaticMode) {
    if (turnStartedAt != 0 && millis() - turnStartedAt >= TURN_PULSE_MS) {
      stopMotors();
    }
    if (direction != "stop" && millis() - lastValidCommandAt > MANUAL_COMMAND_TIMEOUT_MS) {
      stopMotors();
    }
    return;
  }

  if (millis() - lastValidCommandAt > AUTO_COMMAND_TIMEOUT_MS) {
    automaticMode = false;
    stopMotors();
    Serial.println(F("Modo automatico cancelado por timeout de comunicacao."));
    return;
  }

  if (automaticStartedAt == 0 || millis() - automaticStartedAt < 1500) {
    stopMotors();
    return;
  }

  if (!sensorsAreValidated()) {
    automaticMode = false;
    stopMotors();
    return;
  }

  bool sensor[4];
  if (!readLineSensors(sensor)) {
    automaticMode = false;
    stopMotors();
    Serial.println(F("Leitura invalida; modo automatico cancelado."));
    return;
  }

  const bool allActive = sensor[0] && sensor[1] && sensor[2] && sensor[3];
  const bool anyActive = sensor[0] || sensor[1] || sensor[2] || sensor[3];
  if (allActive || !anyActive) {
    stopMotors();
    return;
  }

  if (sensor[1] || sensor[2]) {
    moveRobot("forward");
  } else if (sensor[0]) {
    moveRobot("left");
  } else if (sensor[3]) {
    moveRobot("right");
  } else {
    stopMotors();
  }

  delay(10);
}
