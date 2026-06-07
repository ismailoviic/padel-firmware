# Padel Court IoT Match Unit

This repository contains the firmware for an ESP32-ETH01 based IoT device designed for Padel court match scoring. Each court carries two units (one per team side), identified in the app by a printed QR code. A unit's only live job is to watch for its court's match and report raw touch-sensor presses (with duration) to the backend — the backend owns 100% of the scoring logic (points, games, sets, tiebreaks) and tells the unit how to react via LED feedback.

Crucially, this device features a completely autonomous **GitHub-based Over-The-Air (OTA) update system**, allowing firmware upgrades without physical USB access.

## ✨ Key Features
* **Instant Hardware Response:** Built on a non-blocking architecture. Touch sensor inputs are processed instantly, even while the device negotiates its Ethernet connection in the background.
* **Match-Aware State Machine:** Polls the backend for its court's active match (`GET /v1/hardware/session`), switching between `IDLE` and `MATCH_LIVE` automatically — no match IDs are ever hardcoded.
* **Dumb Sensor, Smart Server:** Reports raw touch durations to `POST /v1/hardware/events`; the backend classifies each as a point or an undo and runs the actual padel scoring engine. The unit only renders the result via its LEDs.
* **Per-Device Authentication:** Identifies itself with its MAC address (`X-Device-Id`) plus a provisioned shared secret (`X-Device-Key`), issued once by the backend (`POST /v1/devices`) and flashed into the sketch.
* **Autonomous GitHub OTA:** The device checks this GitHub repository periodically. If a new version is detected in `version.json`, it securely downloads the compiled `.bin` file and updates its own flash memory.
* **Robust Networking:** Utilizes the ESP32-ETH01's hardware LAN8720 chip for highly stable, hardwired internet access.

## 🛠 Hardware Requirements
* **Microcontroller:** ESP32-ETH01 (v1.4)
* **Touch Sensor:** TTP223 (HW-763)
* **Feedback:** 5x Standard LEDs (with appropriate current-limiting resistors)

### Wiring Guide
Due to the ETH01's internal LAN routing, specific pins must be used to avoid boot-loop issues.

| Component | Pin / Interface | ESP32-ETH01 Pin | Notes |
| :--- | :--- | :--- | :--- |
| **Touch Sensor** | VCC | 3V3 | |
| | I/O | IO39 | Input only |
| **LED Array** | LED 1 | IO2 | |
| | LED 2 | IO12 | |
| | LED 3 | IO33 (485_EN) | |
| | LED 4 | IO5 (RXD) | |
| | LED 5 | IO17 (TXD) | |

## 📦 Software Dependencies
Install the following libraries via the Arduino Library Manager before compiling:
* `ArduinoJson` by Benoit Blanchon (v6.x or v7.x)

*(Note: Requires ESP32 Arduino Core v3.x or higher for `NetworkClientSecure` compatibility).*

## 🚀 How to Push an OTA Firmware Update
Because this device pulls its updates directly from this repository, you must follow this exact workflow to push a new feature or bug fix:

1. **Update the Code:** Make your changes in the Arduino IDE.
2. **Increment the Version:** In the `.ino` file, change `String CURRENT_VERSION = "x.x";` to your new version number (e.g., `"1.1"`).
3. **Compile the Binary:** In the Arduino IDE menu, click **Sketch -> Export compiled Binary**. 
4. **Update `version.json`:** Open the `version.json` file in this repository and change the version string to match your code exactly: `{"version": "1.1"}`.
5. **Commit and Push:** Upload both the new `version.json` and the newly generated `padel_device_async_work_code_v1.ino.bin` to the `main` branch of this repository.

Within its next polling cycle (currently set to 1 minute for testing), the ESP32 will detect the version bump, download the `.bin` file, and reboot with the new firmware.

> **⚠️ Important:** This GitHub repository must remain **Public** for the ESP32 to download the raw binary files without an authentication token.