// PackRegistry.ino — pack UUID registry and lifetime stats
//
// UUID = regCYC (cycle count at first registration — never changes)
// Identification at plug-in: scan /packs/, match regCYC ≤ current ≤ regCYC+TOLERANCE
//                            AND maxSocAtReg == pack.maxSoc()  (tiebreaker)
// Storage: /packs/CYC-XXXX.dat  one per physical pack, plain key=value text

#include <LittleFS.h>

PackRecord packRec[NUM_PACKS];

// ── File helpers ──────────────────────────────────────────────────────────────

static void _cycFilename(char* out, size_t len, uint16_t regCYC) {
    snprintf(out, len, "/packs/CYC-%04u.dat", (unsigned)regCYC);
}

// Read a uint16 from "KEY=VALUE\n" text blob
static bool _rdU16(const char* buf, const char* key, uint16_t* out) {
    char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(buf, pat);
    if (!p) return false;
    *out = (uint16_t)atoi(p + strlen(pat));
    return true;
}

static bool _rdU8(const char* buf, const char* key, uint8_t* out) {
    uint16_t v = 0;
    if (!_rdU16(buf, key, &v)) return false;
    *out = (uint8_t)v;
    return true;
}

static bool _rdFloat(const char* buf, const char* key, float* out) {
    char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(buf, pat);
    if (!p) return false;
    *out = atof(p + strlen(pat));
    return true;
}

static bool _rdStr(const char* buf, const char* key, char* out, size_t outLen) {
    char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(buf, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < outLen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return true;
}

// Parse comma-separated uint16 array
static uint8_t _rdU16Arr(const char* buf, const char* key,
                          uint16_t* arr, uint8_t maxLen) {
    char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    uint8_t n = 0;
    while (n < maxLen && *p && *p != '\n' && *p != '\r') {
        arr[n++] = (uint16_t)atoi(p);
        while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
        if (*p == ',') p++;
    }
    return n;
}

static uint8_t _rdU8Arr(const char* buf, const char* key,
                         uint8_t* arr, uint8_t maxLen) {
    char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    uint8_t n = 0;
    while (n < maxLen && *p && *p != '\n' && *p != '\r') {
        arr[n++] = (uint8_t)atoi(p);
        while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
        if (*p == ',') p++;
    }
    return n;
}

// ── Load / save ───────────────────────────────────────────────────────────────

static bool _loadRecord(uint8_t port, const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    size_t sz = (size_t)f.size();
    if (sz == 0 || sz > 512) { f.close(); return false; }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) { f.close(); return false; }
    f.read((uint8_t*)buf, sz);
    buf[sz] = '\0';
    f.close();

    PackRecord& r = packRec[port];
    memset(&r, 0, sizeof(r));

    _rdU16  (buf, "regCYC",           &r.regCYC);
    _rdU8   (buf, "maxSocAtReg",      &r.maxSocAtReg);
    _rdStr  (buf, "firstSeen",         r.firstSeen, sizeof(r.firstSeen));
    _rdU16  (buf, "currentCycles",    &r.currentCycles);
    _rdU16  (buf, "sessions",         &r.sessions);
    _rdFloat(buf, "totalWhCharged",   &r.totalWhCharged);
    _rdFloat(buf, "totalWhDischarged",&r.totalWhDischarged);
    _rdU16Arr(buf, "spreadHistory",    r.spreadHistory, PACK_HISTORY_LEN);
    _rdU8Arr (buf, "sohHistory",       r.sohHistory,    PACK_HISTORY_LEN);
    _rdU8   (buf, "historyLen",       &r.historyLen);

    snprintf(r.cycID, sizeof(r.cycID), "CYC-%u", (unsigned)r.regCYC);
    r.known = (r.regCYC > 0);

    free(buf);
    return r.known;
}

static void _saveRecord(uint8_t port) {
    if (!fsReady) return;
    PackRecord& r = packRec[port];
    if (!r.known) return;

    char path[32]; _cycFilename(path, sizeof(path), r.regCYC);
    File f = LittleFS.open(path, "w", true);
    if (!f) {
        Serial.printf("[REG] save failed: %s\n", path);
        return;
    }

    f.printf("regCYC=%u\n",           (unsigned)r.regCYC);
    f.printf("maxSocAtReg=%u\n",      (unsigned)r.maxSocAtReg);
    f.printf("firstSeen=%s\n",        r.firstSeen);
    f.printf("currentCycles=%u\n",    (unsigned)r.currentCycles);
    f.printf("sessions=%u\n",         (unsigned)r.sessions);
    f.printf("totalWhCharged=%.2f\n", r.totalWhCharged);
    f.printf("totalWhDischarged=%.2f\n", r.totalWhDischarged);
    f.printf("historyLen=%u\n",       (unsigned)r.historyLen);

    f.print("spreadHistory=");
    for (uint8_t i = 0; i < r.historyLen; i++) {
        if (i) f.print(',');
        f.print((unsigned)r.spreadHistory[i]);
    }
    f.println();

    f.print("sohHistory=");
    for (uint8_t i = 0; i < r.historyLen; i++) {
        if (i) f.print(',');
        f.print((unsigned)r.sohHistory[i]);
    }
    f.println();

    f.close();
    Serial.printf("[REG] saved %s  cyc=%u  Wh+%.1f/%.1f  sess=%u\n",
                  path, (unsigned)r.currentCycles,
                  r.totalWhCharged, r.totalWhDischarged, (unsigned)r.sessions);
}

// ── Public API ────────────────────────────────────────────────────────────────

void packRegistryInit() {
    memset(packRec, 0, sizeof(packRec));
    if (!fsReady) return;
    if (!LittleFS.exists("/packs")) LittleFS.mkdir("/packs");
    Serial.println("[REG] ready — /packs directory ensured");
}

// Called from UART.ino the moment a port goes from invalid → valid.
void packRegistryIdentify(uint8_t port) {
    if (!fsReady || port >= NUM_PACKS) return;

    uint16_t curCyc = packs[port].cycles;
    uint8_t  curMax = packs[port].maxSoc;
    packRec[port].known = false;

    // Scan all .dat files in /packs/
    File dir = LittleFS.open("/packs");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        packRegistryRegister(port);
        return;
    }

    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();   // base name only, e.g. "CYC-0054.dat"
        f.close();

        char path[32];
        snprintf(path, sizeof(path), "/packs/%s", name);

        // Quick-read just regCYC and maxSocAtReg from file
        File rf = LittleFS.open(path, "r");
        if (rf) {
            size_t sz = (size_t)rf.size();
            char* buf = (sz > 0 && sz < 256) ? (char*)malloc(sz + 1) : nullptr;
            if (buf) {
                rf.read((uint8_t*)buf, sz);
                buf[sz] = '\0';
                uint16_t rCYC = 0; uint8_t rMax = 0;
                _rdU16(buf, "regCYC", &rCYC);
                _rdU8 (buf, "maxSocAtReg", &rMax);
                free(buf);

                bool cycMatch = (curCyc >= rCYC) &&
                                (curCyc - rCYC <= PACK_CYC_TOLERANCE);
                bool maxMatch = (rMax == curMax);

                if (cycMatch && maxMatch) {
                    rf.close();
                    dir.close();
                    _loadRecord(port, path);
                    packRec[port].currentCycles = curCyc;
                    Serial.printf("[REG] port%u identified: %s  cyc=%u→%u\n",
                                  port+1, packRec[port].cycID,
                                  (unsigned)packRec[port].regCYC,
                                  (unsigned)curCyc);
                    return;
                }
            }
            rf.close();
        }

        f = dir.openNextFile();
    }
    dir.close();

    // No match — register as new pack
    packRegistryRegister(port);
}

// Called (internally and from UART) when a pack is seen for the first time.
void packRegistryRegister(uint8_t port) {
    if (port >= NUM_PACKS) return;

    PackRecord& r = packRec[port];
    memset(&r, 0, sizeof(r));

    r.regCYC       = packs[port].cycles;
    r.maxSocAtReg  = packs[port].maxSoc;
    r.currentCycles = r.regCYC;
    r.sessions     = 0;
    r.historyLen   = 0;

    // Date string for firstSeen
    if (timeIsSynced()) {
        time_t t = timeNowSec();
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(r.firstSeen, sizeof(r.firstSeen), "%04d-%02d-%02d",
                 tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
    } else {
        strcpy(r.firstSeen, "unknown");
    }

    snprintf(r.cycID, sizeof(r.cycID), "CYC-%u", (unsigned)r.regCYC);
    r.known = true;

    _saveRecord(port);
    Serial.printf("[REG] port%u registered NEW pack: %s  maxSoc=%u%%\n",
                  port+1, r.cycID, (unsigned)r.maxSocAtReg);
}

// Called from Logger.ino at session close.
// Accumulates Wh, snapshots spread + SoH, increments session counter, saves.
void packRegistrySessionUpdate(uint8_t port) {
    if (port >= NUM_PACKS || !fsReady) return;
    PackRecord& r = packRec[port];
    if (!r.known) return;

    // Accumulate Wh then zero session accumulators so next session starts clean
    r.totalWhCharged    += packs[port].whIn;
    r.totalWhDischarged += packs[port].whOut;
    packs[port].whIn     = 0.0f;
    packs[port].whOut    = 0.0f;

    // Update cycle count (may have incremented if a full charge happened)
    r.currentCycles = packs[port].cycles;

    // Append to rolling history (shift left when full)
    uint16_t sp  = (uint16_t)((packs[port].cellHigh - packs[port].cellLow) * 1000.0f + 0.5f);
    uint8_t  soh = packs[port].maxSoc;

    if (r.historyLen < PACK_HISTORY_LEN) {
        r.spreadHistory[r.historyLen] = sp;
        r.sohHistory   [r.historyLen] = soh;
        r.historyLen++;
    } else {
        memmove(r.spreadHistory, r.spreadHistory + 1,
                (PACK_HISTORY_LEN - 1) * sizeof(uint16_t));
        memmove(r.sohHistory,    r.sohHistory    + 1,
                (PACK_HISTORY_LEN - 1) * sizeof(uint8_t));
        r.spreadHistory[PACK_HISTORY_LEN - 1] = sp;
        r.sohHistory   [PACK_HISTORY_LEN - 1] = soh;
    }

    r.sessions++;
    _saveRecord(port);
}

const PackRecord* packRegGet(uint8_t port) {
    return (port < NUM_PACKS) ? &packRec[port] : nullptr;
}
