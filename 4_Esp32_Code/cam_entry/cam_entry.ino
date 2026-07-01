#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "base64.h"
#include <esp_wifi.h>

// --- 1. WiFi & MQTT Config ---
const char* ssid = "Audi";
const char* password = "wsti1070";
const char* mqtt_server = "broker.hivemq.com";
const char* serverUrl = "https://asia-southeast1-smart-parking-system-d6e6c.cloudfunctions.net/handle_entry"; 

WiFiClient espClient;
PubSubClient client(espClient);

// --- AI Thinker Pin Config ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN      4 

volatile bool startCapture = false;

// --- MQTT Callback ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) == "parking/entry/trigger") {
    startCapture = true;
    Serial.println("🎯 [EVENT] Entry Capture Triggered");
  }
}

void setup_wifi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi Connected: " + WiFi.localIP().toString());
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "EntryCam_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      client.subscribe("parking/entry/trigger");
      Serial.println("✅ MQTT Connected & Subscribed");
    } else { delay(5000); }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  //digitalWrite(FLASH_LED_PIN, LOW);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // ใช้ VGA เพื่อคุณภาพที่ AI วิเคราะห์ทะเบียนได้แม่นยำ
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA; 
    config.jpeg_quality = 10;          
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Camera Init Failed");
    return;
  }
  
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
}

void captureAndSend() {
  // 1. Memory Guard: เช็ค RAM ก่อนทำงานหนัก
  if (ESP.getFreeHeap() < 60000) {
    Serial.println("⚠️ Warning: Low Memory, skipping capture");
    return;
  }

  Serial.println("📸 [PROCESS] Entry Capture Started...");
  
  // 2. Flush Camera Buffers (ล้างภาพค้างเก่า)
  for(int i=0; i<2; i++) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  // 3. Capture with Flash
  //digitalWrite(FLASH_LED_PIN, HIGH);
  //delay(500); // ให้เวลาเซนเซอร์ปรับแสง
  camera_fb_t * fb = esp_camera_fb_get();
  //digitalWrite(FLASH_LED_PIN, LOW);

  if (!fb) {
    client.publish("parking/error", "{\"message\":\"Entry Cam Capture Failed\"}");
    return;
  }

  // 4. Base64 Encoding
  String base64Image = base64::encode(fb->buf, fb->len);
  esp_camera_fb_return(fb); 

  // 5. HTTP POST Request (JSON Mode)
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(30000); 

    // สร้าง JSON สำหรับส่ง
    String jsonRequest = "{\"image\":\"" + base64Image + "\"}";
    base64Image = ""; // คืน Memory ทันที
    
    Serial.println("📤 [HTTP] Posting to Firebase...");
    int httpResponseCode = http.POST(jsonRequest);

    if (httpResponseCode == 200) {
      String response = http.getString();
      Serial.println("✅ [HTTP] Success");
      
      // ส่ง Response (JSON) ไปยัง MQTT เพื่อให้ ESP1 ประมวลผลต่อ
      client.publish("parking/entry/result", response.c_str());
      client.publish("parking/entry/gate", "OPEN");
    } else {
      String errorMsg = "{\"message\":\"HTTP Error " + String(httpResponseCode) + "\"}";
      client.publish("parking/error", errorMsg.c_str());
      Serial.println("❌ [HTTP] Error: " + String(httpResponseCode));
    }
    http.end();
  } else {
    client.publish("parking/error", "{\"message\":\"WiFi Disconnected during upload\"}");
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  if (startCapture) {
    startCapture = false; 
    captureAndSend();
    Serial.println("💤 [PROCESS] Done, Returning to Standby");
  }
}