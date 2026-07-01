#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <ESP32Servo.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// ---------------- WIFI / MQTT ----------------

const char* ssid = "Audi";
const char* password = "wsti1070";
const char* mqtt_server = "broker.hivemq.com";

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastReconnectAttempt = 0;
const long MQTT_RECONNECT_INTERVAL = 5000;

// ---------------- FIREBASE ----------------

const String FIREBASE_HOST =
  "smart-parking-system-d6e6c-default-rtdb.asia-southeast1.firebasedatabase.app";

const String FIREBASE_AUTH =
  "BXZkrd7H6AM61rMnF1ZHiKYN8GrhLCu62doeqPto";

const String FIREBASE_PATH = "/parking_status.json";

// ---------------- PINS ----------------

#define NUM_SLOTS 6

const int trigPins[NUM_SLOTS] = { 4, 17, 18, 21, 27, 32 };
const int echoPins[NUM_SLOTS] = { 15, 16, 5, 19, 14, 33 };

#define TRIG_EXIT 25
#define ECHO_EXIT 26

#define SERVO_IN_PIN 12
#define SERVO_OUT_PIN 13

#define PARKING_THRESHOLD 12
#define EXIT_THRESHOLD 12

#define GATE_CLOSE 17
#define GATE_OPEN 90

// ---------------- STRUCT ----------------

typedef struct {
  bool slot[NUM_SLOTS];
  bool exit_detect;
  int totalOccupied;
} ParkingData;

typedef struct {
  char line1[32];
  char line2[32];
  bool isError;
} DisplayMsg;

typedef struct {
  int command;
} CommandData;

// ---------------- GLOBAL ----------------

Servo servoIn;
Servo servoOut;

ParkingData parkingData;
DisplayMsg msgToLcd;

uint8_t esp2Address[] = { 0xC8, 0xF0, 0x9E, 0xA6, 0xBD, 0x9C };

// flags

volatile bool gateInTrigger = false;
volatile bool gateOutTrigger = false;

volatile bool isWaitingEntryResponse = false;
volatile bool isWaitingExitResponse = false;
volatile bool isWaitingForExitCam = false;

// timing

unsigned long exitRequestTime = 0;
unsigned long exitCooldownStart = 0;

const long MQTT_TIMEOUT = 50000;
const long COOLDOWN_DURATION = 10000;

bool isExitCooldown = false;

int exitStableCount = 0;

// RTOS

TaskHandle_t HardwareTaskHandle;
TaskHandle_t FirebaseTaskHandle;

QueueHandle_t firebaseQueue;

// ------------------------------------------------
// Ultrasonic
// ------------------------------------------------

long readUltrasonic(int trigPin, int echoPin) {

  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 25000);

  if (duration <= 0) return 999;

  return duration * 0.034 / 2;
}

// ------------------------------------------------
// Exit Reset
// ------------------------------------------------

void resetExitSystem() {

  isWaitingExitResponse = false;
  isWaitingForExitCam = false;
  exitStableCount = 0;
}

// ------------------------------------------------
// Firebase Sender
// ------------------------------------------------

void sendToFirebase(String jsonData) {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = "https://" + FIREBASE_HOST + FIREBASE_PATH + "?auth=" + FIREBASE_AUTH;

  http.begin(url);

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PUT(jsonData);

  if (httpCode > 0) {

    Serial.printf("🔥 FIREBASE HTTP %d\n", httpCode);

  } else {

    Serial.printf("❌ FIREBASE ERROR %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ------------------------------------------------
// Firebase Task
// ------------------------------------------------

void FirebaseTask(void* pv) {

  String json;

  for (;;) {

    if (xQueueReceive(firebaseQueue, &json, portMAX_DELAY) == pdTRUE) {

      sendToFirebase(json);
    }
  }
}

// ------------------------------------------------
// Hardware Task
// ------------------------------------------------

void HardwareTask(void* pv) {

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);

  servoIn.setPeriodHertz(50);
  servoOut.setPeriodHertz(50);

  servoIn.attach(SERVO_IN_PIN, 500, 2400);
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);

  servoIn.write(GATE_CLOSE);
  servoOut.write(GATE_CLOSE);

  vTaskDelay(1000 / portTICK_PERIOD_MS);

  servoIn.detach();
  servoOut.detach();

  unsigned long lastSlotScan = 0;
  unsigned long lastExitScan = 0;

  for (;;) {

    // ---------- ENTRY GATE ----------

    if (gateInTrigger) {

      gateInTrigger = false;

      servoIn.attach(SERVO_IN_PIN, 500, 2400);

      vTaskDelay(100 / portTICK_PERIOD_MS);

      servoIn.write(GATE_OPEN);

      vTaskDelay(3000 / portTICK_PERIOD_MS);

      servoIn.write(GATE_CLOSE);

      vTaskDelay(1000 / portTICK_PERIOD_MS);

      servoIn.detach();

      isWaitingEntryResponse = false;
    }

    // ---------- EXIT GATE ----------

    if (gateOutTrigger) {

      gateOutTrigger = false;

      servoOut.attach(SERVO_OUT_PIN, 500, 2400);

      vTaskDelay(100 / portTICK_PERIOD_MS);

      servoOut.write(GATE_OPEN);

      isExitCooldown = true;
      exitCooldownStart = millis();

      vTaskDelay(4000 / portTICK_PERIOD_MS);

      servoOut.write(GATE_CLOSE);

      vTaskDelay(1000 / portTICK_PERIOD_MS);

      servoOut.detach();

      resetExitSystem();
    }

    // ---------- EXIT SENSOR ----------

    if (!isExitCooldown && millis() - lastExitScan > 200) {

      int d = readUltrasonic(TRIG_EXIT, ECHO_EXIT);

      if (d < EXIT_THRESHOLD && d > 2) {

        exitStableCount++;

        if (exitStableCount >= 2 && !isWaitingForExitCam) {

          isWaitingForExitCam = true;

          isWaitingExitResponse = true;

          exitRequestTime = millis();

          client.publish("parking/exit/trigger", "CAPTURE");

          Serial.println("📸 EXIT CAPTURE");
        }

      } else {

        exitStableCount = 0;
      }

      lastExitScan = millis();
    }

    // ---------- SLOT SCAN ----------

    if (millis() - lastSlotScan > 2000) {

      StaticJsonDocument<512> doc;

      int occupiedCount = 0;

      for (int i = 0; i < NUM_SLOTS; i++) {

        int d = readUltrasonic(trigPins[i], echoPins[i]);

        bool occ = (d < PARKING_THRESHOLD && d > 2);

        parkingData.slot[i] = occ;

        if (occ) occupiedCount++;

        doc["slot_" + String(i)] = occ;

        vTaskDelay(25 / portTICK_PERIOD_MS);
      }

      parkingData.totalOccupied = occupiedCount;

      doc["total_occupied"] = occupiedCount;

      doc["last_update"] = millis();

      String json;

      serializeJson(doc, json);

      xQueueSend(firebaseQueue, &json, 0);

      parkingData.exit_detect = isWaitingForExitCam;

      esp_now_send(esp2Address, (uint8_t*)&parkingData, sizeof(parkingData));

      lastSlotScan = millis();
    }

    // ---------- EXIT TIMEOUT ----------

    if (isWaitingExitResponse && millis() - exitRequestTime > MQTT_TIMEOUT) {

      resetExitSystem();

      memset(&msgToLcd, 0, sizeof(msgToLcd));

      strncpy(msgToLcd.line1, " NETWORK ERROR ", 31);
      strncpy(msgToLcd.line2, " TIMEOUT / WIFI", 31);

      msgToLcd.isError = true;

      esp_now_send(esp2Address, (uint8_t*)&msgToLcd, sizeof(msgToLcd));

      Serial.println("⚠ EXIT TIMEOUT");
    }

    if (isExitCooldown && millis() - exitCooldownStart > COOLDOWN_DURATION) {

      isExitCooldown = false;
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ------------------------------------------------
// MQTT Callback
// ------------------------------------------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  Serial.println("----- MQTT MESSAGE RECEIVED -----");
  Serial.print("Topic: ");
  Serial.println(topic);

  Serial.print("Payload: ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  Serial.println("---------------------------------");

  StaticJsonDocument<512> doc;

  DeserializationError err = deserializeJson(doc, payload, length);

  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  String top = String(topic);

  // ENTRY RESULT
  if (top == "parking/entry/result") {

    Serial.println("ENTRY RESULT RECEIVED");

    const char* status = doc["status"] | "failed";

    memset(&msgToLcd, 0, sizeof(msgToLcd));

    if (String(status) == "success") {

      const char* plate = doc["plate"] | "WELCOME";

      strncpy(msgToLcd.line1, "ENTRY SUCCESS", 31);
      strncpy(msgToLcd.line2, plate, 31);

      msgToLcd.isError = false;

      gateInTrigger = true;

    } else {

      strncpy(msgToLcd.line1, " ENTRY FAILED ", 31);
      strncpy(msgToLcd.line2, " TRY AGAIN ", 31);

      msgToLcd.isError = true;

      isWaitingEntryResponse = false;
    }

    esp_now_send(esp2Address, (uint8_t*)&msgToLcd, sizeof(msgToLcd));
  }

  // EXIT RESULT
  if (top == "parking/exit/result") {

    Serial.println("EXIT RESULT RECEIVED");

    const char* status = doc["status"] | "failed";

    memset(&msgToLcd, 0, sizeof(msgToLcd));

    if (String(status) == "success") {

      int fee = doc["fee"] | 0;
      const char* duration = doc["duration"] | "0m";

      snprintf(msgToLcd.line1, 31, "FEE: %d THB", fee);
      snprintf(msgToLcd.line2, 31, "TIME: %s", duration);

      msgToLcd.isError = false;

      gateOutTrigger = true;

      Serial.println("EXIT SUCCESS → OPEN GATE");

    } else {

      strncpy(msgToLcd.line1, " SYSTEM ERROR ", 31);
      strncpy(msgToLcd.line2, " EXIT FAILED ", 31);

      msgToLcd.isError = true;

      resetExitSystem();

      Serial.println("EXIT FAILED");
    }

    esp_now_send(esp2Address, (uint8_t*)&msgToLcd, sizeof(msgToLcd));
  }
}

// ------------------------------------------------
// ESP-NOW RECEIVE
// ------------------------------------------------

void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {

  if (len == sizeof(CommandData)) {

    CommandData* cmd = (CommandData*)incomingData;

    if (cmd->command == 1 && !isWaitingEntryResponse) {

      isWaitingEntryResponse = true;

      client.publish("parking/entry/trigger", "CAPTURE");
    }
  }
}

// ------------------------------------------------
// MQTT Reconnect
// ------------------------------------------------

boolean reconnectMQTT() {

  String clientId = "MainGate_";
  clientId += String(random(0xffff), HEX);

  if (client.connect(clientId.c_str())) {

    client.subscribe("parking/entry/result");
    client.subscribe("parking/exit/result");

    Serial.println("MQTT Connected");

    return true;
  }

  return false;
}

// ------------------------------------------------
// Setup
// ------------------------------------------------

void setup() {

  Serial.begin(115200);

  for (int i = 0; i < NUM_SLOTS; i++) {

    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
  }

  pinMode(TRIG_EXIT, OUTPUT);
  pinMode(ECHO_EXIT, INPUT);

  // WIFI

  WiFi.mode(WIFI_AP_STA);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }

  Serial.println("WiFi Connected");

  // MQTT

  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);

  // ESP NOW

  if (esp_now_init() == ESP_OK) {

    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peerInfo = {};

    memcpy(peerInfo.peer_addr, esp2Address, 6);

    peerInfo.channel = WiFi.channel();
    peerInfo.encrypt = false;

    esp_now_add_peer(&peerInfo);
  }

  // QUEUE

  firebaseQueue = xQueueCreate(5, sizeof(String));

  // TASKS

  xTaskCreatePinnedToCore(
    HardwareTask,
    "HardwareTask",
    8192,
    NULL,
    3,
    &HardwareTaskHandle,
    1);

  xTaskCreatePinnedToCore(
    FirebaseTask,
    "FirebaseTask",
    8192,
    NULL,
    1,
    &FirebaseTaskHandle,
    0);
}

// ------------------------------------------------
// LOOP
// ------------------------------------------------

void loop() {

  if (WiFi.status() != WL_CONNECTED) {

    WiFi.reconnect();
  }

  if (!client.connected()) {

    if (millis() - lastReconnectAttempt > MQTT_RECONNECT_INTERVAL) {

      lastReconnectAttempt = millis();

      reconnectMQTT();
    }
  }

  client.loop();

  vTaskDelay(10 / portTICK_PERIOD_MS);
}