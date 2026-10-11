// Okai BMS Display — v0.1.0
// LILYGO T-Display-S3 (ESP32-S3) | 4x Ruipu/Okai 10S4P packs | 9600 baud
// No delay() anywhere. All timing via millis().

#include "Config.h"
#include "Led.h"
#include <driver/gpio.h>

void setup() {
  // Latch LiPo power rail immediately — must come first or LiPo connection drops
  gpio_hold_dis((gpio_num_t)POWER_EN_PIN);
  pinMode(POWER_EN_PIN, OUTPUT);
  digitalWrite(POWER_EN_PIN, HIGH);

  // 2026-10-11 - RELEASE THE DEEP-SLEEP PAD LATCH. doSleep() calls
  // gpio_deep_sleep_hold_en() (PowerManager.ino:55), which latches EVERY pad through deep
  // sleep — not just POWER_EN. Nothing ever released it: this function undid the hold on
  // POWER_EN alone, so after a wake the buttons stayed electrically frozen at whatever
  // level they held at the moment of sleep. An internal pull-up CANNOT fight a hold latch,
  // so a button that was down when the board slept reads LOW forever, the stuck-button
  // detector fires at 15 s and refuses it, and only a true power cycle clears it.
  //
  // That is exactly the owner's report: BTN2 red, no short in the switch, cleared by a
  // restart, back again later. Not yet proven to be the cause of his sightings — the boot
  // logs captured on 10-10 and 10-11 both show rst:0x15 (USB reset), not a sleep wake — but
  // it is a real latch with a two-line fix and it had to go either way.
  //
  // ORDER IS SAFETY-CRITICAL. POWER_EN must already be driven HIGH above before the global
  // hold is dropped; release it while the pin is undriven and the board cuts its own LiPo
  // rail. That is why this sits AFTER the digitalWrite and not before it.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)BUTTON1_PIN);
  gpio_hold_dis((gpio_num_t)BUTTON2_PIN);
  gpio_hold_dis((gpio_num_t)BUTTON3_PIN);

  Serial.begin(115200);   // UART0 — USB-C debug

  powerManagerInit(); // configures EXT0 wakeup before anything else
  uartInit();         // open Serial1 (GPIO2 TX) BEFORE the heartbeat task starts
  heartbeatInit();    // spawns the Core-1 (prio 20) keep-alive task
  ledInit();          // NeoPixel bars on GPIO10/13 — EARLY, so the keep-alive
                      // indicator + strips are live and cleared BEFORE the display /
                      // fs / wifi init (which must never gate or crash the indicator)
  loggerInit();       // mounts LittleFS first (packlabelInit needs fsReady)
  packlabelInit();    // loads labels, session counters, initialises DS3231
  packRegistryInit(); // ensures /packs dir, ready to identify packs
  displayInit();
  wifiServerInit();
  // 2026-10-06 - Owner's spec: "turn it off and on, and that should make it connect to
  // the network easily" - no button, no opening the box. Tries the stored networks,
  // falls back to its own AP, stays reachable for WIFI_ON_WINDOW_MS, and only shuts
  // down if nothing answered AND no charge is running - i.e. he is out riding.
  wifiStartBoot();

  Serial.println("Okai BMS Display ready — " FW_VERSION);
}

void loop() {
  powerManagerLoop(); // hold BTN1 ≥ 4 s → deep sleep (runs before display reads button)
  heartbeatLoop();    // no-op — keep-alive now runs on its own Core-1 task
  uartLoop();         // Priority 2 — read pack data
  rideWatchUpdate();  // SOP-038 interlocks — MUST run every pass, right after uartLoop()
                      // so it samples fresh frames. Nothing may early-return above this:
                      // the ride and load interlocks are only as good as their sampling.
  loggerLoop();       // Priority 3 — flush to LittleFS
  displayLoop();      // Priority 4 — update TFT
  wifiServerLoop();   // Priority 5 — serve CSV if WiFi active
  ledLoop();          // Priority 6 — NeoPixel bars + keep-alive indicator (non-blocking)
  diagLoop();         // bench cadence log over USB serial (bench-only)
}
