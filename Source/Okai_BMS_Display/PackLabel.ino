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
static uint32_t _syncMillisSec = 0; // millis()/1000 at last sync — THIS BOOT only
// 2026-07-26 - true when the epoch came from flash rather than a live browser
// sync. Time then runs forward correctly but sits behind reality by however long
// the board was powered off, so logs say "restored" instead of "synced".
static bool     _timeRestored = false;

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
        _timeSynced    = true;
        // Verify sanity: epoch must be after 2024-01-01
        if (_syncEpochSec < 1704067200UL) { _timeSynced = false; return; }

        // 2026-07-26 - RE-ANCHOR to this boot. Do NOT restore tr.millisSec.
        // WHAT WAS WRONG: millisSec is a millis() reading, which is meaningless
        // across a reboot because millis() restarts at zero. Restoring it made
        // timeNowSec() compute a NEGATIVE elapsed, which is clamped to 0 — so the
        // clock froze permanently at the instant of the last browser sync. Every
        // row of every log carried one identical timestamp; the 2026-07-25 logs
        // all read 2026-07-25T03:00:01 on every single line, across three separate
        // sessions. Anchoring to the current boot makes time advance again.
        _syncMillisSec = millis() / 1000;

        // We cannot know how long the board was powered off, so this clock is
        // behind reality until the browser syncs it. Flag it rather than lie.
        _timeRestored = true;
        Serial.printf("[TIME] restored from flash: %lu (STALE — board-off time is "
                      "unknown; open the dashboard to re-sync)\n",
                      (unsigned long)_syncEpochSec);
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
bool timeIsSynced()   { return _timeSynced; }
bool timeIsRestored() { return _timeRestored; }

time_t timeNowSec() {
    if (!_timeSynced) return 0;
#ifdef USE_DS3231
    if (_rtcOk) return (time_t)_rtc.now().unixtime();
#endif
    // Software: base epoch + elapsed millis since sync (cast prevents underflow at rollover)
    int32_t elapsed = (int32_t)((uint32_t)(millis()/1000) - _syncMillisSec);
    return (time_t)(_syncEpochSec + (uint32_t)(elapsed > 0 ? elapsed : 0));
}

// Called from /settime (browser JS Date.now() in milliseconds)
void timeSyncSet(int64_t browserEpochMs) {
    uint32_t epochSec = (uint32_t)(browserEpochMs / 1000LL);
    _syncEpochSec  = epochSec;
    _syncMillisSec = millis() / 1000;
    _timeSynced    = true;
    _timeRestored  = false;   // a live browser sync clears the stale flag

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
// 2026-10 - The number a human sees is now bound to the PHYSICAL PACK, not the port.
// PackRegistry assigns every pack an autoNum at first registration and stores it in
// that pack's /packs/CYC-XXXX.dat, so the number follows the battery into whatever
// port it is plugged into — no user action, and it can no longer go stale the moment
// packs are swapped, which is exactly why the old port-bound labels went unused.
//
// /labels.bin is now LEGACY. It is still read at boot and used as a fallback for a
// port whose pack has not been identified yet (first few seconds after plug-in, or a
// pack the registry cannot fingerprint). It is never written again.
uint8_t labelGet(uint8_t port) {
    if (port >= NUM_PACKS) return 0;
    uint8_t n = packRegistryNumber(port);     // pack-bound: override ?: autoNum
    if (n) return n;
    return _labels[port];                     // legacy port-bound fallback
}

void labelSet(uint8_t port, uint8_t label) {
    if (port >= NUM_PACKS || label > NUM_LABELS) return;
    packRegistrySetLabel(port, label);         // no-op if the pack is unidentified
    _labels[port] = label;                     // keep the legacy fallback coherent
    // saveLabels() must still run. For an UNIDENTIFIED pack — the exact fallback case
    // this legacy map exists for — packRegistrySetLabel stores nothing, so without this
    // the owner's choice lived only in RAM and vanished on the next reboot, while
    // /labels.bin kept serving whatever stale port-bound number was already in flash.
    saveLabels();
}

// "3" if assigned, "P1" if not
void labelStr(uint8_t port, char *buf, size_t len) {
    uint8_t l = labelGet(port);
    if (l == 0) snprintf(buf, len, "P%u", port + 1);
    else        snprintf(buf, len, "%u", l);
}

// ── Session counters ──────────────────────────────────────────────────────────
uint16_t sessRideNext()   { if (_sessRide   < 999) _sessRide++;   return _sessRide; }
uint16_t sessChargeNext() { if (_sessCharge < 999) _sessCharge++; return _sessCharge; }

// ── Filename builder ──────────────────────────────────────────────────────────
void makeLogFilename(char *out, size_t outLen,
                     char type, uint16_t session, uint8_t seg) {
    // 2026-07-26 - Owner request: name the device, then say plainly what the log
    // is, because these files sit alongside logs from other projects and "R_"
    // means nothing once they are off the device.
    //   /Okai_RIDE_20260726_057_1.csv
    //   /Okai_CHRG_20260726_012_1.csv
    // RIDE and CHRG are both 4 chars so the names align in the file list.
    // Longest form is 29 chars + NUL; callers pass a 44-byte buffer.
    const char *kind = (type == 'R') ? "RIDE" : "CHRG";

    if (_timeSynced) {
        time_t t = timeNowSec();
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out, outLen, "/Okai_%s_%04d%02d%02d_%03u_%u.csv",
                 kind,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 (unsigned)session, seg);
    } else {
        snprintf(out, outLen, "/Okai_%s_S%03u_%u.csv",
                 kind, (unsigned)session, seg);
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
