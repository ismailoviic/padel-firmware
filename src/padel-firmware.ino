#include <Arduino.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

// ==========================================
// 1. HARDWARE PINS & SETTINGS
// ==========================================
#define TOUCH_PIN 39

const int ledPins[5] = { 12, 2, 5, 17, 33 };

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
String SESSION_ENDPOINT = BASE_URL + "/v1/hardware/session";
String EVENTS_ENDPOINT = BASE_URL + "/v1/hardware/events";

// --- GITHUB OTA SETTINGS ---
// IMPORTANT: Change this version string EVERY time you upload new code to GitHub!
String CURRENT_VERSION = "1.44";
String GITHUB_VERSION_URL = "https://raw.githubusercontent.com/ismailoviic/padel-firmware/main/version.json";
String GITHUB_FIRMWARE_URL = "https://raw.githubusercontent.com/ismailoviic/padel-firmware/main/build/esp32.esp32.esp32/padel-firmware.ino.bin";

String deviceId = "";

// This unit's shared secret — issued once by `POST /v1/devices` when the
// court owner provisions it (alongside the publicCode printed on its QR
// label), then flashed here. Sent on every match-API request together with
// deviceId (its MAC address) to authenticate as a registered court unit.
String DEVICE_SECRET = "REPLACE_WITH_PROVISIONED_DEVICE_SECRET";

// State Flags
volatile bool isUpdating = false;
bool bootTelemetrySent = false;

// ==========================================
// 3. MATCH STATE MACHINE
// ==========================================
// IDLE       — no live match on this court right now; periodically poll
//              /session to discover one (player-side QR scans + leader
//              "Start" are what actually create/launch it — this unit just
//              watches for the result).
// MATCH_LIVE — a match is in progress on this court; every touch press is
//              measured and reported as a raw duration. Classifying it as a
//              point or an undo is entirely the backend's job — the unit
//              only ever describes *what it observed*.
enum DeviceMode { IDLE, MATCH_LIVE };
DeviceMode mode = IDLE;
String currentMatchId = "";

unsigned long lastSessionPollAt = 0;
const unsigned long SESSION_POLL_INTERVAL_MS = 4000;

// ==========================================
// 4. GITHUB OTA BACKGROUND TASK (Runs on Core 0)
// ==========================================
void performOTA(void* parameter) {
  for (;;) {
    // Wait before checking. Currently set to 1 minute (60000 ms) for testing.
    // For production, change to 3600000 (1 hour).
    vTaskDelay(60000 / portTICK_PERIOD_MS);

    if (!isUpdating && ETH.linkUp() && ETH.localIP().toString() != "0.0.0.0") {
      Serial.println("\n[OTA] Checking GitHub for updates...");

      WiFiClientSecure client;
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
// 5. LED FEEDBACK
// ==========================================
// The cahier des charges describes RGB feedback (Flash Bleu / Vert Fixe /
// Rouge Clignotant...), but this board carries 5 plain (non-RGB) LEDs —
// intent is conveyed through distinguishable on/off blink patterns instead:
//   * point registered     -> one short flash
//   * undo registered      -> two quick double-blinks
//   * match lifecycle      -> one long flash (live match found, or match over)
//   * device/event error   -> rapid triple-blink
void flashAll(int times, int onMs, int offMs) {
  for (int b = 0; b < times; b++) {
    for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], HIGH);
    delay(onMs);
    for (int i = 0; i < 5; i++) digitalWrite(ledPins[i], LOW);
    if (b < times - 1) delay(offMs);
  }
}

void ledPointFeedback()            { flashAll(1, 150, 0); }
void ledUndoFeedback()             { flashAll(2, 120, 120); }
void ledMatchLifecycleFeedback()   { flashAll(1, 800, 0); }
void ledErrorFeedback()            { flashAll(3, 100, 100); }

// ==========================================
// 6. MATCH-API COMMUNICATION
// ==========================================
bool networkReady() {
  String ip = ETH.localIP().toString();
  return ETH.linkUp() && ip != "0.0.0.0" && ip != "(IP unset)";
}

void addDeviceAuthHeaders(HTTPClient& http) {
  // Per-device shared-secret auth (see middleware/deviceAuth.ts on the
  // backend) — mirrors a Bearer token, but for machines instead of staff.
  http.addHeader("X-Device-Id", deviceId);
  http.addHeader("X-Device-Key", DEVICE_SECRET);
}

// Reports a raw event — 'touch' | 'match_started' | 'heartbeat' | 'error' —
// to POST /v1/hardware/events. `durationMs` is only meaningful for 'touch'
// (pass a negative value to omit it). On a 200 OK, `responseBody` carries the
// raw JSON reply (classification + score snapshot for touches) for the
// caller to inspect; returns false on any non-200 / network failure.
bool reportHardwareEvent(const String& type, long durationMs, const String& message, String& responseBody) {
  if (!networkReady()) {
    Serial.println("[EVENT] Network not ready yet — " + type + " dropped.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, EVENTS_ENDPOINT);
  addDeviceAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> body;
  body["type"] = type;
  if (durationMs >= 0) body["durationMs"] = durationMs;
  if (message.length() > 0) body["message"] = message;

  String jsonPayload;
  serializeJson(body, jsonPayload);
  Serial.println("\n[EVENT] Sending: " + jsonPayload);

  int httpCode = http.POST(jsonPayload);
  bool ok = false;

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("[EVENT] Response (" + String(httpCode) + "): " + response);
    if (httpCode == HTTP_CODE_OK) {
      responseBody = response;
      ok = true;
    }
  } else {
    Serial.print("[EVENT] Error sending event: ");
    Serial.println(httpCode);
  }

  http.end();
  return ok;
}

// Polls GET /v1/hardware/session — how this unit discovers its court's
// active match without ever hardcoding a match id, drives IDLE -> MATCH_LIVE
// when one goes live, and notices completion/forfeit/cancellation to fall
// back to IDLE.
void pollSession() {
  if (!networkReady()) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, SESSION_ENDPOINT);
  addDeviceAuthHeaders(http);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    deserializeJson(doc, payload);

    JsonVariant matchIdField = doc["data"]["matchId"];
    String status = doc["data"]["status"] | "";
    bool hasLiveMatch = !matchIdField.isNull() && status == "live";

    if (hasLiveMatch && mode != MATCH_LIVE) {
      currentMatchId = matchIdField.as<String>();
      mode = MATCH_LIVE;
      Serial.println("[MATCH] Live match detected on this court: " + currentMatchId);
      ledMatchLifecycleFeedback();
    } else if (!hasLiveMatch && mode == MATCH_LIVE) {
      Serial.println("[MATCH] Match " + currentMatchId + " is no longer live — back to IDLE");
      currentMatchId = "";
      mode = IDLE;
      ledMatchLifecycleFeedback();
    }
  } else if (httpCode == HTTP_CODE_UNAUTHORIZED) {
    Serial.println("[MATCH] Device rejected (401) — check DEVICE_SECRET / provisioning status.");
    ledErrorFeedback();
  } else {
    Serial.printf("[MATCH] Session poll failed. HTTP code: %d\n", httpCode);
  }

  http.end();
}

// ==========================================
// 7. MAIN SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Initialize LEDs immediately
  for (int i = 0; i < 5; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
  pinMode(TOUCH_PIN, INPUT);

  Serial.println("[SYS] Sensors Ready. System Booted. Mode: IDLE");
  Serial.println("[SYS] Firmware Version: " + CURRENT_VERSION);

  // Trigger Ethernet to start in the background (Non-Blocking)
  Serial.println("[NET] Starting Ethernet hardware in background...");
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);

  deviceId = ETH.macAddress();
  Serial.println("[NET] Device ID: " + deviceId);

  // Start the OTA background task
  xTaskCreatePinnedToCore(performOTA, "OTA_Task", 8192, NULL, 1, NULL, 0);
}

// ==========================================
// 8. MAIN LOOP (Runs on Core 1)
// ==========================================
void loop() {
  if (isUpdating) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return;
  }

  // --- BACKGROUND NETWORK CHECK ---
  if (!bootTelemetrySent) {
    if (networkReady()) {
      Serial.print("\n[NET] Success! IP Assigned: ");
      Serial.println(ETH.localIP());
      String ack;
      reportHardwareEvent("heartbeat", -1, "boot", ack);
      bootTelemetrySent = true;
    }
  }

  // --- SESSION DISCOVERY (drives IDLE <-> MATCH_LIVE) ---
  unsigned long now = millis();
  if (now - lastSessionPollAt >= SESSION_POLL_INTERVAL_MS) {
    lastSessionPollAt = now;
    pollSession();
  }

  // --- TOUCH SENSOR LOGIC — only meaningful mid-match ---
  // The unit measures press-and-release duration (exactly as the previous
  // generic telemetry did) and reports the raw value; it never decides
  // point-vs-undo locally — that classification, and all of the actual
  // padel scoring, lives entirely on the server.
  if (mode == MATCH_LIVE && digitalRead(TOUCH_PIN) == HIGH) {
    unsigned long startTime = millis();
    while (digitalRead(TOUCH_PIN) == HIGH) { delay(20); }
    unsigned long durationMs = millis() - startTime;

    Serial.println("[TOUCH] Press duration: " + String(durationMs) + "ms — reporting raw, server classifies");

    String responseBody;
    if (reportHardwareEvent("touch", (long)durationMs, "", responseBody)) {
      StaticJsonDocument<512> result;
      deserializeJson(result, responseBody);
      String action = result["data"]["action"] | "";
      bool matchOver = result["data"]["matchOver"] | false;

      if (action == "undo") {
        ledUndoFeedback();
      } else if (action == "point") {
        ledPointFeedback();
      }

      if (matchOver) {
        Serial.println("[MATCH] Match completed — returning to IDLE");
        ledMatchLifecycleFeedback();
        mode = IDLE;
        currentMatchId = "";
      }
    } else {
      // Most likely a 409 — the match ended/changed between our last poll
      // and this touch. The next session poll re-syncs our mode either way.
      ledErrorFeedback();
    }

    delay(200); // brief debounce before the next press is considered
  }

  vTaskDelay(10 / portTICK_PERIOD_MS);
}
