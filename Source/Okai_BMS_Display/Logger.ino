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
    if (millis() < _rideUntilMs) return LOG_RIDE;
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
    _logFile.printf("# OkaiBMS %s | %s | Start: %s | RTC: %s\n",
                    FW_VERSION,
                    _curType == 'R' ? "RIDE" : "CHARGE",
                    ts,
                    timeIsSynced() ? "synced" : "NOSYNC");

    _logFile.print("# Packs at session start:");
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        _logFile.printf("  port%u=L%s(%ucyc,%u%%)", i+1, lbl,
                        packs[i].cycles, packs[i].soc);
    }
    _logFile.println();
    _logFile.printf("# Segment: %u  MaxBytes: %lu\n", _segment, LOG_MAX_FILE_BYTES);
    _logFile.println("Timestamp,UpSec,Label,Port,SOC,Voltage_V,Current_A,"
                     "Power_W,CellHigh_V,CellLow_V,Delta_mV,MaxTemp_C,Cycles");
    _fileSizeBytes = (uint32_t)_logFile.size();
}

static void openFile() {
    char fname[44];
    makeLogFilename(fname, sizeof(fname), _curType, _curSession, _segment);
    _logFile = LittleFS.open(fname, "w", true);
    if (!_logFile) {
        Serial.printf("[LOG] open failed: %s\n", fname);
        return;
    }
    _fileOpen = true;
    writeFileHeader();
    _logFile.flush();
    Serial.printf("[LOG] opened %s\n", fname);
}

static void closeFile() {
    if (!_fileOpen) return;
    for (uint8_t i = 0; i < NUM_PACKS; i++)
        if (packs[i].valid) packRegistrySessionUpdate(i);
    _logFile.println("# EOF");
    _logFile.close();
    _fileOpen = false;
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
    if (!LittleFS.begin(true)) {
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
    if (millis() - _lastLogMs < modeInterval()) return;
    _lastLogMs = millis();
    if (!_fileOpen) return;

    char ts[24]; fmtTimestamp(ts, sizeof(ts));
    uint32_t upSec = millis() / 1000;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        float delta  = packs[i].cellHigh - packs[i].cellLow;
        float powerW = packs[i].voltage * packs[i].current;

        char row[128];
        int n = snprintf(row, sizeof(row),
                 "%s,%lu,%s,%u,%u,%.3f,%+.3f,%.1f,%.3f,%.3f,%u,%u,%u",
                 ts, (unsigned long)upSec, lbl, (unsigned)(i+1),
                 (unsigned)packs[i].soc,
                 packs[i].voltage, packs[i].current, powerW,
                 packs[i].cellHigh, packs[i].cellLow,
                 (unsigned)(delta * 1000.0f + 0.5f),
                 (unsigned)packs[i].maxTemp,
                 (unsigned)packs[i].cycles);
        _logFile.println(row);
        _fileSizeBytes += (uint32_t)(n + 1);
    }
    _logFile.flush();

    if (_fileSizeBytes >= LOG_MAX_FILE_BYTES) rollSegment();
}
