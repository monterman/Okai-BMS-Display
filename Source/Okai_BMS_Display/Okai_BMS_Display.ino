// Okai BMS Display — v0.1.0
// LILYGO T-Display-S3 (ESP32-S3) | 4x Ruipu/Okai 10S4P packs | 9600 baud
// No delay() anywhere. All timing via millis().

#include "Config.h"

void setup() {
  Serial.begin(115200);   // UART0 — USB-C debug

  heartbeatInit();
  uartInit();
  loggerInit();
  displayInit();
  wifiServerInit();

  Serial.println("Okai BMS Display ready — " FW_VERSION);
}

void loop() {
  heartbeatLoop();    // Priority 1 — always first
  uartLoop();         // Priority 2 — read pack data
  loggerLoop();       // Priority 3 — flush to LittleFS
  displayLoop();      // Priority 4 — update TFT
  wifiServerLoop();   // Priority 5 — serve CSV if WiFi active
}
