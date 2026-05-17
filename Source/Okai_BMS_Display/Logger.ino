// Logger.ino — incremental CSV logging to LittleFS (crash-safe)
//
// LittleFS is the built-in flash filesystem on ESP32.
// No external library needed — included with arduino-esp32.
// File survives power loss; each row is appended and flushed immediately.

#include <LittleFS.h>

bool fsReady = false;
static uint32_t _logLast;

void loggerInit() {
    _logLast = 0;

    if (!LittleFS.begin(true)) {     // true = format on first-time fail
        Serial.println("[LOG] LittleFS mount failed");
        return;
    }
    fsReady = true;

    // Create file with header if it doesn't exist yet
    if (!LittleFS.exists(LOG_FILENAME)) {
        File f = LittleFS.open(LOG_FILENAME, "w", true);
        if (f) { f.print(LOG_CSV_HEADER); f.close(); }
    }

    // Report free space
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    Serial.printf("[LOG] LittleFS ready  used=%u/%u bytes\n", used, total);
}

void loggerLoop() {
    if (!fsReady) return;
    if (millis() - _logLast < LOG_INTERVAL_MS) return;
    _logLast = millis();

    File f = LittleFS.open(LOG_FILENAME, "a");
    if (!f) { Serial.println("[LOG] open failed"); return; }

    uint32_t ts = millis() / 1000;
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        f.printf("%lu,%u,%u,%.3f,%.3f,%.3f,%.3f,%u\n",
                 ts,
                 (unsigned)(i + 1),
                 (unsigned)packs[i].soc,
                 packs[i].voltage,
                 packs[i].current,
                 packs[i].cellHigh,
                 packs[i].cellLow,
                 (unsigned)packs[i].maxTemp);
    }
    f.close();
}
