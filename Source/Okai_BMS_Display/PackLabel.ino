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
    if (!f) {
        // 2026-10-05 - F-7: this failure used to be silent, and the most likely cause of
        // it is a FULL filesystem. The counters then roll back to their last saved value
        // on the next boot and start handing out session numbers that already exist on
        // disk — so the next session overwrites an old one. Worth a loud line.
        Serial.println("[LABEL] saveSessions FAILED to open /sessions.bin - counters will "
                       "ROLL BACK on next boot and filenames may collide (filesystem full?)");
        return;
    }
    const size_t a = f.write((uint8_t*)&_sessRide,   2);
    const size_t b = f.write((uint8_t*)&_sessCharge, 2);
    f.close();
    if (a != 2 || b != 2) {
        Serial.printf("[LABEL] saveSessions SHORT WRITE (%u+%u of 4 bytes) - counters "
                      "will roll back on next boot\n", (unsigned)a, (unsigned)b);
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

    // 2026-10-09 - DRIFT GUARD. The dashboard carries <meta refresh 5> AND an inline
    // fetch('/settime'), so an open browser tab called this EVERY 5 SECONDS and each call
    // unconditionally erased a flash sector and wrote the DS3231: ~17,000 sector erases a
    // day from a tab nobody is looking at. The AP-client window re-arm added today is what
    // makes that indefinite rather than self-limiting after 60 s, so it is mine to fix.
    //
    // It also matters for SOP-038: a flash erase blocks, and the thing it blocks is the
    // keep-alive's own flash access path. Logging ranks below WiFi; a clock write that
    // nothing asked for ranks below everything.
    //
    // Accept a sync that moves the clock meaningfully, or the first one of a session.
    // Anything inside the window is the same tab telling us what we already know.
    const bool  firstSync = !_timeSynced;
    const int32_t driftSec = firstSync ? 0 : (int32_t)(epochSec - timeNowSec());
    const int32_t driftMag = driftSec < 0 ? -driftSec : driftSec;
    if (!firstSync && driftMag < TIME_RESYNC_MIN_DRIFT_SEC) {
        _syncEpochSec  = epochSec;        // keep RAM exact — costs nothing
        _syncMillisSec = millis() / 1000;
        _timeRestored  = false;
        return;                            // no flash write, no I2C write
    }

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

// "P3-6" — the PORT and the owner's PACK NUMBER together, which is the pairing he actually
// uses and the one thing the screens did not show.
//
// 2026-10-09, his words: "I do not see the label number. I see P1, which is the port, and I
// see the CYC, or the charge cycles, which I really don't care to see... that would be much
// more helpful than the CYC. That doesn't matter to me because I don't know how to read
// that." Correct on both counts — the cycle count is a diagnostic for the registry, not a
// thing a human reads off a screen while holding a battery, and the number written in white
// marker on the pack is the only identifier he has in his hand.
//
// "P3-?" when a pack is talking but the registry has not identified it. That state is real
// and must be visible: it is exactly when he needs to go and assign a number, and showing a
// bare "P3" would hide it.
// 2026-10-09 - SPACES AROUND THE DASH, at the owner's request: "When the numbers are too
// close together, they're harder to read." He is right and it costs two pixels of a line
// that has room: "P3 - 6" reads as a port and a pack, "P3-6" reads as one jumbled token.
void portPackStr(uint8_t port, char *buf, size_t len) {
    if (port >= NUM_PACKS) { snprintf(buf, len, "P?"); return; }
    if (!packs[port].valid) { snprintf(buf, len, "P%u", port + 1); return; }
    uint8_t n = labelGet(port);
    if (n) snprintf(buf, len, "P%u - %u", port + 1, n);
    else   snprintf(buf, len, "P%u - ?", port + 1);
}

// ── Session counters ──────────────────────────────────────────────────────────
// 2026-10-05 - F-3: cap raised 999 -> LOG_SESSION_MAX (9999). These counters CLAMP, they
// do not wrap, and at the clamp two separate things broke:
//   - every file in that stream read as session 999, which the free-space guard treats as
//     "the session in progress" and therefore refuses to prune — so the stream became
//     permanently unprunable and the filesystem filled anyway;
//   - two sessions at the clamp on one day produced an IDENTICAL filename, and openFile()
//     opens "w", truncating the earlier session to its header. In NOSYNC mode (no date in
//     the name) that collision was unconditional, every session, every boot.
// Warn well before the new cap so this is visible long before it bites again.
static void _sessWarnNearMax(const char* which, uint16_t v) {
    if (v >= LOG_SESSION_MAX - 10) {
        Serial.printf("[LABEL] WARNING %s session counter at %u of %u - at the cap, "
                      "filenames start colliding and logs stop being prunable\n",
                      which, (unsigned)v, (unsigned)LOG_SESSION_MAX);
    }
}

uint16_t sessRideNext() {
    if (_sessRide < LOG_SESSION_MAX) _sessRide++;
    _sessWarnNearMax("ride", _sessRide);
    return _sessRide;
}

uint16_t sessChargeNext() {
    if (_sessCharge < LOG_SESSION_MAX) _sessCharge++;
    _sessWarnNearMax("charge", _sessCharge);
    return _sessCharge;
}

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
