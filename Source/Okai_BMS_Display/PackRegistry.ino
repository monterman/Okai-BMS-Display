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

// 2026-10-05 - Takes a PackRecord by reference rather than a port index, so a record
// belonging to a pack that is NOT currently plugged in can be read and rewritten
// without borrowing one of the four live port slots. _dedupeNumber() needs exactly
// that: it has to renumber a stored pack sitting in a drawer.
static bool _loadRecordInto(PackRecord& r, const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    size_t sz = (size_t)f.size();
    if (sz == 0 || sz > 512) { f.close(); return false; }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) { f.close(); return false; }
    f.read((uint8_t*)buf, sz);
    buf[sz] = '\0';
    f.close();

    memset(&r, 0, sizeof(r));

    _rdU16  (buf, "regCYC",           &r.regCYC);
    _rdU8   (buf, "maxSocAtReg",      &r.maxSocAtReg);
    // autoNum / label are absent from pre-2026-10 records. _rdU8 leaves the field
    // at its memset-0 value when the key is missing, and 0 is the correct default
    // for both (0 autoNum = "assign on next registration", 0 label = "use autoNum"),
    // so old files load cleanly with no version bump and no migration pass.
    _rdU8   (buf, "autoNum",          &r.autoNum);
    _rdU8   (buf, "label",            &r.label);
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

static bool _loadRecord(uint8_t port, const char* path) {
    return _loadRecordInto(packRec[port], path);
}

static void _saveRecordOf(PackRecord& r) {
    if (!fsReady) return;
    if (!r.known) return;

    char path[32]; _cycFilename(path, sizeof(path), r.regCYC);
    File f = LittleFS.open(path, "w", true);
    if (!f) {
        Serial.printf("[REG] save failed: %s\n", path);
        return;
    }

    f.printf("regCYC=%u\n",           (unsigned)r.regCYC);
    f.printf("maxSocAtReg=%u\n",      (unsigned)r.maxSocAtReg);
    f.printf("autoNum=%u\n",          (unsigned)r.autoNum);
    f.printf("label=%u\n",            (unsigned)r.label);
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

static void _saveRecord(uint8_t port) {
    if (port >= NUM_PACKS) return;
    _saveRecordOf(packRec[port]);
}

// ── Auto identity ─────────────────────────────────────────────────────────────
// The human-facing pack number is assigned by the device, not the user. Scan every
// stored record and return the first free number in 1..NUM_LABELS, so packs are
// numbered in first-seen order and a number freed by a deleted record is reused.
// Returns 0 if all numbers are taken (more packs than labels) — callers treat 0 as
// "unnumbered" and fall back to the port tag.
//
// 2026-10-05 - Now counts EFFECTIVE numbers (label if set, else autoNum), not autoNum
// alone. WHAT WAS WRONG: a stored pack with an explicit label of 4 left autoNum 4 free,
// so the next unseen pack was handed 4 as its automatic number and two batteries showed
// "4" on the home screen and in every CSV Label column. Harmless while nobody used
// labels; actively misleading now that the fleet carries printed labels 1 / 4 / 6.
static void _collectTakenNumbers(bool* taken, const char* excludePath) {
    memset(taken, 0, (NUM_LABELS + 1) * sizeof(bool));

    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            // Build the path BEFORE close(). File::close() releases the FileImpl whose
            // destructor frees the strdup'd string that name() returned, so using the
            // pointer afterwards is a use-after-free.
            char path[40];
            snprintf(path, sizeof(path), "/packs/%s", f.name());
            f.close();
            if (!excludePath || strcmp(path, excludePath) != 0) {
                File rf = LittleFS.open(path, "r");
                if (rf) {
                    size_t sz = (size_t)rf.size();
                    char* buf = (sz > 0 && sz < 512) ? (char*)malloc(sz + 1) : nullptr;
                    if (buf) {
                        rf.read((uint8_t*)buf, sz);
                        buf[sz] = '\0';
                        uint8_t a = 0, l = 0;
                        _rdU8(buf, "autoNum", &a);
                        _rdU8(buf, "label",   &l);
                        free(buf);
                        uint8_t eff = l ? l : a;
                        if (eff >= 1 && eff <= NUM_LABELS) taken[eff] = true;
                    }
                    rf.close();
                }
            }
            f = dir.openNextFile();
        }
    }
    if (dir) dir.close();
}

static uint8_t _lowestFree(const bool* taken) {
    for (uint8_t n = 1; n <= NUM_LABELS; n++) if (!taken[n]) return n;
    return 0;
}

static uint8_t _nextFreeAutoNum() {
    bool taken[NUM_LABELS + 1];
    _collectTakenNumbers(taken, nullptr);
    return _lowestFree(taken);
}

// 2026-10-05 - A human number must point at exactly ONE battery.
//
// When a label is assigned, any OTHER stored pack already showing that number is
// renumbered to the lowest free number. The newest statement from the owner wins,
// because they are reading it off the sticker on the battery in their hand.
//
// A duplicate can arise two ways: an explicit label colliding with another pack's
// automatic number (the common case — autoNum 4 was handed out before label 4 existed),
// or two explicit labels colliding (the owner relabelled). Both are resolved by clearing
// the loser's label and giving it a fresh autoNum, so no record is ever deleted and no
// wear history is lost — only the displayed number moves.
#define DEDUPE_MAX_VICTIMS 4
static void _dedupeNumber(uint8_t keepPort, uint8_t n) {
    if (!fsReady || !n || n > NUM_LABELS) return;

    char keepPath[40] = {0};
    if (keepPort < NUM_PACKS && packRec[keepPort].known)
        _cycFilename(keepPath, sizeof(keepPath), packRec[keepPort].regCYC);

    // Collect the victims first. Rewriting records inside the directory walk would be
    // mutating the directory that the open iterator is still reading.
    char    victims[DEDUPE_MAX_VICTIMS][40];
    uint8_t nv = 0;

    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && nv < DEDUPE_MAX_VICTIMS) {
            char path[40];
            snprintf(path, sizeof(path), "/packs/%s", f.name());
            f.close();
            if (!keepPath[0] || strcmp(path, keepPath) != 0) {
                File rf = LittleFS.open(path, "r");
                if (rf) {
                    size_t sz = (size_t)rf.size();
                    char* buf = (sz > 0 && sz < 512) ? (char*)malloc(sz + 1) : nullptr;
                    if (buf) {
                        rf.read((uint8_t*)buf, sz);
                        buf[sz] = '\0';
                        uint8_t a = 0, l = 0;
                        _rdU8(buf, "autoNum", &a);
                        _rdU8(buf, "label",   &l);
                        free(buf);
                        if ((l ? l : a) == n) {
                            strncpy(victims[nv], path, sizeof(victims[0]) - 1);
                            victims[nv][sizeof(victims[0]) - 1] = '\0';
                            nv++;
                        }
                    }
                    rf.close();
                }
            }
            f = dir.openNextFile();
        }
        if (f) f.close();
    }
    if (dir) dir.close();

    for (uint8_t v = 0; v < nv; v++) {
        PackRecord tmp;
        if (!_loadRecordInto(tmp, victims[v])) continue;

        bool taken[NUM_LABELS + 1];
        _collectTakenNumbers(taken, victims[v]);   // every number in use EXCEPT this one's
        const uint8_t fresh = _lowestFree(taken);  // 0 = fleet outgrew NUM_LABELS

        tmp.label   = 0;
        tmp.autoNum = fresh;
        _saveRecordOf(tmp);

        // Keep a live port's RAM copy coherent — the duplicate may be plugged in right
        // now, in which case the screen would otherwise keep showing the old number
        // until the next re-identification.
        for (uint8_t q = 0; q < NUM_PACKS; q++) {
            if (q == keepPort || !packRec[q].known) continue;
            if (packRec[q].regCYC == tmp.regCYC) {
                packRec[q].label   = 0;
                packRec[q].autoNum = fresh;
            }
        }
        Serial.printf("[REG] #%u was duplicated by %s - renumbered to #%u\n",
                      (unsigned)n, victims[v], (unsigned)fresh);
    }
}

// ── One-shot label seed ───────────────────────────────────────────────────────
// See Config.h § Pack labels for the owner's port->label table and the three gates
// that make this fire exactly once. In short: version file, 2-minute window, and never
// on a pack the registry could not identify.
static const uint8_t _labelSeed[NUM_PACKS] = LABEL_SEED_LIST;
static bool    _seedArmed = false;   // true only on the first boot after a version bump
static uint8_t _seedUsed  = 0;       // bitmask of ports already seeded this boot

static void _seedLoad() {
    _seedArmed = false;
    if (!fsReady) return;

    uint8_t stored = 0;
    File f = LittleFS.open("/labelseed.bin", "r");
    if (f) {
        if ((size_t)f.size() >= 1) f.read(&stored, 1);
        f.close();
    }
    if (stored == LABEL_SEED_VERSION) {
        Serial.printf("[SEED] label seed v%u already applied - standing down\n",
                      (unsigned)stored);
        return;
    }

    // Record "applied" NOW, before a single pack has been seen. If the board loses
    // power halfway through seeding, the next boot must NOT re-arm and relabel whatever
    // is in the ports then — a half-seeded fleet gets fixed by the picker or by a
    // version bump, never by a silent retry with different batteries in the slots.
    File w = LittleFS.open("/labelseed.bin", "w", true);
    if (w) {
        uint8_t v = LABEL_SEED_VERSION;
        w.write(&v, 1);
        w.close();
    } else {
        Serial.println("[SEED] cannot write /labelseed.bin - seeding SKIPPED rather "
                       "than risk repeating every boot");
        return;
    }

    _seedArmed = true;
    _seedUsed  = 0;
    Serial.printf("[SEED] ARMED v%u: port1->#%u port2->#%u port3->#%u port4->#%u, "
                  "window %lu s\n",
                  (unsigned)LABEL_SEED_VERSION,
                  (unsigned)_labelSeed[0], (unsigned)_labelSeed[1],
                  (unsigned)_labelSeed[2], (unsigned)_labelSeed[3],
                  (unsigned long)(LABEL_SEED_WINDOW_MS / 1000UL));
}

// Called right after a port has been identified or registered.
static void _applyLabelSeed(uint8_t port) {
    if (!_seedArmed || port >= NUM_PACKS) return;
    if (_seedUsed & (1u << port)) return;

    if (millis() > LABEL_SEED_WINDOW_MS) {
        _seedArmed = false;
        Serial.println("[SEED] window closed - seed retired, numbers are now manual only");
        return;
    }

    const uint8_t n = _labelSeed[port];
    if (!n || n > NUM_LABELS) return;

    if (!packRec[port].known) {
        // Ambiguous or unidentifiable. Writing a label here would mean registering a
        // brand-new record for a battery we already believe is one of two known ones.
        Serial.printf("[SEED] port%u not identified - NOT seeding; if the screen asks, "
                      "answer #%u\n", port + 1, (unsigned)n);
        return;
    }

    _seedUsed |= (1u << port);
    packRec[port].label = n;
    _saveRecord(port);
    _dedupeNumber(port, n);
    Serial.printf("[SEED] port%u %s = #%u (owner's printed label)\n",
                  port + 1, packRec[port].cycID, (unsigned)n);
}

// ── Public API ────────────────────────────────────────────────────────────────

// Find the stored pack carrying human number n and adopt it into this port, so its
// lifetime history continues instead of a duplicate record being created. Used when
// the owner resolves an ambiguous insertion by telling us which pack it is.
static bool _adoptByNumber(uint8_t port, uint8_t n) {
    if (!n) return false;
    char foundPath[40] = {0};

    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            char path[40];
            snprintf(path, sizeof(path), "/packs/%s", f.name());
            f.close();
            File rf = LittleFS.open(path, "r");
            if (rf) {
                size_t sz = (size_t)rf.size();
                char* buf = (sz > 0 && sz < 512) ? (char*)malloc(sz + 1) : nullptr;
                if (buf) {
                    rf.read((uint8_t*)buf, sz);
                    buf[sz] = '\0';
                    uint8_t a = 0, l = 0;
                    _rdU8(buf, "autoNum", &a);
                    _rdU8(buf, "label",   &l);
                    free(buf);
                    if ((l ? l : a) == n) strncpy(foundPath, path, sizeof(foundPath) - 1);
                }
                rf.close();
            }
            if (foundPath[0]) break;
            f = dir.openNextFile();
        }
    }
    if (dir) dir.close();
    if (!foundPath[0]) return false;

    // Refuse if another port already holds this record. Two ports carrying the same
    // regCYC both write the SAME file at session close, so one port's entire session
    // energy and its sessions++ are silently lost to last-writer-wins, and historyLen
    // diverges between the two copies.
    for (uint8_t q = 0; q < NUM_PACKS; q++) {
        if (q == port || !packRec[q].known) continue;
        char other[40]; _cycFilename(other, sizeof(other), packRec[q].regCYC);
        if (strcmp(other, foundPath) == 0) {
            Serial.printf("[REG] port%u cannot adopt #%u - already held by port%u\n",
                          port + 1, (unsigned)n, q + 1);
            return false;
        }
    }

    // _loadRecord returns false WITHOUT memsetting on open failure / bad size, which
    // would leave the record untouched while we reported success — the owner answers the
    // prompt, the log says "resolved", and nothing is stored.
    if (!_loadRecord(port, foundPath)) {
        Serial.printf("[REG] port%u adopt FAILED to load %s\n", port + 1, foundPath);
        return false;
    }
    packRec[port].currentCycles = packs[port].cycles;   // re-anchor so the next match is tight
    packRec[port].ambiguous     = false;
    _saveRecord(port);
    Serial.printf("[REG] port%u resolved by owner to #%u (%s)\n",
                  port + 1, (unsigned)n, packRec[port].cycID);
    return true;
}

// Manual override of the auto-assigned number. Bound to the PHYSICAL PACK, so it
// follows the battery across ports. label 0 restores the automatic number.
void packRegistrySetLabel(uint8_t port, uint8_t label) {
    if (port >= NUM_PACKS || label > NUM_LABELS) return;

    // Ambiguous insertion: the owner is telling us WHICH stored pack this is. Adopt
    // that record so its wear history continues on the right battery — the whole
    // reason for asking rather than guessing.
    if (!packRec[port].known && packRec[port].ambiguous) {
        // "--" (0) means SKIP, not "new pack". Registering here would create a third
        // record for a battery we already know is one of two existing ones — the exact
        // mis-filing the ambiguity prompt exists to prevent — and it was the one-press
        // default, because the picker pre-fills at 0 when nothing is known.
        if (!label) {
            Serial.printf("[REG] port%u ambiguity left unresolved by owner\n", port + 1);
            return;
        }
        if (_adoptByNumber(port, label)) return;
        packRegistryRegister(port);            // no pack carries that number — it is new
        packRec[port].label = label;
        _saveRecord(port);
        _dedupeNumber(port, label);
        return;
    }

    if (!packRec[port].known) return;          // unidentified and not ambiguous — nowhere to store
    packRec[port].label = label;
    _saveRecord(port);
    // Save BEFORE dedupe: the scan reads this record back off flash to work out which
    // numbers are still free, so it has to already see the new one as taken.
    _dedupeNumber(port, label);
    Serial.printf("[REG] port%u %s label override = %u\n",
                  port + 1, packRec[port].cycID, (unsigned)label);
}

// What the human should see for the pack currently in this port:
// manual override if set, else the auto-assigned number, else 0 = "unknown".
uint8_t packRegistryNumber(uint8_t port) {
    if (port >= NUM_PACKS || !packRec[port].known) return 0;
    return packRec[port].label ? packRec[port].label : packRec[port].autoNum;
}

void packRegistryInit() {
    memset(packRec, 0, sizeof(packRec));
    if (!fsReady) return;
    if (!LittleFS.exists("/packs")) LittleFS.mkdir("/packs");
    Serial.println("[REG] ready — /packs directory ensured");
    _seedLoad();   // arms the one-shot label seed on the first boot after a flash
}

// Called from UART.ino the moment a port goes from invalid → valid.
// Called from UART.ino the moment a port goes from invalid -> valid.
//
// 2026-10 - MATCHING REWRITTEN. The previous scheme compared the live cycle count
// against each record's REGISTRATION cycles with a +/-500 window and accepted the
// FIRST file the directory walk returned. With this fleet spaced 10-17 cycles apart
// that window spans several packs at once, so identity was effectively decided by
// directory order and one pack's wear history could be filed under another. Now:
//   - compare against LAST-SEEN cycles (currentCycles), which advances every sighting
//   - take the NEAREST candidate, not the first
//   - if the two best are within PACK_CYC_AMBIGUOUS of each other, refuse to guess
// See Config.h for the fleet numbers that motivated each constant.
void packRegistryIdentify(uint8_t port) {
    if (!fsReady || port >= NUM_PACKS) return;

    uint16_t curCyc = packs[port].cycles;
    uint8_t  curMax = packs[port].maxSoc;
    memset(&packRec[port], 0, sizeof(PackRecord));

    char     bestPath[40] = {0};
    uint16_t bestDiff     = 0xFFFF;
    uint16_t secondDiff   = 0xFFFF;

    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            char path[40];
            snprintf(path, sizeof(path), "/packs/%s", f.name());
            f.close();

            File rf = LittleFS.open(path, "r");
            if (rf) {
                size_t sz = (size_t)rf.size();
                char* buf = (sz > 0 && sz < 512) ? (char*)malloc(sz + 1) : nullptr;
                if (buf) {
                    rf.read((uint8_t*)buf, sz);
                    buf[sz] = '\0';
                    uint16_t rReg = 0, rCur = 0; uint8_t rMax = 0;
                    _rdU16(buf, "regCYC",        &rReg);
                    _rdU16(buf, "currentCycles", &rCur);
                    _rdU8 (buf, "maxSocAtReg",   &rMax);
                    free(buf);

                    // Records written before currentCycles was tracked fall back to
                    // registration cycles - the only sighting we ever had.
                    uint16_t lastSeen = rCur ? rCur : rReg;

                    // Cycle count only ever increases. A reading below last-seen is a
                    // different pack (or a BMS reset), never this one.
                    if (rReg && curCyc >= lastSeen) {
                        uint16_t diff = curCyc - lastSeen;
                        int socD = (int)curMax - (int)rMax;
                        if (socD < 0) socD = -socD;
                        if (diff <= PACK_CYC_MATCH_WINDOW && socD <= PACK_SOC_MATCH_SLACK) {
                            if (diff < bestDiff) {
                                secondDiff = bestDiff;
                                bestDiff   = diff;
                                strncpy(bestPath, path, sizeof(bestPath) - 1);
                            } else if (diff < secondDiff) {
                                secondDiff = diff;
                            }
                        }
                    }
                }
                rf.close();
            }
            f = dir.openNextFile();
        }
    }
    if (dir) dir.close();

    if (bestPath[0] == '\0') {          // nothing close enough - a pack we have not met
        packRegistryRegister(port);
        _applyLabelSeed(port);
        return;
    }

    // Two stored packs within PACK_CYC_AMBIGUOUS of each other. Guessing here is how
    // a weak pack's decline ends up recorded against a healthy one, so do not.
    if (secondDiff != 0xFFFF && (secondDiff - bestDiff) <= PACK_CYC_AMBIGUOUS) {
        packRec[port].known         = false;
        packRec[port].ambiguous     = true;
        packRec[port].currentCycles = curCyc;
        Serial.printf("[REG] port%u AMBIGUOUS cyc=%u (best +%u, runner-up only %u further) - asking owner\n",
                      port + 1, (unsigned)curCyc, (unsigned)bestDiff,
                      (unsigned)(secondDiff - bestDiff));
        _applyLabelSeed(port);   // prints the number to answer with; seeds nothing
        return;
    }

    _loadRecord(port, bestPath);
    packRec[port].currentCycles = curCyc;
    if (packRec[port].known && packRec[port].autoNum == 0) {
        packRec[port].autoNum = _nextFreeAutoNum();   // backfill pre-auto-number records
    }
    _saveRecord(port);                  // advance last-seen cycles for the next match
    _applyLabelSeed(port);
    Serial.printf("[REG] port%u identified: %s  #%u  cyc=%u (+%u since last seen)\n",
                  port + 1, packRec[port].cycID,
                  (unsigned)packRegistryNumber(port),
                  (unsigned)curCyc, (unsigned)bestDiff);
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
    r.autoNum      = _nextFreeAutoNum();   // permanent human number, assigned once
    r.label        = 0;                    // no manual override

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
