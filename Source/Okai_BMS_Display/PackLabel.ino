// PackLabel.ino — pack labels (1-8), time sync, session counters, filenames
//
// Time source priority:
//   1. DS3231 RTC module on GPIO11(SDA)/GPIO12(SCL)  [optional hardware]
//   2. Software offset persisted in /timesync.bin      [no hardware needed]
//
// Without DS3231: open the web dashboard once after each firmware flash
// (or power cycle with dead RTC battery) — JS auto-calls /settime and the
// offset is stored in flash. Drift ~10 s/day (ESP32 crystal) — fine for logs.
//
// With DS3231: set once ever (browser JS or /settime), RTC keeps time across
// power cycles with coin-cell backup. No per-session action needed.
//
// Library (optional): "RTClib" by Adafruit — only needed if DS3231 is fitted.
// If not installed, comment out #define USE_DS3231 below.

#define USE_DS3231   // comment this line out if RTClib is not installed

#ifdef USE_DS3231
  #include <Wire.h>
  #include <RTClib.h>
  static RTC_DS3231 _rtc;
  static bool       _rtcOk = false;
#endif

#include <LittleFS.h>

// ── State ─────────────────────────────────────────────────────────────────────
static bool     _timeSynced   = false;
static uint32_t _syncEpochSec = 0;  // wall-clock second at last sync
static uint32_t _syncMillisSec = 0; // millis()/1000 at last sync

static uint8_t  _labels[NUM_PACKS] = {0}; // 0=unassigned, 1-8=label
static uint16_t _sessRide   = 0;
static uint16_t _sessCharge = 0;

// ── Persistence ───────────────────────────────────────────────────────────────
struct TimeRecord { uint32_t epochSec; uint32_t millisSec; };

static void loadTimeFromFlash() {
    if (!fsReady) return;
    File f = LittleFS.open("/timesync.bin", "r");
    if (f && (size_t)f.size() >= sizeof(TimeRecord)) {
        TimeRecord tr;
        f.read((uint8_t*)&tr, sizeof(tr));
        f.close();
        _syncEpochSec  = tr.epochSec;
        _syncMillisSec = tr.millisSec;
        _timeSynced    = true;
        // Verify sanity: epoch must be after 2024-01-01
        if (_syncEpochSec < 1704067200UL) { _timeSynced = false; return; }
        uint32_t nowEst = _syncEpochSec + (millis()/1000 - _syncMillisSec);
        Serial.printf("[TIME] restored from flash: ~%lu (drift: %lus since sync)\n",
                      (unsigned long)nowEst,
                      (unsigned long)(millis()/1000 - _syncMillisSec));
    } else {
        if (f) f.close();
    }
}

static void saveTimeToFlash(uint32_t epochSec) {
    if (!fsReady) return;
    TimeRecord tr = { epochSec, millis() / 1000 };
    File f = LittleFS.open("/timesync.bin", "w", true);
    if (f) { f.write((uint8_t*)&tr, sizeof(tr)); f.close(); }
}

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
#ifdef USE_DS3231
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (_rtc.begin(&Wire)) {
        _rtcOk = true;
        if (!_rtc.lostPower()) {
            _timeSynced    = true;
            _syncEpochSec  = _rtc.now().unixtime();
            _syncMillisSec = millis() / 1000;
            DateTime n = _rtc.now();
            Serial.printf("[RTC] DS3231 ok: %04u-%02u-%02u %02u:%02u:%02u\n",
                          n.year(), n.month(), n.day(),
                          n.hour(), n.minute(), n.second());
        } else {
            Serial.println("[RTC] DS3231 found but lost power — needs /settime");
        }
    } else {
        Serial.println("[RTC] DS3231 not found — using flash-based time");
    }
#endif

    // Software fallback: try flash (used when DS3231 absent or not yet set)
    if (!_timeSynced) loadTimeFromFlash();

    loadLabels();
    loadSessions();
    Serial.printf("[LABEL] labels: %u %u %u %u  rides: %u  charges: %u  synced: %s\n",
                  _labels[0], _labels[1], _labels[2], _labels[3],
                  _sessRide, _sessCharge,
                  _timeSynced ? "yes" : "no");
}

// ── Time ─────────────────────────────────────────────────────────────────────
bool timeIsSynced() { return _timeSynced; }

time_t timeNowSec() {
    if (!_timeSynced) return 0;
#ifdef USE_DS3231
    if (_rtcOk) return (time_t)_rtc.now().unixtime();
#endif
    // Software: base epoch + elapsed millis since sync
    return (time_t)(_syncEpochSec + (millis()/1000 - _syncMillisSec));
}

// Called from /settime (browser JS Date.now() in milliseconds)
void timeSyncSet(int64_t browserEpochMs) {
    uint32_t epochSec = (uint32_t)(browserEpochMs / 1000LL);
    _syncEpochSec  = epochSec;
    _syncMillisSec = millis() / 1000;
    _timeSynced    = true;

#ifdef USE_DS3231
    if (_rtcOk) {
        _rtc.adjust(DateTime(epochSec));
        Serial.println("[RTC] DS3231 updated from browser");
    }
#endif
    // Always save to flash so it survives reboot even without DS3231
    saveTimeToFlash(epochSec);

    struct tm tm_info;
    time_t t = (time_t)epochSec;
    gmtime_r(&t, &tm_info);
    Serial.printf("[TIME] synced: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  tm_info.tm_year+1900, tm_info.tm_mon+1, tm_info.tm_mday,
                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
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

// "3" if assigned, "P1" if not
void labelStr(uint8_t port, char *buf, size_t len) {
    uint8_t l = labelGet(port);
    if (l == 0) snprintf(buf, len, "P%u", port + 1);
    else        snprintf(buf, len, "%u", l);
}

// ── Session counters ──────────────────────────────────────────────────────────
uint16_t sessRideNext()   { return ++_sessRide; }
uint16_t sessChargeNext() { return ++_sessCharge; }

// ── Filename builder ──────────────────────────────────────────────────────────
void makeLogFilename(char *out, size_t outLen,
                     char type, uint16_t session, uint8_t seg) {
    if (_timeSynced) {
        time_t t = timeNowSec();
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out, outLen, "/%c_%04d%02d%02d_%03u_%u.csv",
                 type,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 (unsigned)session, seg);
    } else {
        snprintf(out, outLen, "/%c_S%03u_%u.csv",
                 type, (unsigned)session, seg);
    }
}

// ── Timestamp string for CSV rows ─────────────────────────────────────────────
void fmtTimestamp(char *buf, size_t len) {
    if (_timeSynced) {
        time_t t = timeNowSec();
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d",
                 tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(buf, len, "T+%lus", millis() / 1000);
    }
}
