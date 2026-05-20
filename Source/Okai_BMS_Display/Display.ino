// Display.ino — 4-screen BMS display on T-Display-S3 (320×170 landscape)
//
// Screens (BTN2 = next →, BTN3 = prev ←, BTN1 = action):
//   0 — Fleet overview   (2×2 grid, BTN1 = WiFi toggle)
//   1 — Per-pack detail  (single pack, BTN1 = cycle P1→P4)
//   2 — Charging live    (fill bars + ETA per pack)
//   3 — Cell health      (delta, cycles, energy SoH vs brand-new BD)
//
// Energy reference: Panasonic NCR18650BD 10S4P = 460.8 Wh design capacity
//
// Library: Arduino_GFX_Library — install via Library Manager
//   Search: "Arduino_GFX"  author: "moononournation"

#include <Arduino_GFX_Library.h>

// ── Palette ───────────────────────────────────────────────────────────────────
static uint16_t C_BG, C_GOOD, C_WARN, C_POOR, C_CHARGE,
                C_TEXT, C_DIM, C_ACCENT, C_HDR;

// ── Display objects ───────────────────────────────────────────────────────────
static Arduino_DataBus *_bus;
static Arduino_GFX     *_gfx;

// ── Layout ────────────────────────────────────────────────────────────────────
#define HDR_H     16
#define DOT_Y    165   // page-indicator dots center Y
#define CELL_W   160
#define CELL_H    73   // (170 - HDR_H - 8) / 2; 8px reserved for dots row

static const uint16_t CX[4] = { 0,      CELL_W, 0,      CELL_W };
static const uint16_t CY[4] = { HDR_H,  HDR_H,  HDR_H + CELL_H,
                                 HDR_H + CELL_H };

// Charging bar geometry (screen 2)
#define BAR_X    60
#define BAR_W   200
#define BAR_H    10

// ── Screen state ──────────────────────────────────────────────────────────────
#define NUM_SCREENS 4
static uint8_t  _screen     = 0;
static uint8_t  _detailPack = 0;
static uint32_t _alertEnd   = 0;
static bool     _prevChargeDone[NUM_PACKS];

// ── Button debounce ───────────────────────────────────────────────────────────
static bool     _b1Prev, _b2Prev, _b3Prev;
static uint32_t _b1Ts, _b2Ts, _b3Ts;
#define DEBOUNCE_MS   50UL
#define LONGPRESS_MS 800UL   // hold BTN3 to enter label assign

// ── Refresh timing ────────────────────────────────────────────────────────────
static uint32_t _dispLast;

// ── Pack disconnect tracking ──────────────────────────────────────────────────
#define DISCONNECT_DEBOUNCE_MS 30000UL
static uint32_t _portLostMs[NUM_PACKS];
static bool     _portWasValid[NUM_PACKS];

// ── Overlay state ─────────────────────────────────────────────────────────────
// Only one overlay active at a time: disconnect modal OR label picker
static bool    _showDisconnect = false;
static uint8_t _disconnectPort = 0;

static bool    _showLabelPick  = false;
static uint8_t _labelPickPort  = 0;
static uint8_t _labelPickVal   = 0;   // 0=unassigned, 1-8=label

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint16_t healthColor(uint8_t i) {
    if (!packs[i].valid) return C_DIM;
    float d = packs[i].cellHigh - packs[i].cellLow;
    if (d >= CELL_DELTA_POOR_V) return C_POOR;
    if (d >= CELL_DELTA_WARN_V) return C_WARN;
    return C_GOOD;
}

// Heuristic SoH % vs brand-new BD cell
// Accounts for cell imbalance (early sign of cell degradation) and age from cycles
static uint8_t sohEstimate(uint8_t i) {
    if (!packs[i].valid) return 0;
    float delta_mV = (packs[i].cellHigh - packs[i].cellLow) * 1000.0f;
    float loss = delta_mV * 0.3f + packs[i].cycles * 0.02f;
    if (loss > 30.0f) loss = 30.0f;
    int soh = 100 - (int)loss;
    return (uint8_t)(soh < 0 ? 0 : soh);
}

static void drawPageDots() {
    _gfx->fillRect(130, DOT_Y - 4, 80, 8, C_BG);
    for (uint8_t i = 0; i < NUM_SCREENS; i++) {
        uint16_t dx = 148 + i * 12;
        if (i == _screen) _gfx->fillCircle(dx, DOT_Y, 3, C_ACCENT);
        else              _gfx->drawCircle(dx, DOT_Y, 3, C_DIM);
    }
}

// ── Overlay: disconnect modal ─────────────────────────────────────────────────
static void drawDisconnectModal() {
    uint8_t port = _disconnectPort;
    char lbl[6]; labelStr(port, lbl, sizeof(lbl));

    _gfx->fillRect(8, 44, 304, 90, C_HDR);
    _gfx->drawRect(8, 44, 304, 90, C_WARN);
    _gfx->drawRect(9, 45, 302, 88, C_WARN);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_WARN);
    _gfx->setCursor(18, 56);
    char line[40];
    snprintf(line, sizeof(line), "Port %u (pack L%s) disconnected", port+1, lbl);
    _gfx->print(line);

    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(18, 70);
    _gfx->print("Inserting a different pack?");

    _gfx->setTextColor(C_GOOD);
    _gfx->setCursor(18, 86);
    _gfx->print("BTN1: Yes \x7e assign new label");

    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(18, 100);
    _gfx->print("BTN2/BTN3: Dismiss");
}

// ── Overlay: label picker ─────────────────────────────────────────────────────
static void drawLabelPicker() {
    _gfx->fillRect(20, 54, 280, 72, C_HDR);
    _gfx->drawRect(20, 54, 280, 72, C_ACCENT);
    _gfx->drawRect(21, 55, 278, 70, C_ACCENT);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(30, 66);
    char title[36];
    snprintf(title, sizeof(title), "Port %u \x7e assign pack label:", _labelPickPort+1);
    _gfx->print(title);

    // Big label value, centered
    char valStr[4];
    if (_labelPickVal == 0) strcpy(valStr, "--");
    else snprintf(valStr, sizeof(valStr), "%u", _labelPickVal);
    _gfx->setTextSize(3);
    _gfx->setTextColor(C_TEXT);
    int16_t vw = (int16_t)strlen(valStr) * 18;
    _gfx->setCursor(160 - vw/2, 80);
    _gfx->print(valStr);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(30, 114);
    _gfx->print("BTN1:cycle  BTN2:confirm  BTN3:cancel");
}

// ── Header bar ───────────────────────────────────────────────────────────────
static void drawHeader() {
    _gfx->fillRect(0, 0, 320, HDR_H, C_HDR);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(2, 4);
    _gfx->print("OKAI BMS " FW_VERSION);

    // WiFi + log mode in the centre
    LogMode lm = logCurrentMode();
    char wstr[16];
    const char *modeTag = (lm == LOG_RIDE) ? " [R]" : (lm == LOG_CHARGE) ? " [C]" : "";
    snprintf(wstr, sizeof(wstr), "%s%s",
             wifiActive ? "WiFi:ON" : "WiFi:OFF", modeTag);
    _gfx->setCursor(148, 4);
    _gfx->setTextColor(wifiActive ? C_GOOD : C_DIM);
    _gfx->print(wstr);

    uint32_t s = millis() / 1000;
    char up[10];
    snprintf(up, sizeof(up), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(256, 4);
    _gfx->print(up);
}

// ── Screen 0: Fleet Overview ──────────────────────────────────────────────────
static void drawFleetCell(uint8_t i) {
    uint16_t cx = CX[i], cy = CY[i];
    uint16_t hc = healthColor(i);

    _gfx->fillRect(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, C_BG);
    _gfx->drawRect(cx,     cy,     CELL_W,     CELL_H,     hc);
    _gfx->drawRect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, hc);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(cx + 4, cy + 4);
    _gfx->print("P"); _gfx->print(i + 1);

    if (!packs[i].valid) {
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(cx + 28, cy + 22);
        _gfx->print("-- --");
        _gfx->setTextSize(1);
        _gfx->setCursor(cx + 44, cy + 50);
        _gfx->print("no data");
        return;
    }

    // Health tag top-right
    float delta = packs[i].cellHigh - packs[i].cellLow;
    const char *htag = (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                       (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
    _gfx->setTextColor(hc);
    _gfx->setCursor(cx + CELL_W - 30, cy + 4);
    _gfx->print(htag);

    // SOC — textSize 3 (char = 18×24 px)
    char soc_s[6];
    snprintf(soc_s, sizeof(soc_s), "%u%%", (unsigned)packs[i].soc);
    int16_t soc_w = (int16_t)strlen(soc_s) * 18;
    _gfx->setTextSize(3);
    _gfx->setTextColor(hc);
    _gfx->setCursor(cx + (CELL_W - soc_w) / 2, cy + 12);
    _gfx->print(soc_s);

    // Voltage + current
    _gfx->setTextSize(1);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(cx + 4, cy + 42);
    char ln1[22];
    snprintf(ln1, sizeof(ln1), "%.1fV  %+.1fA", packs[i].voltage, packs[i].current);
    _gfx->print(ln1);

    // Cell delta + max temp
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(cx + 4, cy + 52);
    uint16_t dmv = (uint16_t)(delta * 1000.0f + 0.5f);
    char ln2[22];
    snprintf(ln2, sizeof(ln2), "d%umV  %u*C", dmv, (unsigned)packs[i].maxTemp);
    _gfx->print(ln2);

    // CYC fingerprint — unique pack identity
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(cx + 4, cy + 62);
    char ln3[14];
    snprintf(ln3, sizeof(ln3), "CYC-%u", (unsigned)packs[i].cycles);
    _gfx->print(ln3);
}

static void drawScreenFleet() {
    drawHeader();
    for (uint8_t i = 0; i < NUM_PACKS; i++) drawFleetCell(i);
    _gfx->fillRect(0, HDR_H + 2 * CELL_H, 320, 170 - (HDR_H + 2 * CELL_H), C_BG);
    drawPageDots();
}

// ── Screen 1: Per-pack Detail ─────────────────────────────────────────────────
static void drawScreenDetail() {
    uint8_t i    = _detailPack;
    uint16_t hc  = healthColor(i);

    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();

    // Pack label + selector dots
    _gfx->setTextSize(1);
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(4, 22);
    char plabel[4]; snprintf(plabel, sizeof(plabel), "P%u", i + 1);
    _gfx->print(plabel);

    for (uint8_t d = 0; d < NUM_PACKS; d++) {
        uint16_t dx = 36 + d * 14;
        if (d == i) _gfx->fillCircle(dx, 25, 4, C_ACCENT);
        else        _gfx->drawCircle(dx, 25, 4, C_DIM);
    }

    // Health tag top-right
    float delta  = packs[i].valid ? (packs[i].cellHigh - packs[i].cellLow) : 0.0f;
    const char *htag = !packs[i].valid ? "----" :
                       (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                       (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
    _gfx->setTextColor(hc);
    _gfx->setCursor(280, 22);
    _gfx->print(htag);

    if (!packs[i].valid) {
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(80, 88);
        _gfx->print("NO DATA");
        drawPageDots();
        return;
    }

    // SOC — textSize 4 (char = 24×32 px)
    char soc_s[6];
    snprintf(soc_s, sizeof(soc_s), "%u%%", (unsigned)packs[i].soc);
    int16_t soc_w = (int16_t)strlen(soc_s) * 24;
    _gfx->setTextSize(4);
    _gfx->setTextColor(hc);
    _gfx->setCursor((320 - soc_w) / 2, 36);
    _gfx->print(soc_s);

    // Instant power | Available energy — textSize 2 (char = 12×16 px)
    float powerW  = packs[i].voltage * packs[i].current;
    float availWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
    char pw_s[14], av_s[14];
    snprintf(pw_s, sizeof(pw_s), "%+.0fW", powerW);
    snprintf(av_s, sizeof(av_s), "%.0fWh avail", availWh);
    _gfx->setTextSize(2);
    _gfx->setTextColor(packs[i].current >= 0 ? C_CHARGE : C_ACCENT);
    _gfx->setCursor(4, 78);
    _gfx->print(pw_s);
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(140, 78);
    _gfx->print(av_s);

    // Voltage | Current — textSize 2
    char v_s[10], a_s[10];
    snprintf(v_s, sizeof(v_s), "%.2fV", packs[i].voltage);
    snprintf(a_s, sizeof(a_s), "%+.2fA", packs[i].current);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(4, 98);
    _gfx->print(v_s);
    _gfx->setCursor(170, 98);
    _gfx->print(a_s);

    // Cell delta | Temp — textSize 1
    uint16_t dmv = (uint16_t)(delta * 1000.0f + 0.5f);
    char d_s[12], t_s[10];
    snprintf(d_s, sizeof(d_s), "d%umV", dmv);
    snprintf(t_s, sizeof(t_s), "%u*C", (unsigned)packs[i].maxTemp);
    _gfx->setTextSize(1);
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(4, 120);  _gfx->print(d_s);
    _gfx->setCursor(100, 120); _gfx->print(t_s);

    // Cycles | SoH vs new BD — textSize 1
    uint8_t soh = sohEstimate(i);
    char c_s[14], soh_s[16];
    snprintf(c_s,   sizeof(c_s),   "CYC-%u", (unsigned)packs[i].cycles);
    snprintf(soh_s, sizeof(soh_s), "SoH %u%% vs new", (unsigned)soh);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 132);  _gfx->print(c_s);
    _gfx->setTextColor(soh >= 80 ? C_GOOD : soh >= 60 ? C_WARN : C_POOR);
    _gfx->setCursor(100, 132); _gfx->print(soh_s);

    // Session energy in/out — textSize 1
    char sess[36];
    snprintf(sess, sizeof(sess), "Sess: +%.1fWh / -%.1fWh",
             packs[i].whIn, packs[i].whOut);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 144);
    _gfx->print(sess);

    // Charger status — textSize 1
    if (packs[i].chargerDetected) {
        _gfx->setTextColor(packs[i].chargeDone ? C_GOOD : C_CHARGE);
        _gfx->setCursor(4, 156);
        _gfx->print(packs[i].chargeDone ? "Charge complete" : "Charging...");
    }

    drawPageDots();
}

// ── Screen 2: Charging Live ───────────────────────────────────────────────────
static void drawScreenCharging() {
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();

    bool anyCharger  = false;
    uint8_t donePacks = 0;
    float totalA     = 0.0f;
    bool blinkOn     = (millis() / 250) % 2;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (packs[i].valid && packs[i].chargerDetected) anyCharger = true;
        if (packs[i].valid && packs[i].chargeDone)      donePacks++;
        if (packs[i].valid && packs[i].isCharging)      totalA += packs[i].current;
    }

    if (!anyCharger) {
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(60, 84);
        _gfx->print("No charger");
        drawPageDots();
        return;
    }

    // One row per pack (28 px per row, starting at y=26)
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        uint16_t ry = 26 + i * 28;
        bool hasData = packs[i].valid && packs[i].chargerDetected;

        // Pack label
        _gfx->setTextSize(1);
        _gfx->setTextColor(C_TEXT);
        _gfx->setCursor(4, ry);
        char pl[4]; snprintf(pl, sizeof(pl), "P%u", i + 1);
        _gfx->print(pl);

        if (!hasData) {
            _gfx->setTextColor(C_DIM);
            _gfx->setCursor(BAR_X, ry);
            _gfx->print("---");
            continue;
        }

        // SOC%  (left of bar)
        char soc_s[6];
        snprintf(soc_s, sizeof(soc_s), "%u%%", (unsigned)packs[i].soc);
        _gfx->setTextColor(C_TEXT);
        _gfx->setCursor(22, ry);
        _gfx->print(soc_s);

        // Fill bar
        uint16_t filled = (uint16_t)((uint32_t)packs[i].soc * BAR_W / 100);
        uint16_t barClr = packs[i].chargeDone ? C_GOOD : C_CHARGE;
        _gfx->fillRect(BAR_X,          ry + 10, filled,       BAR_H, barClr);
        _gfx->fillRect(BAR_X + filled, ry + 10, BAR_W - filled, BAR_H, C_DIM);
        _gfx->drawRect(BAR_X - 1, ry + 9, BAR_W + 2, BAR_H + 2, C_DIM);

        // Animated leading-edge blink while charging
        if (packs[i].isCharging && blinkOn && filled < BAR_W) {
            _gfx->drawFastVLine(BAR_X + filled, ry + 10, BAR_H, 0xFFFF);
        }

        // ETA or DONE (right of bar)
        if (packs[i].chargeDone) {
            _gfx->setTextColor(C_GOOD);
            _gfx->setCursor(BAR_X + BAR_W + 4, ry);
            _gfx->print("DONE");
        } else if (packs[i].isCharging && packs[i].current > 0.05f) {
            float etaMin = ((100.0f - packs[i].soc) / 100.0f)
                           * PACK_DESIGN_AH / packs[i].current * 60.0f;
            uint16_t eta = (uint16_t)(etaMin + 0.5f);
            char eta_s[10];
            if (eta >= 60) snprintf(eta_s, sizeof(eta_s), "%uh%02um", eta/60, eta%60);
            else           snprintf(eta_s, sizeof(eta_s), "%um", eta);
            _gfx->setTextColor(C_CHARGE);
            _gfx->setCursor(BAR_X + BAR_W + 4, ry);
            _gfx->print(eta_s);
        }
    }

    // Summary
    float totalW = 0.0f;
    for (uint8_t i = 0; i < NUM_PACKS; i++)
        if (packs[i].valid && packs[i].isCharging)
            totalW += packs[i].voltage * packs[i].current;

    char sum[40];
    snprintf(sum, sizeof(sum), "Total: %.0fW  Packs done: %u/%u",
             totalW, (unsigned)donePacks, (unsigned)NUM_PACKS);
    _gfx->setTextSize(1);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 152);
    _gfx->print(sum);

    drawPageDots();
}

// ── Screen 3: Cell Health / Energy ───────────────────────────────────────────
static void drawScreenHealth() {
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(4, 20);
    _gfx->print("CELL HEALTH  (vs NCR18650BD new)");

    // Column headers
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 32);
    _gfx->print("Pack  dV-mV  CYC#  SoH  Avail.Wh  Status");

    uint8_t worstPack = 0xFF;
    uint8_t lowestSoH = 255;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        uint16_t ry = 44 + i * 18;
        _gfx->setCursor(4, ry);

        if (!packs[i].valid) {
            _gfx->setTextColor(C_DIM);
            char row[42];
            snprintf(row, sizeof(row), "P%u    ---    ---   ---  ---Wh  no data", i+1);
            _gfx->print(row);
            continue;
        }

        uint8_t  soh  = sohEstimate(i);
        uint16_t dmv  = (uint16_t)((packs[i].cellHigh - packs[i].cellLow) * 1000.0f + 0.5f);
        float    avWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        uint16_t hc   = healthColor(i);
        const char *stag = (dmv >= (uint16_t)(CELL_DELTA_POOR_V * 1000.0f)) ? "POOR" :
                           (dmv >= (uint16_t)(CELL_DELTA_WARN_V * 1000.0f)) ? "WARN" : "GOOD";

        if (soh < lowestSoH) { lowestSoH = soh; worstPack = i; }

        // Fixed-width data row
        char row[42];
        snprintf(row, sizeof(row), "P%u  %4u  %4u  %3u%%  %4.0fWh",
                 i+1, (unsigned)dmv, (unsigned)packs[i].cycles,
                 (unsigned)soh, avWh);
        _gfx->setTextColor(C_TEXT);
        _gfx->print(row);

        _gfx->setTextColor(hc);
        _gfx->setCursor(276, ry);
        _gfx->print(stag);
    }

    // Worst-pack recommendation
    if (worstPack != 0xFF && lowestSoH < 90) {
        uint16_t dmv = packs[worstPack].valid
                       ? (uint16_t)((packs[worstPack].cellHigh - packs[worstPack].cellLow) * 1000.0f)
                       : 0;
        int replace = 200 - (int)dmv * 2;
        if (replace < 0) replace = 0;
        char rec[44];
        snprintf(rec, sizeof(rec), "P%u worst: %u%% SoH, ~%d cyc remaining",
                 worstPack + 1, (unsigned)lowestSoH, replace);
        _gfx->setTextColor(C_WARN);
        _gfx->setCursor(4, 120);
        _gfx->print(rec);
    }

    // Design-capacity reference
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 134);
    _gfx->print("New BD design: 460.8Wh  Per-cell V: N/A (internal)");

    drawPageDots();
}

// ── Charge-done flash overlay ─────────────────────────────────────────────────
static void applyAlertOverlay() {
    // Detect transitions to chargeDone
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        bool done = packs[i].valid && packs[i].chargeDone;
        if (done && !_prevChargeDone[i]) _alertEnd = millis() + 1500;
        _prevChargeDone[i] = done;
    }
    if (millis() < _alertEnd) {
        bool flashOn = ((millis() - (_alertEnd - 1500)) / 250) % 2;
        if (flashOn) {
            _gfx->drawRect(0, 0, 320, 170, 0xFFFF);
            _gfx->drawRect(1, 1, 318, 168, 0xFFFF);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void displayInit() {
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);

    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);
    pinMode(BUTTON3_PIN, INPUT_PULLUP);
    _b1Prev = _b2Prev = _b3Prev = HIGH;
    _b1Ts   = _b2Ts   = _b3Ts   = 0;

    memset(_prevChargeDone,  0, sizeof(_prevChargeDone));
    memset(_portLostMs,      0, sizeof(_portLostMs));
    memset(_portWasValid,    0, sizeof(_portWasValid));
    _showDisconnect = false;
    _showLabelPick  = false;

    _bus = new Arduino_ESP32PAR8Q(
        TFT_DC, TFT_CS, TFT_WR, TFT_RD,
        TFT_D0, TFT_D1, TFT_D2, TFT_D3,
        TFT_D4, TFT_D5, TFT_D6, TFT_D7
    );
    _gfx = new Arduino_ST7789(_bus, TFT_RST,
                               1, true, 170, 320, 35, 0, 35, 0);
    _gfx->begin();
    _gfx->fillScreen(BLACK);

    // Palette — matches plan spec
    C_BG     = _gfx->color565( 13,  13,  13);
    C_GOOD   = _gfx->color565(  0, 230, 118);
    C_WARN   = _gfx->color565(255, 202,  40);
    C_POOR   = _gfx->color565(255,  82,  82);
    C_CHARGE = _gfx->color565(  0, 188, 212);
    C_TEXT   = _gfx->color565(224, 224, 224);
    C_DIM    = _gfx->color565( 80,  80,  80);
    C_ACCENT = _gfx->color565( 68, 170, 255);
    C_HDR    = _gfx->color565( 14,  14,  28);

    _screen     = 0;
    _detailPack = 0;
    _alertEnd   = 0;
    _dispLast   = 0;

    Serial.println("[DISP] ready 320x170, 4 screens");
}

// ── Disconnect detection ──────────────────────────────────────────────────────
static void checkDisconnects(uint32_t now) {
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (packs[i].valid) {
            _portWasValid[i] = true;
            _portLostMs[i]   = 0;
        } else if (_portWasValid[i]) {
            if (_portLostMs[i] == 0) _portLostMs[i] = now;
            if ((now - _portLostMs[i]) > DISCONNECT_DEBOUNCE_MS) {
                // Confirmed real disconnect after 30 s
                _portWasValid[i] = false;
                _portLostMs[i]   = 0;
                if (!_showDisconnect && !_showLabelPick) {
                    _showDisconnect = true;
                    _disconnectPort = i;
                    _labelPickVal   = labelGet(i);  // pre-fill with current label
                }
            }
        }
    }
}

void displayLoop() {
    uint32_t now = millis();

    // ── Disconnect detection (runs always, independent of overlays)
    checkDisconnects(now);

    // ── Button reading
    bool b1 = digitalRead(BUTTON1_PIN);
    bool b2 = digitalRead(BUTTON2_PIN);
    bool b3 = digitalRead(BUTTON3_PIN);

    // ── Overlay: label picker (highest priority — consumes all buttons)
    if (_showLabelPick) {
        // BTN1 — cycle label 0 → 1 → … → 8 → 0
        if (_b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
            _b1Ts = now;
            _labelPickVal = (_labelPickVal >= NUM_LABELS) ? 0 : _labelPickVal + 1;
        }
        // BTN2 — confirm
        if (_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) {
            _b2Ts = now;
            labelSet(_labelPickPort, _labelPickVal);
            _showLabelPick = false;
            _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);  // force redraw
        }
        // BTN3 — cancel
        if (_b3Prev == HIGH && b3 == LOW && (now - _b3Ts) > DEBOUNCE_MS) {
            _b3Ts = now;
            _showLabelPick = false;
            _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
        }
        _b1Prev = b1; _b2Prev = b2; _b3Prev = b3;
        drawLabelPicker();
        return;
    }

    // ── Overlay: disconnect modal
    if (_showDisconnect) {
        // BTN1 — yes, assign new label
        if (_b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
            _b1Ts = now;
            _showDisconnect = false;
            _showLabelPick  = true;
            _labelPickPort  = _disconnectPort;
            // _labelPickVal already set when disconnect was triggered
        }
        // BTN2 or BTN3 — dismiss
        if ((_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) ||
            (_b3Prev == HIGH && b3 == LOW && (now - _b3Ts) > DEBOUNCE_MS)) {
            _b2Ts = _b3Ts = now;
            _showDisconnect = false;
            _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
        }
        _b1Prev = b1; _b2Prev = b2; _b3Prev = b3;
        drawDisconnectModal();
        return;
    }

    // ── Normal button handling
    // BTN1 — WiFi toggle (screen 0) or cycle pack (screen 1)
    if (_b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
        _b1Ts = now;
        if (_screen == 0)      wifiToggle();
        else if (_screen == 1) _detailPack = (_detailPack + 1) % NUM_PACKS;
    }
    _b1Prev = b1;

    // BTN2 — next screen →
    if (_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) {
        _b2Ts   = now;
        _screen = (_screen + 1) % NUM_SCREENS;
    }
    _b2Prev = b2;

    // BTN3 — prev screen ← (short press) OR label assign (long press on screen 0)
    if (b3 == LOW && _b3Prev == HIGH) _b3Ts = now;  // record press start
    if (_b3Prev == LOW && b3 == HIGH) {              // released
        uint32_t held = now - _b3Ts;
        if (held >= LONGPRESS_MS && _screen == 0) {
            // Long press on fleet screen → label assignment for port 0
            _showLabelPick = true;
            _labelPickPort = 0;
            _labelPickVal  = labelGet(0);
        } else if (held >= DEBOUNCE_MS && held < LONGPRESS_MS) {
            _screen = (_screen + NUM_SCREENS - 1) % NUM_SCREENS;
        }
    }
    _b3Prev = b3;

    // ── Charge-done alert (every loop for responsive flash)
    applyAlertOverlay();

    // ── Periodic screen refresh
    if (now - _dispLast < DISPLAY_REFRESH_MS) return;
    _dispLast = now;

    switch (_screen) {
        case 0: drawScreenFleet();    break;
        case 1: drawScreenDetail();   break;
        case 2: drawScreenCharging(); break;
        case 3: drawScreenHealth();   break;
    }

    applyAlertOverlay();
}
