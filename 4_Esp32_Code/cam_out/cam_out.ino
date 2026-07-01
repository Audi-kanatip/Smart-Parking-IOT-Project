#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "base64.h"
#include <esp_wifi.h>

//////////////////////////////////////////////////////
//////////////////// WIFI CONFIG /////////////////////
//////////////////////////////////////////////////////

const char* ssid = "Megarule";
const char* password = "ismeyok555";

//////////////////////////////////////////////////////
//////////////////// MQTT CONFIG /////////////////////
//////////////////////////////////////////////////////

const char* mqtt_server = "broker.hivemq.com";

WiFiClient espClient;
PubSubClient client(espClient);

//////////////////////////////////////////////////////
//////////////////// HTTPS CLIENT ////////////////////
//////////////////////////////////////////////////////

WiFiClientSecure secureClient;

//////////////////////////////////////////////////////
//////////////////// CLOUD API ///////////////////////
//////////////////////////////////////////////////////

const char* serverUrl =
"https://asia-southeast1-smart-parking-system-d6e6c.cloudfunctions.net/handle_exit";

//////////////////////////////////////////////////////
//////////////////// CAMERA PINS /////////////////////
//////////////////////////////////////////////////////

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5

#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define FLASH_LED_PIN 4

//////////////////////////////////////////////////////

volatile bool startExitCapture = false;

//////////////////////////////////////////////////////
//////////////////// WIFI EVENT //////////////////////
//////////////////////////////////////////////////////

void WiFiEvent(WiFiEvent_t event)
{
  switch(event)
  {
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("⚠️ WiFi Lost → Reconnecting...");
      WiFi.reconnect();
      break;

    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.print("✅ WiFi IP: ");
      Serial.println(WiFi.localIP());
      break;

    default:
      break;
  }
}

//////////////////////////////////////////////////////
//////////////////// WIFI CONNECT ////////////////////
//////////////////////////////////////////////////////

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(WiFiEvent);

  Serial.println("📡 Connecting WiFi...");
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_11dBm); // ปรับได้ตั้งแต่ 8.5dBm ถึง 20dBm (ลองที่ 11 หรือ 15)

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ WiFi Connected");
}

//////////////////////////////////////////////////////
//////////////////// MQTT CALLBACK ///////////////////
//////////////////////////////////////////////////////

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String incoming;

  for (int i = 0; i < length; i++)
  {
    incoming += (char)payload[i];
  }

  Serial.print("📥 MQTT: ");
  Serial.println(incoming);

  if (String(topic) == "parking/exit/trigger")
  {
    startExitCapture = true;
    Serial.println("🎯 Exit Capture Triggered");
  }
}

//////////////////////////////////////////////////////
//////////////////// MQTT RECONNECT //////////////////
//////////////////////////////////////////////////////

void reconnectMQTT()
{
  while (!client.connected())
  {
    Serial.println("🔌 Connecting MQTT...");

    String clientId = "ExitCam_" + String(random(0xffff), HEX);

    if (client.connect(clientId.c_str()))
    {
      Serial.println("✅ MQTT Connected");

      client.subscribe("parking/exit/trigger");

      client.publish("parking/status", "EXIT CAM ONLINE");
    }
    else
    {
      Serial.print("❌ MQTT Failed rc=");
      Serial.println(client.state());

      delay(3000);
    }
  }
}

//////////////////////////////////////////////////////
//////////////////// CAMERA INIT /////////////////////
//////////////////////////////////////////////////////

void initCamera()
{
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if(psramFound())
  {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  }
  else
  {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK)
  {
    Serial.println("❌ Camera Init Failed");
    ESP.restart();
  }

  Serial.println("📷 Camera Ready");
}

//////////////////////////////////////////////////////
//////////////////// MQTT RESULT /////////////////////
//////////////////////////////////////////////////////

void publishResult(String response)
{
  if (!client.connected())
  {
    reconnectMQTT();
  }

  client.loop();

  client.publish("parking/exit/result", response.c_str());
  client.publish("parking/exit/gate", "OPEN");

  Serial.println("📡 MQTT Result Sent");
}

//////////////////////////////////////////////////////
//////////////////// CAPTURE & SEND //////////////////
//////////////////////////////////////////////////////

void captureAndSend()
{
  Serial.println("📸 Exit Capture Started");

  Serial.printf("Free Heap Before Capture: %d\n", ESP.getFreeHeap());

  if (ESP.getFreeHeap() < 60000)
  {
    Serial.println("⚠️ Low Memory - Skip Capture");
    return;
  }

  // Flush old frames
  for(int i=0;i<2;i++)
  {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  camera_fb_t * fb = esp_camera_fb_get();

  if (!fb)
  {
    Serial.println("❌ Capture Failed");
    return;
  }

  Serial.printf("📷 Image Size: %d bytes\n", fb->len);

  String base64Image = base64::encode(fb->buf, fb->len);

  esp_camera_fb_return(fb);

  delay(100);

  Serial.printf("Free Heap Before HTTP: %d\n", ESP.getFreeHeap());

  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;

    secureClient.setInsecure();

    http.begin(secureClient, serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(40000);

    String jsonRequest = "{\"image\":\"" + base64Image + "\"}";

    base64Image = "";

    Serial.println("📤 Sending to Cloud...");

    int httpCode = http.POST(jsonRequest);

    if (httpCode == 200)
    {
      String response = http.getString();

      Serial.println("✅ Cloud Response:");
      Serial.println(response);

      publishResult(response);
    }
    else
    {
      Serial.printf("❌ HTTP Error %d\n", httpCode);

      client.publish("parking/error", "EXIT HTTP ERROR");

      delay(2000);

      Serial.println("🔁 Retrying HTTP...");

      httpCode = http.POST(jsonRequest);

      if (httpCode == 200)
      {
        String response = http.getString();
        publishResult(response);
      }
    }

    http.end();
  }
  else
  {
    Serial.println("⚠️ WiFi Lost During Send");
    connectWiFi();
  }

  Serial.println("💤 Exit Unit Standby\n");
}

//////////////////////////////////////////////////////
//////////////////// SETUP ///////////////////////////
//////////////////////////////////////////////////////

void setup()
{
  Serial.begin(115200);
  setCpuFrequencyMhz(160);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  connectWiFi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
  client.setBufferSize(4096);

  initCamera();
}

//////////////////////////////////////////////////////
//////////////////// LOOP ////////////////////////////
//////////////////////////////////////////////////////

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  if (!client.connected())
  {
    reconnectMQTT();
  }

  client.loop();

  if (startExitCapture)
  {
    startExitCapture = false;

    captureAndSend();
  }
}