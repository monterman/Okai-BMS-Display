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
static bool     sPm_sleepArmed  = false;   // held past SLEEP_HOLD_MS; sleeps on RELEASE
static uint32_t sPm_lastLoadMs  = 0;       // last time any pack drew real current

// Any pack above this, any direction, counts as "in use".
#define PM_LOAD_A        1.0f
#define PM_LOAD_WINDOW   60000UL

static bool pmPacksLoadedRecently() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        float a = packs[i].current;
        if (a < 0) a = -a;
        if (a > PM_LOAD_A) { sPm_lastLoadMs = now; break; }
    }
    return sPm_lastLoadMs && (now - sPm_lastLoadMs) < PM_LOAD_WINDOW;
}

void powerManagerInit() {
    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON1_PIN, 0);
    // 2026-10-06 - TIMER WAKE, one line, and it converts a latch into an outage. EXT0 wakes
    // on GPIO0 going LOW, so a bridge that sleeps the board and THEN releases leaves the pin
    // HIGH and nothing ever wakes it: keep-alive dead, packs cut, buggy stopped and staying
    // stopped. With a timer the worst case is a 60 s interruption that recovers by itself.
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
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

        // 2026-10-06 - SLEEP ON RELEASE, NOT ON DURATION. This replaces a guard that did
        // not work and that I had reported as closing Rex's K-2. It did not close anything:
        // BTN_STUCK_MS is 15 s and SLEEP_HOLD_MS is 4 s, so the stuck flag was always still
        // false when it was tested; and displayLoop() early-returns on gSleepCountdownActive
        // ABOVE updateStuckButtons(), so from 500 ms of hold the detector was not even
        // running. Dead twice over, and reachable only in the one case that was already safe.
        //
        // The structural fix needs no faith in the button at all: ARM at SLEEP_HOLD_MS, then
        // sleep only on the RELEASE edge. A human always lets go. A water bridge does not -
        // it holds, or it chatters, and neither produces the clean release this now requires.
        // A bridge that holds forever simply never sleeps the board, which is the safe
        // failure. See the release branch below for where doSleep() actually happens.
        if (held >= SLEEP_HOLD_MS) sPm_sleepArmed = true;
        if (held >= OVERLAY_START_MS &&
            now - sPm_lastOverlay >= OVERLAY_UPDATE_MS) {
            sPm_lastOverlay = now;
            displaySleepOverlay(held);
        }
    } else {
        if (sPm_downMs != 0) {
            bool hadOverlay = (now - sPm_downMs) >= OVERLAY_START_MS;
            bool wantSleep  = sPm_sleepArmed;
            sPm_downMs      = 0;
            sPm_lastOverlay = 0;
            sPm_sleepArmed  = false;
            if (hadOverlay) displaySleepOverlay(0xFFFFFFFFUL);  // clear + restore display

            if (wantSleep) {
                // LOAD INTERLOCK. The only discriminator between "the owner wants to sleep"
                // and "water is bridging the button" that water cannot fake: a rider on the
                // water has packs under load. Sleeping there halts the keep-alive and the
                // packs cut output 5 s later, which is a vehicle stopping, not a screen
                // turning off. Recommended by the 2026-10-05 audit and not taken then.
                if (pmPacksLoadedRecently()) {
                    Serial.println("[PWR] sleep REFUSED - packs have been under load within "
                                   "the last 60 s. Sleeping would stop the keep-alive and cut "
                                   "traction power.");
                } else {
                    doSleep();  // does not return
                }
            }
        }
    }
}
