#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_wifi.h>

#define NUM_SLOTS 6
#define DEBUG_LED 19
#define BUTTON_PIN 18

const char* target_ssid = "Audi";

LiquidCrystal_I2C lcdIn(0x27, 16, 2);  
LiquidCrystal_I2C lcdOut(0x23, 16, 2); 

// พินไฟสถานะช่องจอด 6 ช่อง
const int ledPins[NUM_SLOTS] = {15, 2, 4, 16, 17, 5};

#define ENTRY_GREEN 13
#define ENTRY_RED   12
#define EXIT_GREEN  14
#define EXIT_RED    27

uint8_t esp1Address[] = {0xC8, 0xF0, 0x9E, 0x83, 0xBC, 0x84};

// --- Structure Matching ESP1 ---
typedef struct ParkingData {
  bool slot[NUM_SLOTS];
  bool exit_detect;
  int totalOccupied;
} ParkingData;

typedef struct DisplayMsg {
  char line1[32]; 
  char line2[32];
  bool isError;
} DisplayMsg;

typedef struct CommandData {
  int command; 
} CommandData;

ParkingData incomingData;
DisplayMsg incomingMsg;
CommandData cmdToEsp1;

unsigned long lastBtnTime = 0;
unsigned long gateOpenStartTime = 0; 
bool isEntryOpen = false;
unsigned long exitOpenStartTime = 0; 
bool isExitOpen = false;

// 🎯 เพิ่มตัวแปรจัดการ Error Timeout
unsigned long errorStartTime = 0;
bool isErrorActive = false;

bool isWaitingEntry = false;
bool isWaitingExit = false;

// --- Helper Functions ---
void updateLcdIn(String l1, String l2) {
  lcdIn.clear();
  lcdIn.setCursor(0, 0); lcdIn.print(l1);
  lcdIn.setCursor(0, 1); lcdIn.print(l2);
}

void updateLcdOut(String l1, String l2) {
  lcdOut.clear();
  lcdOut.setCursor(0, 0); lcdOut.print(l1);
  lcdOut.setCursor(0, 1); lcdOut.print(l2);
}

// --- การรับข้อมูลจาก ESP1 ---
void onDataRecv(const uint8_t * mac, const uint8_t *data, int len) {
  
  // -------------------------------------------------------
  // 1. จัดการข้อมูลสถานะช่องจอด (ParkingData)
  // -------------------------------------------------------
  if (len == sizeof(ParkingData)) {
    memcpy(&incomingData, data, sizeof(incomingData));
    
    // อัปเดตไฟ LED ทีละช่อง (รถจอด=แดง/ดับ | ว่าง=เขียว/ติด)
    for (int i = 0; i < NUM_SLOTS; i++) {
      digitalWrite(ledPins[i], incomingData.slot[i] ? LOW : HIGH); 
    }

    // --- คำนวณและแสดงจำนวนที่ว่างที่หน้าจอขาเข้า (LCD In) ---
    int availableSlots = NUM_SLOTS - incomingData.totalOccupied; // 6 - จอดจริง = ที่ว่าง
    
    // อัปเดตเฉพาะตอนสถานะปกติ (ไม่ใช่ตอนกำลัง Capture หรือโชว์ Error)
    if (!isEntryOpen && !isWaitingEntry && !isErrorActive) {
      lcdIn.setCursor(0, 0);
      lcdIn.print("WELCOME TO PARK"); // หรือข้อความหัวข้อที่คุณต้องการ
      
      lcdIn.setCursor(0, 1);
      if (availableSlots > 0) {
        lcdIn.print(" FREE: ");
        lcdIn.print(availableSlots);
        lcdIn.print(" SLOTS    "); // เติม Space กันตัวเลขเก่าค้าง
      } else {
        lcdIn.print(" SORRY, FULL!   "); 
      }
    }

    // ตรวจจับรถขาออก (เซนเซอร์ Ultrasonic ขาออกตรวจเจอรถ)
    if (incomingData.exit_detect && !isExitOpen && !isWaitingExit && !isErrorActive) {
      updateLcdOut("VEHICLE DETECTED", "PROCESSING...");
      isWaitingExit = true; 
    }
    if (!incomingData.exit_detect && !isExitOpen) isWaitingExit = false;
  }
  
  // -------------------------------------------------------
  // 2. จัดการข้อความโชว์บนจอจากผลลัพธ์ MQTT (DisplayMsg)
  // -------------------------------------------------------
  else if (len == sizeof(DisplayMsg)) {
    memcpy(&incomingMsg, data, sizeof(incomingMsg));
    
    // 🚩 กรณีได้รับข้อมูลแบบ Error (เช่น หาป้ายไม่เจอ, เน็ตหลุด)
    if (incomingMsg.isError) {
      digitalWrite(DEBUG_LED, HIGH); 
      isErrorActive = true;
      errorStartTime = millis();

      if (isWaitingEntry) {
        updateLcdIn("ENTRY ERROR!", incomingMsg.line2);
        digitalWrite(ENTRY_GREEN, LOW);
        digitalWrite(ENTRY_RED, HIGH); // ล็อกไฟแดง
        isWaitingEntry = false;
      } else {
        updateLcdOut("EXIT ERROR!", incomingMsg.line2);
        digitalWrite(EXIT_GREEN, LOW);
        digitalWrite(EXIT_RED, HIGH); // ล็อกไฟแดง
        isWaitingExit = false;
      }
    } 
    // ✅ กรณีได้รับข้อมูลแบบ Success (เช่น จ่ายเงินสำเร็จ, จดจำป้ายได้)
    else {
      digitalWrite(DEBUG_LED, LOW);
      isErrorActive = false; 
      String line1 = String(incomingMsg.line1);
      
      // ตรวจว่าเป็นข้อมูลฝั่งขาออก (ดูจากคำว่า FEE หรือสถานะที่รออยู่)
      if (isWaitingExit || line1.indexOf("FEE") != -1) {
        updateLcdOut(incomingMsg.line1, incomingMsg.line2);
        digitalWrite(EXIT_GREEN, HIGH); 
        digitalWrite(EXIT_RED, LOW);   // เปลี่ยนเป็นไฟเขียวขาออก
        exitOpenStartTime = millis();
        isExitOpen = true; 
        isWaitingExit = false;
      } 
      // เป็นข้อมูลฝั่งขาเข้า
      else {
        updateLcdIn(incomingMsg.line1, incomingMsg.line2);
        digitalWrite(ENTRY_GREEN, HIGH); 
        digitalWrite(ENTRY_RED, LOW);  // เปลี่ยนเป็นไฟเขียวขาเข้า
        gateOpenStartTime = millis();
        isEntryOpen = true; 
        isWaitingEntry = false;
      }
    }
  }
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

int32_t getWiFiChannel(const char *ssid) {
  if (int32_t n = WiFi.scanNetworks()) {
    for (uint8_t i = 0; i < n; i++) {
      if (!strcmp(ssid, WiFi.SSID(i).c_str())) return WiFi.channel(i);
    }
  }
  return 0;
}

void setup() {
  Serial.begin(115200);

  lcdIn.init(); lcdIn.backlight();
  lcdOut.init(); lcdOut.backlight();

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW); 
  }
  
  pinMode(ENTRY_GREEN, OUTPUT); pinMode(ENTRY_RED, OUTPUT);
  pinMode(EXIT_GREEN, OUTPUT); pinMode(EXIT_RED, OUTPUT); 
  pinMode(DEBUG_LED, OUTPUT); 
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // สถานะเริ่มต้น: ไฟแดงต้องติดทั้งสองฝั่ง
  digitalWrite(ENTRY_RED, HIGH); digitalWrite(ENTRY_GREEN, LOW);
  digitalWrite(EXIT_RED, HIGH);  digitalWrite(EXIT_GREEN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  int32_t channel = getWiFiChannel(target_ssid);
  if (channel == 0) channel = 1;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) return;
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, esp1Address, 6);
  peerInfo.channel = channel;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  //updateLcdIn("Entry Gate", "PRESS BUTTON");
  updateLcdOut("EXIT GATE", "READY TO EXIT");
}

void loop() {
  // 1. จัดการปุ่มกดขาเข้า (Entry Trigger)
  if (digitalRead(BUTTON_PIN) == LOW && !isWaitingEntry && !isEntryOpen) {
    if (millis() - lastBtnTime > 5000) { 
      updateLcdIn("CAPTURING...", "PLEASE WAIT");
      isWaitingEntry = true; 
      isErrorActive = false; // รีเซ็ตสถานะ Error เมื่อเริ่มใหม่
      cmdToEsp1.command = 1; 
      esp_now_send(esp1Address, (uint8_t *) &cmdToEsp1, sizeof(cmdToEsp1));
      lastBtnTime = millis();
    }
  }

  // 2. Auto-Reset หลังเปิดไม้กั้นขาเข้า
  if (isEntryOpen && (millis() - gateOpenStartTime > 5000)) { 
    digitalWrite(ENTRY_GREEN, LOW);
    digitalWrite(ENTRY_RED, HIGH);
    isEntryOpen = false;
    //updateLcdIn("Entry Gate", "PRESS BUTTON");
  }

  // 3. Auto-Reset หลังเปิดไม้กั้นขาออก
  if (isExitOpen && (millis() - exitOpenStartTime > 5000)) {
    digitalWrite(EXIT_GREEN, LOW);
    digitalWrite(EXIT_RED, HIGH);
    isExitOpen = false;
    isWaitingExit = false;
    updateLcdOut("EXIT GATE", "READY TO EXIT");
  }

  // 4. 🎯 Auto-Reset เมื่อเจอ Error (ค้างหน้าจอ Error ไว้ 5 วิแล้วกลับมา Ready)
  if (isErrorActive && (millis() - errorStartTime > 5000)) {
    isErrorActive = false;
    digitalWrite(DEBUG_LED, LOW);
    //updateLcdIn("Entry Gate", "PRESS BUTTON");
    updateLcdOut("EXIT GATE", "READY TO EXIT");
  }
}