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

    // 2026-10-05 - F-11: clamp. historyLen comes straight out of a text file; a damaged
    // or hand-edited record could carry any value up to 255, and both history arrays are
    // PACK_HISTORY_LEN long. _saveRecordOf loops to historyLen, so an oversized value
    // reads past the end of both arrays and writes a record that cannot be loaded back.
    if (r.historyLen > PACK_HISTORY_LEN) r.historyLen = PACK_HISTORY_LEN;

    snprintf(r.cycID, sizeof(r.cycID), "CYC-%u", (unsigned)r.regCYC);
    r.known = (r.regCYC > 0);
    // Remember the file we actually came from, so a later save cannot be redirected onto
    // some other pack's record by filename derivation. See PackRecord::file.
    snprintf(r.file, sizeof(r.file), "%s", path);

    free(buf);
    return r.known;
}

static bool _loadRecord(uint8_t port, const char* path) {
    return _loadRecordInto(packRec[port], path);
}

static void _saveRecordOf(PackRecord& r) {
    if (!fsReady) return;
    if (!r.known) return;

    // Write where this record LIVES, never where its regCYC says it would live. Legacy
    // records loaded before file[] existed fall back to the derivation, which is correct
    // for them: they are already at the derived name.
    char path[32];
    if (r.file[0]) snprintf(path, sizeof(path), "%s", r.file);
    else           _cycFilename(path, sizeof(path), r.regCYC);
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
static void _dedupeNumberByPath(const char* keep, uint8_t n) {
    if (!fsReady || !n || n > NUM_LABELS) return;
    // An empty keep would make the filter match everything INCLUDING the record we were
    // asked to protect, stripping the number just assigned. Not reachable today; one line.
    if (!keep || !keep[0]) return;

    char keepPath[40] = {0};
    if (keep && keep[0]) snprintf(keepPath, sizeof(keepPath), "%s", keep);

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
        // Match on the FILE, not on regCYC: two packs can legitimately share a cycle
        // count (ports 1 and 4 both read 56 on 2026-10-06), so regCYC no longer
        // identifies a record. The file does.
        for (uint8_t q = 0; q < NUM_PACKS; q++) {
            if (!packRec[q].known || !packRec[q].file[0]) continue;
            if (strcmp(packRec[q].file, keepPath) == 0) continue;   // never the kept one
            if (strcmp(packRec[q].file, victims[v]) == 0) {
                packRec[q].label   = 0;
                packRec[q].autoNum = fresh;
            }
        }
        if (fresh) {
            Serial.printf("[REG] #%u was duplicated by %s - renumbered to #%u\n",
                          (unsigned)n, victims[v], (unsigned)fresh);
        } else {
            // 2026-10-05 - F-6. Only reachable with more than NUM_LABELS packs on file.
            // The record keeps its full history; it just has no number to show, and
            // labelStr() falls back to the port tag. Say so rather than leave a pack
            // silently nameless.
            Serial.printf("[REG] #%u was duplicated by %s - but all %u numbers are "
                          "taken, so it now has NONE. Delete a retired pack's record "
                          "or raise NUM_LABELS\n",
                          (unsigned)n, victims[v], (unsigned)NUM_LABELS);
        }
    }
}

// Port-indexed convenience. The record a port holds knows its own filename now, so this no
// longer derives one — deriving is what let two packs share a file.
static void _dedupeNumber(uint8_t keepPort, uint8_t n) {
    const char* keep = (keepPort < NUM_PACKS && packRec[keepPort].known)
                       ? packRec[keepPort].file : nullptr;
    _dedupeNumberByPath(keep, n);
}

// ── One-shot label seed ───────────────────────────────────────────────────────
// See Config.h § Pack labels for the owner's port->label table and the three gates
// that make this fire exactly once. In short: version file, 2-minute window, and never
// on a pack the registry could not identify.
static const uint8_t _labelSeed[NUM_PACKS] = LABEL_SEED_LIST;
static bool     _seedArmed  = false;  // true only on the first boot after a version bump
static uint8_t  _seedUsed   = 0;      // bitmask of ports already seeded this boot
// 2026-10-05 - F-9: measure the window from when the seed ARMED, not from millis()==0.
// ESP-IDF keeps the microsecond counter running across deep sleep, so after a sleep
// millis() can already exceed the window and the one shot would be spent silently
// without a single pack having been looked at.
static uint32_t _seedArmedMs = 0;

// 2026-10-05 - F-1: how good the match was that set packRec[port].known.
// 0 = brand-new registration (cannot be a mis-match: the record was just created for
// the pack in front of us). 0xFFFF = nothing identified. Anything else is the forward
// cycle drift against the matched record's last-seen count.
static uint16_t _matchDiff[NUM_PACKS];

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

    _seedArmed   = true;
    _seedUsed    = 0;
    _seedArmedMs = millis();
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

    if ((millis() - _seedArmedMs) > LABEL_SEED_WINDOW_MS) {
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

    // GATE 4 (Config.h § PACK_SEED_MAX_DRIFT): a confident match is not the same as a
    // correct one. Demand a tight match, or a registration we just created ourselves.
    const uint16_t drift   = _matchDiff[port];
    const bool     socSame = (packRec[port].maxSocAtReg == packs[port].maxSoc);
    if (drift != 0 && (drift > PACK_SEED_MAX_DRIFT || !socSame)) {
        Serial.printf("[SEED] port%u %s match too loose to label (+%u cyc, maxSoc %u vs "
                      "%u) - NOT seeding #%u. Use the picker, or plug the right pack in "
                      "and bump LABEL_SEED_VERSION\n",
                      port + 1, packRec[port].cycID, (unsigned)drift,
                      (unsigned)packs[port].maxSoc, (unsigned)packRec[port].maxSocAtReg,
                      (unsigned)n);
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
    // 2026-10-09 - R-6, RE-OPENED AND NOW CLOSED HERE TOO. This check used to DERIVE the
    // other port's filename from its regCYC instead of reading the name it actually holds.
    // The fix was applied to packRegistryIdentify() and written up there, but this twin was
    // missed — and this is the ambiguity-resolution path, which is exactly what the
    // 8-pack rebuild triggers. Two packs sharing a cycle count get names CYC-56.dat and
    // CYC-56_2.dat; deriving from regCYC yields CYC-56.dat for BOTH, so the collision is
    // invisible, two ports end up on one file, and last-writer-wins silently destroys one
    // pack's session Wh and sessions++. Ports 1 and 4 both read 56 cycles today, so the _2
    // names already exist on the device. Compare the STORED name.
    for (uint8_t q = 0; q < NUM_PACKS; q++) {
        if (q == port || !packRec[q].known || !packRec[q].file[0]) continue;
        if (strcmp(packRec[q].file, foundPath) == 0) {
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

    // 2026-10-05 - F-5: clearing the override back to "automatic" has to re-check the
    // autoNum underneath it. That number was assigned before the label existed and may
    // have been handed to another pack in the meantime (or renumbered by _dedupeNumber),
    // so simply revealing it again can resurrect the duplicate this whole change removes.
    if (label == 0) {
        bool taken[NUM_LABELS + 1];
        // R-7: exclude the file this port actually holds, not a derived name.
        _collectTakenNumbers(taken, packRec[port].file);
        if (!packRec[port].autoNum || taken[packRec[port].autoNum])
            packRec[port].autoNum = _lowestFree(taken);
        _saveRecord(port);
        Serial.printf("[REG] port%u %s override cleared - automatic number is #%u\n",
                      port + 1, packRec[port].cycID, (unsigned)packRec[port].autoNum);
        return;
    }

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
    _matchDiff[port] = 0xFFFF;          // nothing identified yet

    // 2026-10-05 - F-15: a different battery is now in this port, so the session energy
    // accumulated by the previous one must not keep adding to it. packRegistrySessionUpdate
    // zeroes these at session close, but a swap mid-session used to carry the old pack's
    // Wh straight onto the new pack's lifetime total.
    packs[port].whIn  = 0.0f;
    packs[port].whOut = 0.0f;

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

    // 2026-10-05 - F-2: refuse a record another port already holds. _adoptByNumber has
    // had this check since it was written; identification did not, which is the path
    // that actually runs on every plug-in.
    //
    // WHY IT MATTERS: two ports carrying the same regCYC both write the SAME file at
    // session close, so one port's entire session Wh and its sessions++ are lost to
    // last-writer-wins and historyLen diverges between the two copies. With the seed
    // armed it also mislabels — port 1 writes label 1, port 2 overwrites it with label 4
    // in that same file, and _dedupeNumber finds no victim because the file it would
    // have to fix is the one it is told to keep.
    // R-6: compare the file the other port ACTUALLY HOLDS. This used to derive a name
    // from regCYC, which the new _2 filenames make wrong: a port holding
    // /packs/CYC-0056_2.dat derives CYC-0056.dat, the comparison misses, and BOTH ports
    // load and write one record - re-opening the very clobber this delta set out to fix.
    // Ports 1 and 4 both read 56 cycles today, so the fleet is already in the state that
    // produces _2 files.
    for (uint8_t q = 0; q < NUM_PACKS; q++) {
        if (q == port || !packRec[q].known || !packRec[q].file[0]) continue;
        if (strcmp(packRec[q].file, bestPath) == 0) {
            packRec[port].known         = false;
            packRec[port].ambiguous     = true;
            packRec[port].currentCycles = curCyc;
            _matchDiff[port]            = 0xFFFF;
            Serial.printf("[REG] port%u best match %s is already held by port%u - "
                          "treating as ambiguous, asking owner\n",
                          port + 1, bestPath, q + 1);
            _applyLabelSeed(port);      // prints the number to answer with; seeds nothing
            return;
        }
    }

    _loadRecord(port, bestPath);
    packRec[port].currentCycles = curCyc;
    _matchDiff[port] = bestDiff;        // how much to trust this for seeding — see F-1
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
    // Claim a filename nothing is using. Plain CYC-NNNN.dat if free, else _2, _3...
    // This is the fix for the clobber: a new pack can no longer land on an existing file.
    _cycFilename(r.file, sizeof(r.file), r.regCYC);
    if (fsReady && LittleFS.exists(r.file)) {
        for (uint8_t n = 2; n < 10; n++) {
            char cand[28];
            snprintf(cand, sizeof(cand), "/packs/CYC-%04u_%u.dat", (unsigned)r.regCYC, n);
            if (n == 9 && LittleFS.exists(cand)) {
                // R-9: exhaustion used to fall through with r.file still set to the base
                // name, which EXISTS - so _saveRecordOf opened it "w" and truncated it.
                // The original clobber, silently. Refuse to register instead.
                Serial.printf("[REG] CANNOT REGISTER: 9 records already share CYC-%04u. "
                              "Delete a stale record from the dashboard first.%s",
                              (unsigned)r.regCYC, "\n");
                r.known = false;
                return;
            }
            if (!LittleFS.exists(cand)) {
                Serial.printf("[REG] %s is taken by another pack - registering as %s "
                              "instead (no record overwritten)\n", r.file, cand);
                snprintf(r.file, sizeof(r.file), "%s", cand);
                break;
            }
        }
    }
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
    // A record created from the pack in front of us cannot be a mis-match, so the seed
    // is allowed to label it (Config.h § PACK_SEED_MAX_DRIFT).
    _matchDiff[port] = 0;

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

// ── Stored-record admin (web) ────────────────────────────────────────────────
// Operates on records by FILENAME, because a record for a pack that is not plugged in has
// no port and its regCYC is not unique (ports 1 and 4 both read 56 cycles on 2026-10-06).

// Guard every path: only files we own, inside /packs, and no traversal.
static bool _validRecPath(const char* f) {
    return f && strncmp(f, "/packs/CYC-", 11) == 0 && !strstr(f, "..") &&
           strlen(f) < 28 && strcmp(f + strlen(f) - 4, ".dat") == 0;
}

// Refuse to touch a record a live port is holding — it would be written straight back.
static bool _heldByPort(const char* f) {
    for (uint8_t q = 0; q < NUM_PACKS; q++)
        if (packRec[q].known && packRec[q].file[0] && strcmp(packRec[q].file, f) == 0)
            return true;
    return false;
}

uint8_t packRegistryList(char* out, size_t outLen, const char* csrf) {
    out[0] = '\0';
    if (!fsReady) return 0;
    size_t  used  = 0;
    uint8_t count = 0;
    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            char path[40];
            snprintf(path, sizeof(path), "/packs/%s", f.name());
            f.close();
            PackRecord r;
            if (_loadRecordInto(r, path)) {
                int8_t onPort = -1;
                for (uint8_t q = 0; q < NUM_PACKS; q++)
                    if (packRec[q].known && strcmp(packRec[q].file, path) == 0) onPort = (int8_t)q;
                // R-5: was row[420]. The format string alone is ~381 literal chars,
                // leaving 39 for 13 substitutions when the path is 19-21 and the token 8.
                // Every row overflowed, and because snprintf's return is the UNTRUNCATED
                // length it was then used as a memcpy size - an out-of-bounds stack read
                // into the served page, and truncation landing inside the closing
                // </form></td></tr>, which dropped the forms and broke the admin UI this
                // whole route exists for.
                char row[640];
                int n = snprintf(row, sizeof(row),
                    "<tr><td>%s</td><td>%u</td><td>%u</td><td>%s</td><td>%u</td><td>%u</td>"
                    "<td>%.1f</td><td>%.1f</td><td>%s</td>"
                    "<td><form method='POST' action='/packedit'>"
                    "<input type='hidden' name='_t' value='%s'>"
                    "<input type='hidden' name='f' value='%s'>"
                    "<input name='l' type='number' min='0' max='%u' value='%u' style='width:4em'>"
                    "<button name='a' value='set'>set</button>"
                    "<button name='a' value='del'>delete</button></form></td></tr>",
                    r.cycID, (unsigned)r.regCYC, (unsigned)r.currentCycles, r.firstSeen,
                    (unsigned)r.autoNum, (unsigned)r.label,
                    r.totalWhCharged, r.totalWhDischarged,
                    (onPort >= 0) ? "IN USE" : "-",
                    csrf, path, (unsigned)NUM_LABELS,
                    (unsigned)(r.label ? r.label : r.autoNum));
                if (n >= (int)sizeof(row)) n = (int)sizeof(row) - 1;   // clamp before use
                // 2026-10-09 - COUNT ONLY WHAT WE ACTUALLY EMITTED. count++ used to run
                // unconditionally, outside this guard, so a record dropped for lack of
                // buffer still incremented the number printed in the footer. At ~463 B a
                // row the old 3600 B buffer held 7 — so a rebuilt 8-pack fleet would have
                // rendered 7 rows under a footer reading "8 stored record(s)", with no
                // warning anywhere. Silent undercount on the one page the rebuild is
                // driven from. The caller's buffer is now sized for the full fleet; this
                // makes the count honest if it is ever too small again.
                if (n > 0 && used + (size_t)n + 1 < outLen) {
                    memcpy(out + used, row, (size_t)n);
                    used += (size_t)n;
                    out[used] = '\0';
                    count++;
                } else {
                    Serial.printf("[REG] LIST TRUNCATED - %s did not fit in %u bytes; "
                                  "the page is INCOMPLETE\n", path, (unsigned)outLen);
                    // N-4: the owner does this on a phone, where no one sees the serial
                    // port. Spend the last of the buffer saying the page is incomplete —
                    // a visibly short list beats a silently short one.
                    const char* warn =
                        "<tr><td colspan='10' style='color:#ff5555;font-weight:bold'>"
                        "LIST TRUNCATED - more records exist than fit on this page. "
                        "Delete some, or raise the rows buffer.</td></tr>";
                    size_t wl = strlen(warn);
                    if (used + wl + 1 < outLen) {
                        memcpy(out + used, warn, wl);
                        used += wl;
                        out[used] = '\0';
                    }
                    break;                       // nothing more will fit; stop walking
                }
            }
            f = dir.openNextFile();
        }
    }
    if (dir) dir.close();
    return count;
}

bool packRegistryForget(const char* file) {
    if (!fsReady || !_validRecPath(file)) return false;
    if (_heldByPort(file)) {
        Serial.printf("[REG] refusing to delete %s - a live port holds it\n", file);
        return false;
    }
    bool ok = LittleFS.remove(file);
    Serial.printf("[REG] %s %s\n", ok ? "DELETED" : "failed to delete", file);
    return ok;
}

bool packRegistrySetLabelByFile(const char* file, uint8_t label) {
    if (!fsReady || !_validRecPath(file) || label > NUM_LABELS) return false;
    PackRecord r;
    if (!_loadRecordInto(r, file)) return false;
    r.label = label;
    // R-8: clearing an override has to re-check the autoNum underneath it, exactly as the
    // device path does. Otherwise it reveals a number another pack has since taken, or -
    // on a legacy record whose autoNum is 0 - leaves the pack with no number at all.
    if (label == 0) {
        bool taken[NUM_LABELS + 1];
        _collectTakenNumbers(taken, file);
        if (!r.autoNum || taken[r.autoNum]) r.autoNum = _lowestFree(taken);
    }
    _saveRecordOf(r);
    // Keep a live port's RAM copy coherent, or the screen keeps the old number until the
    // pack is re-seated.
    for (uint8_t q = 0; q < NUM_PACKS; q++)
        if (packRec[q].known && strcmp(packRec[q].file, file) == 0) packRec[q].label = label;
    if (label) _dedupeNumberByPath(file, label);   // keep THIS record, renumber others
    Serial.printf("[REG] %s label set to #%u via web\n", file, (unsigned)label);
    return true;
}

uint8_t packRegistryForgetAll(void) {
    if (!fsReady) return 0;
    // Collect first, delete after closing the directory: removing inside an openNextFile()
    // walk kills the walk, which is the bug that made the dashboard's old "Delete all"
    // stop after one file.
    // 2026-10-09 - B-5: the cap was 12, and a wipe that hit it looked EXACTLY like a
    // complete one — same message, same page, no warning. The dump of 2026-10-05 already
    // showed four records all claiming 55 cycles, so a registry well past 12 is not
    // hypothetical, and a half-wiped registry is the worst possible starting point for a
    // rebuild: the leftovers are the blended-history records the rebuild exists to destroy.
    // 32 covers 8 packs plus every _2.._9 collision name. packRegistryCount() below lets
    // the caller state plainly whether anything is left.
    // F-5: static, not stack. 32x40 is 1,280 B and this runs inside
    // _srv.handleClient() on the 8 KB loopTask stack, on the one path the owner
    // deliberately exercises twice during a rebuild. An overflow there is a panic
    // reboot, and under K-1 a reboot stops the keep-alive. One call site, single
    // threaded, so static is free - and it shows up honestly in the RAM figure.
    static char victims[32][40];
    uint8_t nv = 0;
    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && nv < 32) {
            snprintf(victims[nv], sizeof(victims[0]), "/packs/%s", f.name());
            f.close();
            if (!_heldByPort(victims[nv])) nv++;
            f = dir.openNextFile();
        }
        if (f) f.close();
    }
    if (dir) dir.close();
    uint8_t gone = 0;
    for (uint8_t i = 0; i < nv; i++) if (LittleFS.remove(victims[i])) gone++;
    Serial.printf("[REG] WIPED %u stored record(s) - rebuild from the markers now\n", gone);
    return gone;
}

// How many records are stored right now. Exists so a wipe can REPORT whether it finished
// instead of leaving a partial wipe indistinguishable from a complete one (B-5).
uint8_t packRegistryCount(void) {
    if (!fsReady) return 0;
    uint8_t n = 0;
    File dir = LittleFS.open("/packs");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) { if (!f.isDirectory()) n++; f.close(); f = dir.openNextFile(); }
    }
    if (dir) dir.close();
    return n;
}

const PackRecord* packRegGet(uint8_t port) {
    return (port < NUM_PACKS) ? &packRec[port] : nullptr;
}
