// Heartbeat.ino — Ruipu/Okai keep-alive on a DEDICATED high-priority task.
//
// The pack stops outputting power unless it receives the 5-byte unlock at least
// every 5 s. We send it every HEARTBEAT_INTERVAL_MS (1 s) — 5x margin.
//
// CRITICAL ROOT-CAUSE FIX: the keep-alive MUST never be starved past the pack's
// 5 s deadline. An earlier build pinned this task to Core 0 thinking that isolated
// it from loop(). But on ESP32 the **WiFi/BT stack also lives on Core 0 at
// priority 23** and preempts a priority-6 task — so turning on the WiFi AP (or any
// Core-0 burst) could delay the keep-alive long enough that packs slept and dropped
// ("turns off/on by itself"). The dumb Arduino never had WiFi, so it never saw this.
//
// Fix: run the heartbeat on **Core 1** (away from the WiFi stack) at **high priority
// (20)** so it PREEMPTS the UI / pack-reads / logging / LED loop instead of being
// preempted by them. It only runs ~5 ms every 1 s, so high priority costs nothing.
//
// TX-only on GPIO2 (Serial1 TX, wired to all 4 pack RX). Passive; no RX flush here.

#include "Config.h"
#include "OkaiBMS.h"

static const uint8_t HB[5] = { 0x3A, 0x13, 0x01, 0x16, 0x79 };

// Diagnostics (watch over USB serial to prove the cadence never slips)
volatile uint32_t g_hbCount  = 0;   // total keep-alives sent
volatile uint32_t g_hbMaxGap = 0;   // worst send-to-send gap, ms (should hover ~1005)
volatile uint32_t g_hbLastMs = 0;   // last-beat timestamp — read by the LED watchdog indicator

static void heartbeatTask(void* param) {
    uint32_t last = millis();
    for (;;) {
        Serial1.write(HB, sizeof(HB));        // GPIO2 → every pack at once
        uint32_t now = millis();
        g_hbLastMs = now;                     // publish beat time (LED indicator reads this)
        uint32_t gap = now - last;
        last = now;
        g_hbCount++;
        if (gap > g_hbMaxGap) g_hbMaxGap = gap;
        // Danger zone: anything over 3 s means we're flirting with the 5 s deadline.
        if (gap > 3000)
            Serial.printf("[HB] *** LONG GAP %lu ms — packs at risk of sleeping! ***\n",
                          (unsigned long)gap);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

// Call AFTER uartInit() — Serial1 (GPIO2 TX) must already be open.
void heartbeatInit() {
    // Core 1 (no WiFi driver there), priority 20 so nothing else on this core can delay
    // the keep-alive. Stack 3072 covers Serial1.write + the printf path.
    //
    // 2026-10-09 - 18 → 20. THE NUMBERS BELOW ARE READ OUT OF THE INSTALLED CORE
    // (arduino-esp32 3.3.8, tools/esp32s3-libs/3.3.8/sdkconfig), not remembered:
    //
    //   CONFIG_LWIP_TCPIP_TASK_PRIO=18          but
    //   CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y  ← lwIP is PINNED TO CORE 0.
    //
    // So the TCP/IP task never competes with a Core-1 task, and an earlier version of
    // this comment claiming it does was wrong. The task that DOES share Core 1 is
    // Arduino's own WiFi event handler:
    //
    //   "arduino_events", ESP_TASKD_EVENT_PRIO - 1  (NetworkEvents.cpp:71-73)
    //   ESP_TASKD_EVENT_PRIO = ESP_TASK_PRIO_MAX - 5 = 25 - 5 = 20,  so it runs at 19
    //   CONFIG_ARDUINO_EVENT_RUNNING_CORE=1         ← same core as this task
    //
    // At 18 the keep-alive was therefore STRICTLY PREEMPTED by every WiFi event callback.
    // At 19 it would merely time-slice with them (~1 ms granularity at
    // CONFIG_FREERTOS_HZ=1000). 20 puts it above arduino_events, which is what "nothing
    // on this core can delay the keep-alive" actually requires.
    //
    // 20 starves nothing: configMAX_PRIORITIES is 25 so the ceiling is 24; esp_timer (22),
    // the system event loop (20) and the WiFi driver (23) are all pinned to Core 0; and
    // Serial1 RX is interrupt-driven, not task-driven. This task runs ~5 ms per second.
    //
    // NOTE FOR THE BENCH: the three-stage SOP-038 proof on record was taken at priority 18
    // and does NOT transfer. Re-run it — serial maxgap, px11 blink on both strips, then the
    // owner confirming the buggy powers up AND STAYS powered.
    g_hbLastMs = millis();   // seed so the indicator starts "healthy", not stale-red
    xTaskCreatePinnedToCore(heartbeatTask, "heartbeat", 3072, nullptr, 20, nullptr, 1);
}

// Heartbeat runs in its own task — nothing to do in the main loop.
void heartbeatLoop() {}

// ── Bench diagnostics (call from loop()) ───────────────────────────────────────
// Turns a bench test into a readable event log: per-pack UP/QUIET edges, WiFi on/off
// edges, and a health line every 3 s (heartbeat count + worst gap since the last
// line, LilyGo 18650 voltage, WiFi state, which packs are live). Bench-only: opening
// the USB serial monitor resets this board, so never rely on it in the field.
void diagLoop() {
    uint32_t now = millis();
    static bool wasUp[NUM_PACKS] = { false, false, false, false };
    static bool wifiWas = false;

    if (wifiActive != wifiWas) { Serial.printf("[WIFI %s]\n", wifiActive ? "ON" : "off"); wifiWas = wifiActive; }

    for (int i = 0; i < NUM_PACKS; i++) {
        bool up = packs[i].valid && (now - packs[i].lastUpdateMs) < 1500;
        if ( up && !wasUp[i]) Serial.printf("[PACK%d] UP\n", i + 1);
        if (!up &&  wasUp[i]) Serial.printf("[PACK%d] QUIET (no frame %lu ms)\n", i + 1,
                                            (unsigned long)(now - packs[i].lastUpdateMs));
        wasUp[i] = up;
    }

    static uint32_t lastDiag = 0;
    if (now - lastDiag < 3000) return;
    lastDiag = now;
    float vbat = analogReadMilliVolts(BAT_ADC_PIN) / 1000.0f * 2.0f;
    uint32_t gap = g_hbMaxGap; g_hbMaxGap = 0;
    // 2026-10-09 - HEAP, two words, because the open question on station mode cannot be
    // answered without it: does a begin/stop reconnect cycle leak? The garage is at the edge
    // of coverage, so a long charge can run hundreds of those cycles, and a slow leak ends as
    // an OOM reboot - which on this board means the keep-alive stops. getMinFreeHeap() is the
    // one that matters: it is the low-water mark since boot, so a downward trend over a soak
    // is the leak, whereas free heap alone just bounces. Soak a 2 h charge with the router
    // OFF (forces the retry path every 60 s) and watch minfree.
    // 2026-10-09 - psram= and iram= added, because the first bench capture could not answer
    // "is PSRAM in use?" at all: the [DISP] line that reports it prints before a serial
    // monitor can be attached, so it is invisible unless the capture starts at t=0. That
    // question is load-bearing - the whole reason WiFi is sequential STA-then-AP rather than
    // APSTA is a stated premise that the ~106 kB framebuffer sits in internal DRAM because
    // there is no PSRAM. A number on every DIAG line costs nothing and settles it forever.
    // iram= is the one that actually predicts an OOM reboot: largest free INTERNAL block,
    // which fragmentation kills long before total free heap looks bad.
    Serial.printf("[DIAG] t=%lus hb=%lu maxgap=%lums vbat=%.2fV heap=%lu min=%lu iram=%lu "
                  "psram=%lu/%lu wifi=%s | p1=%c p2=%c p3=%c p4=%c\n",
                  (unsigned long)(now / 1000), (unsigned long)g_hbCount, (unsigned long)gap, vbat,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize(),
                  wifiActive ? "ON" : "off",
                  wasUp[0] ? '#' : '-', wasUp[1] ? '#' : '-', wasUp[2] ? '#' : '-', wasUp[3] ? '#' : '-');
}
