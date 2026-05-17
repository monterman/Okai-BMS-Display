// PackLabel.ino — pack label assignment (1-8), DS3231 RTC, session counters,
//                 filename generation, and browser-based time override
//
// Labels 1-8 stored in /labels.bin (4 bytes).
// Session counters (ride/charge) in /sessions.bin (4 bytes, uint16_t × 2).
// RTC is the primary time source; browser sync (/settime) can correct it.

#include <Wire.h>
#include <RTClib.h>   // Adafruit RTClib — install via Library Manager

// ── DS3231 ───────────────────────────────────────────────────────────────────
static RTC_DS3231 _rtc;
static bool       _rtcOk    = false;
static bool       _timeSynced = false;   // true once RTC has a valid time

// ── Labels ───────────────────────────────────────────────────────────────────
static uint8_t _labels[NUM_PACKS] = {0}; // 0 = unassigned, 1-8 = pack label

// ── Session counters ─────────────────────────────────────────────────────────
static uint16_t _sessRide   = 0;
static uint16_t _sessCharge = 0;

// ── Persistence helpers ───────────────────────────────────────────────────────
static void loadLabels() {
    if (!fsReady) return;
    File f = LittleFS.open("/labels.bin", "r");
    if (f && (size_t)f.size() >= NUM_PACKS) f.read(_labels, NUM_PACKS);
    if (f) f.close();
}

static void saveLabels() {
    if (!fsReady) return;
    File f = LittleFS.open("/labels.bin", "w", true);
    if (f) { f.write(_labels, NUM_PACKS); f.close(); }
}

static void loadSessions() {
    if (!fsReady) return;
    File f = LittleFS.open("/sessions.bin", "r");
    if (f && (size_t)f.size() >= 4) {
        f.read((uint8_t*)&_sessRide,   2);
        f.read((uint8_t*)&_sessCharge, 2);
    }
    if (f) f.close();
}

void saveSessions() {
    if (!fsReady) return;
    File f = LittleFS.open("/sessions.bin", "w", true);
    if (f) {
        f.write((uint8_t*)&_sessRide,   2);
        f.write((uint8_t*)&_sessCharge, 2);
        f.close();
    }
}

// ── Init ─────────────────────────────────────────────────────────────────────
void packlabelInit() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    if (_rtc.begin(&Wire)) {
        _rtcOk = true;
        if (_rtc.lostPower()) {
            // Crystal was reset — RTC has no valid time yet
            Serial.println("[RTC] lost power — time not set (open dashboard to set)");
            _timeSynced = false;
        } else {
            _timeSynced = true;
            DateTime now = _rtc.now();
            Serial.printf("[RTC] time: %04u-%02u-%02u %02u:%02u:%02u\n",
                          now.year(), now.month(), now.day(),
                          now.hour(), now.minute(), now.second());
        }
    } else {
        Serial.println("[RTC] DS3231 not found — timestamps will be NOSYNC");
        Serial.println("[RTC] check wiring: SDA=GPIO11  SCL=GPIO12");
    }

    loadLabels();
    loadSessions();
    Serial.printf("[LABEL] labels: %u %u %u %u  rides: %u  charges: %u\n",
                  _labels[0], _labels[1], _labels[2], _labels[3],
                  _sessRide, _sessCharge);
}

// ── Label access ──────────────────────────────────────────────────────────────
uint8_t labelGet(uint8_t port) {
    return (port < NUM_PACKS) ? _labels[port] : 0;
}

void labelSet(uint8_t port, uint8_t label) {
    if (port >= NUM_PACKS || label > NUM_LABELS) return;
    _labels[port] = label;
    saveLabels();
}

// Returns human-readable label: "3" if assigned, "P1" if not
void labelStr(uint8_t port, char *buf, size_t len) {
    uint8_t l = labelGet(port);
    if (l == 0) snprintf(buf, len, "P%u", port + 1);
    else        snprintf(buf, len, "%u", l);
}

// ── Session counters ──────────────────────────────────────────────────────────
uint16_t sessRideNext()   { return ++_sessRide; }
uint16_t sessChargeNext() { return ++_sessCharge; }

// ── Time ─────────────────────────────────────────────────────────────────────
bool timeIsSynced() { return _timeSynced; }

time_t timeNowSec() {
    if (!_rtcOk || !_timeSynced) return 0;
    return (time_t)_rtc.now().unixtime();
}

// Called from WiFiServer.ino /settime route (browser sends Date.now() in ms)
void timeSyncSet(int64_t browserEpochMs) {
    uint32_t epoch = (uint32_t)(browserEpochMs / 1000LL);
    if (_rtcOk) {
        _rtc.adjust(DateTime(epoch));
        _timeSynced = true;
        DateTime now = _rtc.now();
        Serial.printf("[RTC] set to %04u-%02u-%02u %02u:%02u:%02u\n",
                      now.year(), now.month(), now.day(),
                      now.hour(), now.minute(), now.second());
    } else {
        Serial.println("[RTC] no module — browser sync ignored (no storage)");
    }
}

// ── Filename builder ──────────────────────────────────────────────────────────
// type: 'R' (ride) or 'C' (charge)
// session: current session counter value
// seg: segment within session (1, 2, 3…)
// out: "/R_20260517_042_1.csv"  or  "/R_S042_1.csv" (NOSYNC)
void makeLogFilename(char *out, size_t outLen,
                     char type, uint16_t session, uint8_t seg) {
    if (_timeSynced) {
        DateTime now = _rtc.now();
        snprintf(out, outLen, "/%c_%04u%02u%02u_%03u_%u.csv",
                 type,
                 now.year(), now.month(), now.day(),
                 (unsigned)session, seg);
    } else {
        snprintf(out, outLen, "/%c_S%03u_%u.csv",
                 type, (unsigned)session, seg);
    }
}

// ── Timestamp string for CSV rows ─────────────────────────────────────────────
// buf must be at least 24 bytes
void fmtTimestamp(char *buf, size_t len) {
    if (_timeSynced && _rtcOk) {
        DateTime now = _rtc.now();
        snprintf(buf, len, "%04u-%02u-%02uT%02u:%02u:%02u",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
    } else {
        snprintf(buf, len, "T+%lus", millis() / 1000);
    }
}
