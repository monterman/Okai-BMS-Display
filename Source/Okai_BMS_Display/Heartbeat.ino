// Heartbeat.ino — Ruipu/Okai keep-alive on a DEDICATED Core-0 task.
//
// The pack stops outputting power unless it receives the 5-byte unlock at least
// every 5 s. The official jsutcliff/OKAI-Battery-Lib sends it every 1 s. Running
// the heartbeat inside the cooperative loop() (alongside the TFT redraw, LittleFS
// flush and WiFi file server) let those stages STARVE it past the 5 s deadline,
// so packs dropped. Moving it to its own high-priority task on Core 0 (loop()
// runs on Core 1) guarantees the cadence no matter what the UI/logging/WiFi do.
//
// TX-only on GPIO2 (Serial1 TX, wired to all 4 pack RX lines). No RX flush here,
// so there is no race with uartLoop()'s reads on Core 1.

#include "Config.h"
#include "OkaiBMS.h"

static const uint8_t HB[5] = { 0x3A, 0x13, 0x01, 0x16, 0x79 };

static void heartbeatTask(void* param) {
    for (;;) {
        Serial1.write(HB, sizeof(HB));            // GPIO2 → every pack at once
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

// Call AFTER uartInit() — Serial1 (GPIO2 TX) must already be open.
void heartbeatInit() {
    // Priority 6 > Arduino loopTask (1); Core 0 so loop() on Core 1 can't delay it.
    xTaskCreatePinnedToCore(heartbeatTask, "heartbeat", 2048, nullptr, 6, nullptr, 0);
}

// Heartbeat now runs in its own task — nothing to do in the main loop.
void heartbeatLoop() {}
