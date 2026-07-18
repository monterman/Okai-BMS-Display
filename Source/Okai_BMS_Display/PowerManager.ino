// PowerManager.ino — BTN1 (GPIO0) hold-to-sleep.
//
// Hold BTN1 ≥ 4 s → deep sleep. Press BTN1 (any duration) to wake.
// During deep sleep all execution halts: no heartbeat, no UART, no logging.
// loggerShutdown() is called before sleep to close the active log file cleanly.
// Short BTN1 presses (<4 s) pass through to Display.ino (WiFi toggle / pack cycle).
//
// Wakeup: EXT0 on GPIO0, level LOW (button press).


#define OVERLAY_START_MS  500UL   // show progress overlay after this much hold time
#define OVERLAY_UPDATE_MS 100UL   // redraw interval during countdown

static uint32_t sPm_downMs      = 0;
static uint32_t sPm_lastOverlay = 0;
static bool     sPm_armed       = false;   // hold-to-sleep arms only AFTER BTN1 is released once

void powerManagerInit() {
    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON1_PIN, 0);
}

static void doSleep() {
    displaySleepOverlay(SLEEP_HOLD_MS);  // 100% bar — last frame shown
    delay(300);
    loggerShutdown();
    Serial.println("[PWR] deep sleep — press BTN1 to wake");
    Serial.flush();
    digitalWrite(TFT_BL_PIN, LOW);
    delay(50);
    gpio_hold_en((gpio_num_t)POWER_EN_PIN);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();  // does not return
}

void powerManagerLoop() {
    bool     pressed = (digitalRead(BUTTON1_PIN) == LOW);
    uint32_t now     = millis();

    if (!sPm_armed) {                  // after wake/boot, ignore a still-held BTN1 until released once
        if (!pressed) sPm_armed = true;
        return;
    }

    if (pressed) {
        if (sPm_downMs == 0) sPm_downMs = now;
        uint32_t held = now - sPm_downMs;

        if (held >= SLEEP_HOLD_MS) {
            doSleep();  // does not return
        }

        if (held >= OVERLAY_START_MS &&
            now - sPm_lastOverlay >= OVERLAY_UPDATE_MS) {
            sPm_lastOverlay = now;
            displaySleepOverlay(held);
        }
    } else {
        if (sPm_downMs != 0) {
            bool hadOverlay = (now - sPm_downMs) >= OVERLAY_START_MS;
            sPm_downMs      = 0;
            sPm_lastOverlay = 0;
            if (hadOverlay) displaySleepOverlay(0xFFFFFFFFUL);  // clear + restore display
        }
    }
}
