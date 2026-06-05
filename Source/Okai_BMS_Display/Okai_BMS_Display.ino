// Okai BMS Display — v0.1.0
// LILYGO T-Display-S3 (ESP32-S3) | 4x Ruipu/Okai 10S4P packs | 9600 baud
// No delay() anywhere. All timing via millis().

#include "Config.h"
#include <driver/gpio.h>

void setup() {
  // Latch LiPo power rail immediately — must come first or LiPo connection drops
  gpio_hold_dis((gpio_num_t)POWER_EN_PIN);
  pinMode(POWER_EN_PIN, OUTPUT);
  digitalWrite(POWER_EN_PIN, HIGH);

  Serial.begin(115200);   // UART0 — USB-C debug

  powerManagerInit(); // configures EXT0 wakeup before anything else
  heartbeatInit();
  uartInit();
  loggerInit();       // mounts LittleFS first (packlabelInit needs fsReady)
  packlabelInit();    // loads labels, session counters, initialises DS3231
  packRegistryInit(); // ensures /packs dir, ready to identify packs
  displayInit();
  wifiServerInit();

  Serial.println("Okai BMS Display ready — " FW_VERSION);
}

void loop() {
  powerManagerLoop(); // hold BTN1 ≥ 4 s → deep sleep (runs before display reads button)
  heartbeatLoop();    // Priority 1 — always first
  uartLoop();         // Priority 2 — read pack data
  loggerLoop();       // Priority 3 — flush to LittleFS
  displayLoop();      // Priority 4 — update TFT
  wifiServerLoop();   // Priority 5 — serve CSV if WiFi active
}
