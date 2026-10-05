// Logger.ino — smart two-stream CSV logging to LittleFS
//
// RIDE stream  (R_*): opens when any pack discharges > LOG_RIDE_THRESHOLD_A
//                     logs every LOG_RIDE_INTERVAL_MS (5 s)
//                     closes after LOG_RIDE_HYSTERESIS_MS of no discharge (2 min)
//
// CHARGE stream (C_*): opens when any pack has chargerDetected
//                      logs every LOG_CHARGE_INTERVAL_MS (30 s)
//                      closes immediately when charger removed
//
// Each file is capped at LOG_MAX_FILE_BYTES (256 KB); rolls to next segment.
// Filenames: R_20260517_042_1.csv  or  R_S042_1.csv (if RTC not set)
//
// PackLabel.ino provides: labelStr(), fmtTimestamp(), makeLogFilename(),
//                         sessRideNext(), sessChargeNext(), saveSessions()

#include <LittleFS.h>

bool fsReady = false;

static LogMode  _mode          = LOG_IDLE;
static File     _logFile;
static bool     _fileOpen      = false;
static uint32_t _lastLogMs     = 0;
static uint32_t _fileSizeBytes = 0;
static uint8_t  _segment       = 1;
static uint16_t _curSession    = 0;
static char     _curType       = 'R';
static uint32_t _rideUntilMs  = 0;   // hysteresis end time

// 2026-07-26 - Crash-safe logging.
// WHAT WAS WRONG: flush() pushes row data down, but LittleFS only commits a
// file's DIRECTORY ENTRY (its size) on sync/close. Lose power mid-session and the
// file can list as 0 KB even though rows were written — which is exactly what the
// owner saw after the board browned out overnight: no new logs "with any
// kilobytes" at the bottom of the page.
// THE FIX: close and reopen in APPEND mode on a timer. Closing is the operation
// that forces the metadata commit, so worst case on power loss is one interval.
// Must be "a" and never "w" — "w" truncates and would erase the session.
#define LOG_CHECKPOINT_MS  60000UL   // commit to flash at least once a minute
static uint32_t _lastCheckpointMs = 0;
static char     _curFileName[44]  = {0};

// 2026-09-25 - CSV format version, printed as "FMT n" in the first header line.
// 1 = original 16-column row, no FMT token in the header at all.
// 2 = Age_ms + Stale appended, plus "#### PACK LOST / BACK" marker lines mid-file.
// A file with no FMT token is format 1. Columns are only ever APPENDED, so a reader
// that indexes by position keeps working across versions.
// 3 (2026-10): appended MaxSoC, CapacityMah, TempAvg_C, TempFET_C, TempMCU_C, ChgState,
//              ChgAct. Columns 1-18 are byte-identical to FMT 2, so older readers that
//              index by position keep working on FMT 3 files.
#define LOG_FORMAT_VERSION  3

// 2026-09-25 - Stale-frame marking.
// WHAT WAS WRONG: when a pack stops transmitting, packs[] keeps its LAST received
// frame until UART.ino times it out (10 s of silence), so the logger re-wrote that
// dead frame every interval as if it were a fresh measurement. In the 2026-07-26
// dying-pack logs that produced byte-identical repeated rows and physically
// impossible samples — port 1 logged at +30.4 A while port 2 read -31.6 A at the
// same UpSec — and it hid the exact event being hunted. See
// docs/2026-09-25-pack-dropout-analysis.md §3.3 and §6.
// THE FIX: remember which frame each row was built from. If the frame timestamp has
// not moved since that pack's previous row, the row is an echo, not a measurement.
// Stale=1 says so outright; Age_ms says how stale without the reader having to know
// the log interval (5 s riding vs 30 s charging).
static uint32_t _rowFrameMs[NUM_PACKS] = {0};   // lastUpdateMs behind this pack's previous row

// 2026-09-25 - Pack dropout / rejoin markers.
// WHAT WAS WRONG: the pack set was recorded only in the session header, so a pack
// going silent mid-ride left nothing behind but missing rows. The reader had to
// infer the event and could not date it closer than one log interval.
// THE FIX: watch packs[].valid every loop() pass and write one marker line on each
// edge. Checked every pass, not on the log interval, so the marker carries the
// moment it actually happened.
static bool     _wasUp[NUM_PACKS]   = {false};  // packs[].valid at the previous edge check
static uint32_t _lostAtMs[NUM_PACKS] = {0};     // when this port last went silent (0 = never, this file)

extern volatile uint32_t g_hbCount;   // Heartbeat.ino — total keep-alives sent (read-only here)

// Exposed for WiFiServer.ino so "Delete all" can skip the file being written.
const char *loggerActiveFile() { return _fileOpen ? _curFileName : ""; }

// ── Mode detection ────────────────────────────────────────────────────────────
static LogMode detectMode() {
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (packs[i].valid && packs[i].chargerDetected) return LOG_CHARGE;
    }
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (packs[i].valid && packs[i].current < -LOG_RIDE_THRESHOLD_A) {
            _rideUntilMs = millis() + LOG_RIDE_HYSTERESIS_MS;
        }
    }
    if ((int32_t)(millis() - _rideUntilMs) < 0) return LOG_RIDE;
    return LOG_IDLE;
}

LogMode logCurrentMode() { return _mode; }

static uint32_t modeInterval() {
    if (_mode == LOG_RIDE)   return LOG_RIDE_INTERVAL_MS;
    if (_mode == LOG_CHARGE) return LOG_CHARGE_INTERVAL_MS;
    return 0xFFFFFFFF;
}

// ── File management ───────────────────────────────────────────────────────────
static void writeFileHeader() {
    char ts[24]; fmtTimestamp(ts, sizeof(ts));
    _logFile.printf("# OkaiBMS %s | FMT %u | %s | Start: %s | RTC: %s\n",
                    FW_VERSION,
                    (unsigned)LOG_FORMAT_VERSION,
                    _curType == 'R' ? "RIDE" : "CHARGE",
                    ts,
                    !timeIsSynced()   ? "NOSYNC"
                    : timeIsRestored() ? "restored (stale — re-sync from dashboard)"
                                       : "synced");

    _logFile.print("# Packs at session start:");
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        _logFile.printf("  port%u=L%s(%ucyc,%u%%)", i+1, lbl,
                        packs[i].cycles, packs[i].soc);
    }
    _logFile.println();
    _logFile.printf("# Segment: %u  MaxBytes: %lu\n", _segment, LOG_MAX_FILE_BYTES);
    // 2026-09-25 - Legend, so the file explains itself when it is read on a phone at
    // the beach with no docs to hand.
    _logFile.println("# Stale=1: row REPEATS the previous frame (pack stopped talking) "
                     "- not a measurement. Age_ms: age of that frame. "
                     "Lines starting #### mark a pack dropping out or rejoining.");
    // 2026-07-26 - Rest column added. Delta_mV is still logged on EVERY row (raw
    // data is never thrown away) but only Rest=1 rows carry a health meaning, so
    // analysis can filter on it instead of guessing from Current_A.
    // 2026-10 - FMT 3 appends seven health columns. APPENDED, never inserted: a FMT 2
    // reader indexing the first 18 columns by position still parses these files.
    // MaxSoC is the one that matters most and was simply missing — it is the BMS's own
    // state-of-health number. TempFET_C is where a tiring pack shows thermal stress
    // first. CapacityMah, the two other temperatures and the charger bytes were all
    // decoded by the library and discarded.
    _logFile.println("Timestamp,UpSec,Label,Port,SOC,Voltage_V,Current_A,"
                     "Power_W,CellHigh_V,CellLow_V,Delta_mV,Rest,MaxTemp_C,Cycles,Status,Warn,"
                     "Age_ms,Stale,"
                     "MaxSoC,CapacityMah,TempAvg_C,TempFET_C,TempMCU_C,ChgState,ChgAct");
    _fileSizeBytes = (uint32_t)_logFile.size();
}

static void openFile() {
    makeLogFilename(_curFileName, sizeof(_curFileName), _curType, _curSession, _segment);
    _logFile = LittleFS.open(_curFileName, "w", true);
    if (!_logFile) {
        Serial.printf("[LOG] open failed: %s\n", _curFileName);
        _curFileName[0] = '\0';
        return;
    }
    _fileOpen = true;
    // 2026-09-25 - Re-seed the per-file edge and stale state. The packs listed in the
    // header are the baseline for THIS file, so they must not also produce a "PACK
    // BACK" marker on the first pass, and the first row of a new segment has no
    // previous row to be an echo of.
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        _wasUp[i]      = packs[i].valid;
        _rowFrameMs[i] = 0;
        _lostAtMs[i]   = 0;
    }
    writeFileHeader();
    _logFile.flush();

    // Commit the header immediately so the file has a real size on disk from the
    // first second, not only once the first checkpoint lands.
    _logFile.close();
    _logFile = LittleFS.open(_curFileName, "a");
    if (!_logFile) {
        Serial.printf("[LOG] reopen failed: %s\n", _curFileName);
        _fileOpen = false;
        _curFileName[0] = '\0';
        return;
    }
    _lastCheckpointMs = millis();
    Serial.printf("[LOG] opened %s\n", _curFileName);
}

// Force LittleFS to commit the directory entry without ending the session.
static void checkpointFile() {
    if (!_fileOpen) return;
    _logFile.flush();
    _logFile.close();
    _logFile = LittleFS.open(_curFileName, "a");
    if (!_logFile) {
        // Reopen failed — stop claiming the file is open rather than writing into
        // a dead handle. The rows already committed stay on disk and readable.
        Serial.printf("[LOG] checkpoint reopen FAILED: %s\n", _curFileName);
        _fileOpen = false;
        return;
    }
    _lastCheckpointMs = millis();
}

// ── Pack dropout / rejoin markers ─────────────────────────────────────────────
// 2026-09-25 - One line per valid->invalid and invalid->valid transition.
//
// WHY A "#" LINE: the header is already four "#" lines and every file ends on
// "# EOF", so anything that reads these logs must already skip "#" — a marker
// cannot break a parser that works today. Four hashes and shouted words so the line
// jumps out of a wall of numbers when the log is skimmed on a phone.
//
// WHY "lastframe=" IS NOT THE SAME AS "up=": UART.ino only clears packs[].valid
// after 10 s of silence, so the marker is written ~10 s AFTER the pack actually went
// quiet. Both numbers are printed: "up=" is when the firmware gave up, "lastframe="
// is the UpSec of the last row that carried real telemetry for that port.
//
// WHY hb= AND hbAge=: they come from the Core-1 keep-alive task, read-only — nothing
// here writes or clears them, so diagLoop()'s 3 s counters are untouched. hbAge is
// the age of the last keep-alive at the instant the pack went quiet: small means the
// keep-alive was beating and the pack went silent anyway; large means the keep-alive
// slipped and the pack slept, which is a different fault. hb= falling between two
// markers proves the board rebooted (the counter starts again from 0).
static void writePackEdges() {
    const uint32_t now   = millis();
    const uint32_t hbAge = now - g_hbLastMs;
    bool wrote = false;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        const bool up = packs[i].valid;
        if (up == _wasUp[i]) continue;
        _wasUp[i] = up;

        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        char line[176];
        int  n;

        if (!up) {
            _lostAtMs[i] = now;
            n = snprintf(line, sizeof(line),
                    "#### PACK LOST  up=%lus port=%u L%s lastframe=up%lus(%.1fs ago) "
                    "%.2fV %+.1fA SOC=%u%% hb=%lu hbAge=%.1fs",
                    (unsigned long)(now / 1000), (unsigned)(i + 1), lbl,
                    (unsigned long)(packs[i].lastUpdateMs / 1000),
                    (now - packs[i].lastUpdateMs) / 1000.0f,
                    packs[i].voltage, packs[i].current, (unsigned)packs[i].soc,
                    (unsigned long)g_hbCount, hbAge / 1000.0f);
        } else {
            char since[20];
            if (_lostAtMs[i]) snprintf(since, sizeof(since), "silent=%lus",
                                       (unsigned long)((now - _lostAtMs[i]) / 1000));
            else              snprintf(since, sizeof(since), "first-seen");
            n = snprintf(line, sizeof(line),
                    "#### PACK BACK  up=%lus port=%u L%s %s "
                    "%.2fV %+.1fA SOC=%u%% hb=%lu hbAge=%.1fs",
                    (unsigned long)(now / 1000), (unsigned)(i + 1), lbl, since,
                    packs[i].voltage, packs[i].current, (unsigned)packs[i].soc,
                    (unsigned long)g_hbCount, hbAge / 1000.0f);
        }
        _logFile.println(line);
        _fileSizeBytes += (uint32_t)(n + 1);
        wrote = true;
    }

    // A dropout is the moment right before the board may lose power, which is exactly
    // when an uncommitted directory entry loses the whole file. Commit now instead of
    // waiting up to LOG_CHECKPOINT_MS. Edges are rate-limited by physics — valid only
    // clears after 10 s of silence — so this cannot run away.
    if (wrote) checkpointFile();
}

static void closeFile() {
    if (!_fileOpen) return;
    for (uint8_t i = 0; i < NUM_PACKS; i++)
        if (packs[i].valid) packRegistrySessionUpdate(i);
    _logFile.println("# EOF");
    _logFile.close();
    _fileOpen = false;
    _curFileName[0] = '\0';
    Serial.printf("[LOG] closed (mode=%u seg=%u bytes=%lu)\n",
                  _mode, _segment, _fileSizeBytes);
}

static void rollSegment() {
    closeFile();
    _segment++;
    openFile();
}

static void enterMode(LogMode newMode) {
    if (_mode != LOG_IDLE) closeFile();
    _mode     = newMode;
    _segment  = 1;
    _curType  = (newMode == LOG_RIDE) ? 'R' : 'C';
    _curSession = (newMode == LOG_RIDE) ? sessRideNext() : sessChargeNext();
    saveSessions();
    openFile();
}

static void exitToIdle() {
    closeFile();
    _mode = LOG_IDLE;
}

// ── Init ─────────────────────────────────────────────────────────────────────
void loggerInit() {
    if (!LittleFS.begin(true, "/littlefs", 10, "ffat")) {
        Serial.println("[LOG] LittleFS mount failed");
        return;
    }
    fsReady = true;
    size_t used = LittleFS.usedBytes(), total = LittleFS.totalBytes();
    Serial.printf("[LOG] LittleFS ready  %u / %u bytes used\n", used, total);
    // Session counters are loaded in packlabelInit() which runs first
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loggerLoop() {
    if (!fsReady) return;

    LogMode newMode = detectMode();

    if (newMode != _mode) {
        if (newMode == LOG_IDLE) exitToIdle();
        else                     enterMode(newMode);
        return;
    }

    if (_mode == LOG_IDLE) return;

    // Commit to flash on its own clock, independent of the sample interval — a
    // CHARGE session only samples every 30 s, and an uncommitted file is exactly
    // what a power cut destroys.
    if (millis() - _lastCheckpointMs >= LOG_CHECKPOINT_MS) checkpointFile();

    // 2026-09-25 - Dropout markers are evaluated EVERY pass, before the interval
    // gate, so a transition is dated to the loop() iteration it happened on instead
    // of being rounded to the next sample. Cost when nothing changed: NUM_PACKS bool
    // compares. Nothing here touches the Core-1 keep-alive task.
    if (_fileOpen) writePackEdges();

    if (millis() - _lastLogMs < modeInterval()) return;
    _lastLogMs = millis();
    if (!_fileOpen) return;

    char ts[24]; fmtTimestamp(ts, sizeof(ts));
    uint32_t nowMs = millis();
    uint32_t upSec = nowMs / 1000;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        float delta  = packs[i].cellHigh - packs[i].cellLow;
        float powerW = packs[i].voltage * packs[i].current;

        // Decode BMS status flags so warnings are captured in the LOG, not just on-screen.
        // Status = raw status byte (lossless). Warn = human summary of genuine alerts.
        // 2026-07-26 - DELTA_* is emitted on REST samples only. Under load the
        // spread is dominated by internal-resistance differences: pack #3, the
        // healthiest in the fleet, logged 160 mV at -9.7 A and 45 mV thirty
        // seconds later at rest. Flagging the loaded sample was pure noise.
        char warn[24]; warn[0] = '\0';
        if (packs[i].rawStatus & 0x10)  strcat(warn, "CELL_UV;");      // bit4
        if (packs[i].atRest) {
            if (delta >= CELL_DELTA_POOR_V)       strcat(warn, "DELTA_POOR;");
            else if (delta >= CELL_DELTA_WARN_V)  strcat(warn, "DELTA_WARN;");
        }
        if (!warn[0]) { warn[0] = '-'; warn[1] = '\0'; }

        // 2026-09-25 - Age_ms / Stale. Stale=1 means this pack's frame timestamp has
        // not moved since its previous row, i.e. every telemetry field below is a
        // repeat of that row and NOT a measurement. Age_ms is the same fact as a
        // number, and survives the reader not knowing which interval was in force.
        const uint32_t ageMs = nowMs - packs[i].lastUpdateMs;
        const unsigned stale = (packs[i].lastUpdateMs == _rowFrameMs[i]) ? 1u : 0u;
        _rowFrameMs[i] = packs[i].lastUpdateMs;

        char row[256];   // grown from 176 for the FMT 3 health columns
        int n = snprintf(row, sizeof(row),
                 "%s,%lu,%s,%u,%u,%.3f,%+.3f,%.1f,%.3f,%.3f,%u,%u,%u,%u,0x%02X,%s,%lu,%u"
                 ",%u,%u,%u,%u,%u,0x%02X,%u",
                 ts, (unsigned long)upSec, lbl, (unsigned)(i+1),
                 (unsigned)packs[i].soc,
                 packs[i].voltage, packs[i].current, powerW,
                 packs[i].cellHigh, packs[i].cellLow,
                 (unsigned)(delta * 1000.0f + 0.5f),
                 packs[i].atRest ? 1u : 0u,
                 (unsigned)packs[i].maxTemp,
                 (unsigned)packs[i].cycles,
                 (unsigned)packs[i].rawStatus, warn,
                 (unsigned long)ageMs, stale,
                 // FMT 3 health columns
                 (unsigned)packs[i].maxSoc,
                 (unsigned)packs[i].capacityMah,
                 (unsigned)packs[i].tempAvg,
                 (unsigned)packs[i].tempFet,
                 (unsigned)packs[i].tempMcu,
                 (unsigned)packs[i].chargerStateRaw,
                 packs[i].chargerActive ? 1u : 0u);
        _logFile.println(row);
        _fileSizeBytes += (uint32_t)(n + 1);
    }
    _logFile.flush();

    if (_fileSizeBytes >= LOG_MAX_FILE_BYTES) rollSegment();
}

// Called before deep sleep — flushes + closes the active log file cleanly.
void loggerShutdown() {
    closeFile();
    if (fsReady) {
        LittleFS.end();
        fsReady = false;
    }
}
