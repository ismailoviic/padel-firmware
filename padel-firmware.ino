#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>

// ==========================================
// 1. HARDWARE PINS & SETTINGS
// ==========================================
#define RST_PIN 32
#define SS_PIN 15
#define SCK_PIN 14
#define MISO_PIN 35
#define MOSI_PIN 4
#define TOUCH_PIN 39

const int ledPins[5] = { 12, 2, 5, 17, 33 };
MFRC522 mfrc522(SS_PIN, RST_PIN);

// ETH01 Specific PHY Configuration
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN
#define ETH_PHY_POWER 16
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18

// ==========================================
// 2. NETWORK, API & OTA CONFIGURATION
// ==========================================
String BASE_URL = "https://padel.esstechnologies.ma";
String TELEMETRY_ENDPOINT = BASE_URL + "/api/device/telemetry"; 

// --- GITHUB OTA SETTINGS ---
// IMPORTANT: Change this version string EVERY time you upload new code to GitHub!
String CURRENT_VERSION = "1.3"; 
String GITHUB_VERSION_URL = "https://raw.githubusercontent.com/ismailoviic/padel-firmware/main/version.json";
String GITHUB_FIRMWARE_URL = "https://raw.githubusercontent.com/ismailoviic/padel-firmware/main/build/esp32.esp32.esp32/padel-firmware.ino.bin";

String deviceId = "";
bool isSystemActive = false;
const String AUTHORIZED_TAG = "F3DAD4AA";

// State Flags
volatile bool isUpdating = false;
bool bootTelemetrySent = false; 

// ==========================================
// 3. GITHUB OTA BACKGROUND TASK (Runs on Core 0)
// ==========================================
void performOTA(void* parameter) {
  for (;;) {
    // Wait before checking. Currently set to 1 minute (60000 ms) for testing.
    // For production, change to 3600000 (1 hour).
    vTaskDelay(60000 / portTICK_PERIOD_MS); 

    if (!isUpdating && ETH.linkUp() && ETH.localIP().toString() != "0.0.0.0") {
      Serial.println("\n[OTA] Checking GitHub for updates...");
      
      NetworkClientSecure client;
      client.setInsecure(); // Bypass SSL verification for GitHub
      
      HTTPClient http;
      http.begin(client, GITHUB_VERSION_URL);
      int httpCode = http.GET();
      
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        StaticJsonDocument<200> doc;
        deserializeJson(doc, payload);
        String latestVersion = doc["version"];
        
        Serial.println("[OTA] Current Version: " + CURRENT_VERSION);
        Serial.println("[OTA] GitHub Version:  " + latestVersion);
        
        if (latestVersion != "null" && latestVersion != CURRENT_VERSION) {
           Serial.println("[OTA] New version found! Starting download...");
           isUpdating = true;
           
           // Turn on all LEDs to indicate downloading
           for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], HIGH);
           
           t_httpUpdate_return ret = httpUpdate.update(client, GITHUB_FIRMWARE_URL);
           
           switch (ret) {
             case HTTP_UPDATE_FAILED:
               Serial.printf("[OTA] Update Failed Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
               for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
               break;
             case HTTP_UPDATE_NO_UPDATES:
               Serial.println("[OTA] No Updates Available");
               for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
               break;
             case HTTP_UPDATE_OK:
               Serial.println("[OTA] Update OK! Rebooting...");
               break;
           }
           isUpdating = false;
        } else {
           Serial.println("[OTA] Device is up to date.");
        }
      } else {
         Serial.printf("[OTA] Failed to check version. HTTP Code: %d\n", httpCode);
      }
      http.end();
    }
  }
}

// ==========================================
// 4. API COMMUNICATION FUNCTION
// ==========================================
void sendTelemetry(String eventType, String eventData, String systemStatus) {
  if (!ETH.linkUp() || ETH.localIP().toString() == "0.0.0.0" || ETH.localIP().toString() == "(IP unset)") {
    Serial.println("[NET] Network not ready yet. Telemetry dropped.");
    return;
  }

  NetworkClientSecure secureClient;
  secureClient.setInsecure();  

  HTTPClient http;
  http.begin(secureClient, TELEMETRY_ENDPOINT);  
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["deviceId"] = deviceId;
  doc["event"] = eventType;
  doc["data"] = eventData;
  doc["status"] = systemStatus;

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  Serial.println("\n[API] Sending: " + jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("[API] Response: " + response);
    // Note: We no longer check for OTA flags here, GitHub handles it.
  } else {
    Serial.print("[API] Error on sending POST: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}

// ==========================================
// 5. MAIN SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Initialize LEDs immediately
  for (int i = 0; i < 5; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
  pinMode(TOUCH_PIN, INPUT);

  // Initialize SPI and RFID immediately
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  
  Serial.println("[SYS] Sensors Ready. System Booted. Status: INACTIVE");
  Serial.println("[SYS] Firmware Version: " + CURRENT_VERSION);

  // Trigger Ethernet to start in the background (Non-Blocking)
  Serial.println("[NET] Starting Ethernet hardware in background...");
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);

  deviceId = ETH.macAddress();
  Serial.println("[NET] Device ID: " + deviceId);

  // Start the OTA background task
  xTaskCreatePinnedToCore(performOTA, "OTA_Task", 8192, NULL, 1, NULL, 0);
}

// ==========================================
// 6. MAIN LOOP (Runs on Core 1)
// ==========================================
void loop() {
  if (isUpdating) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return;
  }

  // --- BACKGROUND NETWORK CHECK ---
  if (!bootTelemetrySent) {
    if (ETH.linkUp() && ETH.localIP().toString() != "0.0.0.0" && ETH.localIP().toString() != "(IP unset)") {
      Serial.print("\n[NET] Success! IP Assigned: ");
      Serial.println(ETH.localIP());
      sendTelemetry("SYSTEM_BOOT", "N/A", "INACTIVE");
      bootTelemetrySent = true; 
    }
  }

  // --- RFID SCAN LOGIC ---
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

    String scannedTag = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) scannedTag += "0";
      scannedTag += String(mfrc522.uid.uidByte[i], HEX);
    }
    scannedTag.toUpperCase();

    if (scannedTag == AUTHORIZED_TAG) {
      if (!isSystemActive) {
        isSystemActive = true;
        sendTelemetry("RFID_SCAN", scannedTag, "Access Granted - ACTIVE");

        for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], HIGH);
        delay(3000);
        for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
      } else {
        isSystemActive = false;
        sendTelemetry("RFID_SCAN", scannedTag, "Logging out - INACTIVE");
        for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
      }
    } else {
      sendTelemetry("RFID_SCAN", scannedTag, "Access Denied");
      for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], HIGH);
        delay(250);
        for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
        delay(250);
      }
    }

    mfrc522.PICC_HaltA();
    delay(500);
  }

  // --- TOUCH SENSOR LOGIC ---
  int touchState = digitalRead(TOUCH_PIN);

  if (isSystemActive && touchState == HIGH) {
    sendTelemetry("TOUCH_CLICK", "N/A", "Sequence Triggered");

    int speed = 150;
    for (int i = 0; i < 5; i++) {
      digitalWrite(ledPins[i], HIGH);
      delay(speed);
    }
    for (int i = 0; i < 5; i++) {
      digitalWrite(ledPins[i], LOW);
      delay(speed);
    }

    while (digitalRead(TOUCH_PIN) == HIGH) { delay(50); }
  }

  vTaskDelay(10 / portTICK_PERIOD_MS); 
}