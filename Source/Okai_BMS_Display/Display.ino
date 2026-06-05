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
#include <canvas/Arduino_Canvas.h>

// ── Palette ───────────────────────────────────────────────────────────────────
static uint16_t C_BG, C_GOOD, C_WARN, C_POOR, C_CHARGE,
                C_TEXT, C_DIM, C_ACCENT, C_HDR, C_NODATA;

// ── Display objects ───────────────────────────────────────────────────────────
static Arduino_DataBus *_bus;
static Arduino_GFX     *_hw;      // hardware ST7789 (canvas output)
static Arduino_Canvas  *_canvas;  // 320×170 RGB565 framebuffer in PSRAM
static Arduino_GFX     *_gfx;    // = _canvas — all draw calls target this

// ── Layout ────────────────────────────────────────────────────────────────────
#define HDR_H     16
#define CELL_W   160
#define CELL_H    77   // (170 - HDR_H) / 2 — dots moved into header, full height used

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
static float    gLocalBatV  = 0.0f;     // onboard 18650 voltage (GPIO4 ADC × 2 divider)

// ── Button debounce ───────────────────────────────────────────────────────────
static bool     _b1Prev, _b2Prev, _b3Prev;
static uint32_t _b1Ts, _b2Ts, _b3Ts;
#define DEBOUNCE_MS   50UL
#define LONGPRESS_MS 800UL   // hold BTN3 to enter label assign

// Set true by displaySleepOverlay() while sleep countdown is active.
// displayLoop() yields the display to PowerManager while this is true.
bool gSleepCountdownActive = false;

bool gLightOn = false;   // FET output state (GPIO13)

// ── Refresh timing ────────────────────────────────────────────────────────────
static uint32_t _dispLast;

// ── Pack disconnect tracking ──────────────────────────────────────────────────
#define DISCONNECT_DEBOUNCE_MS 120000UL  // 2 min — filters brief glitches and short stops
static uint32_t _portLostMs[NUM_PACKS];
static bool     _portWasValid[NUM_PACKS];

// ── Overlay state ─────────────────────────────────────────────────────────────
// Only one overlay active at a time: disconnect modal OR label picker
static bool    _showDisconnect = false;
static uint8_t _disconnectPort = 0;

static bool    _showLabelPick  = false;
static uint8_t _labelPickPort  = 0;
static uint8_t _labelPickVal   = 0;   // 0=unassigned, 1-8=label

// ── Alternating display phase (5 s primary / 3 s secondary) ─────────────────
#define ALT_A_MS 5000UL
#define ALT_B_MS 3000UL
static inline bool altPhaseA() {
    return (millis() % (ALT_A_MS + ALT_B_MS)) < ALT_A_MS;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint16_t healthColor(uint8_t i) {
    if (!packs[i].valid) return C_NODATA;
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

// drawPageDots() removed — dots now live in drawHeader() top-right

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

// ── Onboard 18650 battery sense ───────────────────────────────────────────────
// LilyGo T-Display-S3: battery on GPIO4 via a 2:1 divider. analogReadMilliVolts()
// returns eFuse-calibrated mV at the pin; ×2 recovers cell voltage. 4-sample average
// to settle ADC noise. Read-only — touches no other subsystem.
static void readLocalBattery() {
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    gLocalBatV = (float)(mv >> 2) * 2.0f / 1000.0f;
}

// ── Header bar (includes nav dots top-right) ─────────────────────────────────
static void drawHeader() {
    _gfx->fillRect(0, 0, 320, HDR_H, C_HDR);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(2, 4);
    _gfx->print("OKAI BMS");

    LogMode lm = logCurrentMode();
    const char *modeTag = (lm == LOG_RIDE) ? " RDE" : (lm == LOG_CHARGE) ? " CHG" : "";
    char wstr[14];
    snprintf(wstr, sizeof(wstr), "%s%s", wifiActive ? "W:ON" : "W:OFF", modeTag);
    _gfx->setCursor(70, 4);
    _gfx->setTextColor(wifiActive ? C_GOOD : C_DIM);
    _gfx->print(wstr);

    // Light state indicator
    _gfx->setCursor(210, 4);
    if (gLightOn) {
        _gfx->setTextColor(C_GOOD);
        _gfx->print("LGT");
    } else {
        _gfx->setTextColor(C_DIM);
        _gfx->print("lgt");
    }

    // ── Onboard 18650 battery — icon body centred on x=160 (bar midpoint) ──
    {
        int pct = (int)constrain((gLocalBatV - 3.0f) / 1.2f * 100.0f, 0.0f, 100.0f);
        uint16_t bc = (gLocalBatV < 0.5f) ? C_DIM :
                      (pct > 60) ? C_GOOD : (pct > 30) ? C_WARN : C_POOR;
        const int BX = 132, BY = 3, BW = 22, BH = 10;       // icon+readout pair centred on x=160
        _gfx->drawRect(BX, BY, BW, BH, C_DIM);              // outline
        _gfx->fillRect(BX + BW, BY + 3, 2, BH - 6, C_DIM);  // nub
        int fill = ((BW - 2) * pct) / 100;
        if (fill > 0) _gfx->fillRect(BX + 1, BY + 1, fill, BH - 2, bc);  // charge fill

        char vb[10];
        bool showPct = !((millis() / 5000) & 1);            // alternate %/voltage every 5 s
        if (gLocalBatV < 0.5f) snprintf(vb, sizeof(vb), "--");
        else if (showPct)      snprintf(vb, sizeof(vb), "%d%%", pct);
        else                   snprintf(vb, sizeof(vb), "%.2fv", gLocalBatV);
        _gfx->setTextSize(1);
        _gfx->setTextColor(bc);
        _gfx->setCursor(BX + BW + 4, 4);                    // readout just right of icon
        _gfx->print(vb);
    }

    // Nav dots in header right side (replaces bottom dot strip)
    for (uint8_t i = 0; i < NUM_SCREENS; i++) {
        uint16_t dx = 256 + i * 14;
        if (i == _screen) _gfx->fillCircle(dx, 8, 3, C_ACCENT);
        else              _gfx->drawCircle(dx, 8, 3, C_DIM);
    }
}

// ── Screen 0: Fleet Overview ──────────────────────────────────────────────────
// Cell layout (160×77 px, size3=18×24, size2=12×16):
//   Row1 cy+3:  P# (size2) + GOOD/WARN/POOR (size2) right
//   Row2 cy+23: Alt-A→ SOC% size3 left + voltage size2 right
//               Alt-B→ current size3 left + delta/temp size2 right
//   Row3 cy+55: Alt-A→ CYC or ~Wh dim size2
//               Alt-B→ (blank — current already fills row2)
static void drawFleetCell(uint8_t i) {
    uint16_t cx = CX[i], cy = CY[i];
    uint16_t hc = healthColor(i);
    bool phA = altPhaseA();

    _gfx->fillRect(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, C_BG);
    _gfx->drawRect(cx,     cy,     CELL_W,     CELL_H,     hc);
    _gfx->drawRect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, hc);

    _gfx->setTextSize(2);

    if (!packs[i].valid) {
        _gfx->setTextColor(C_NODATA);
        _gfx->setCursor(cx + 4, cy + 3);
        char plbl[4]; snprintf(plbl, sizeof(plbl), "P%u", i + 1);
        _gfx->print(plbl);
        _gfx->setCursor(cx + 4, cy + 28);
        _gfx->print("NO DATA");
        return;
    }

    // Row 1 — P# left, health tag right (size2)
    float delta = packs[i].cellHigh - packs[i].cellLow;
    const char *htag = (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                       (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
    char plbl[4]; snprintf(plbl, sizeof(plbl), "P%u", i + 1);
    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(cx + 4, cy + 3);
    _gfx->print(plbl);
    _gfx->setTextColor(hc);
    _gfx->setCursor(cx + CELL_W - 52, cy + 3);  // 4ch×12+4px margin
    _gfx->print(htag);

    // Row 2 — primary value size3 (left) + secondary size2 (right)
    uint16_t dmv = (uint16_t)(delta * 1000.0f + 0.5f);
    if (phA) {
        // SOC% big left, voltage smaller right
        char soc_s[5]; snprintf(soc_s, sizeof(soc_s), "%u%%", (unsigned)packs[i].soc);
        _gfx->setTextSize(3);
        _gfx->setTextColor(hc);
        _gfx->setCursor(cx + 4, cy + 23);
        _gfx->print(soc_s);
        char v_s[8]; snprintf(v_s, sizeof(v_s), "%.1fv", packs[i].voltage);
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_ACCENT);
        _gfx->setCursor(cx + CELL_W - (int16_t)strlen(v_s) * 12 - 4, cy + 31);
        _gfx->print(v_s);
        // Row 3 — CYC or Wh dim
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(cx + 4, cy + 58);
        char r3[14];
        if (logCurrentMode() == LOG_RIDE) {
            snprintf(r3, sizeof(r3), "~%.0fWh", (packs[i].soc / 100.0f) * PACK_DESIGN_WH);
        } else {
            snprintf(r3, sizeof(r3), "CYC%u", (unsigned)packs[i].cycles);
        }
        _gfx->print(r3);
    } else {
        // Current big left, delta+temp smaller right
        char a_s[8]; snprintf(a_s, sizeof(a_s), "%+.1fA", packs[i].current);
        _gfx->setTextSize(3);
        _gfx->setTextColor(C_ACCENT);
        _gfx->setCursor(cx + 4, cy + 23);
        _gfx->print(a_s);
        char dt_s[14]; snprintf(dt_s, sizeof(dt_s), "d%u %u*C", dmv, (unsigned)packs[i].maxTemp);
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_TEXT);
        _gfx->setCursor(cx + 4, cy + 58);
        _gfx->print(dt_s);
    }
}

// ── Empty-cell summary helpers ────────────────────────────────────────────────

static void _summaryTimeStr(char* out, size_t len, float totalWh) {
    bool riding = (logCurrentMode() == LOG_RIDE);
    bool ready  = (g_ridePowerN >= 3 && g_ridePowerEma_W >= 20.0f);
    if (!riding)  { snprintf(out, len, "Fleet info"); return; }
    if (!ready)   { snprintf(out, len, "~-- min");    return; }
    uint16_t m = (uint16_t)(totalWh / g_ridePowerEma_W * 60.0f + 0.5f);
    if (m >= 60) snprintf(out, len, "~%uh%02um", m/60, m%60);
    else         snprintf(out, len, "~%u min",   m);
}

// Wide panel — fills a full 320px row (both cells in a row empty)
static void _drawSummaryWide(uint16_t ry, uint8_t nValid,
                              float totalWh, float totalAh) {
    bool riding = (logCurrentMode() == LOG_RIDE);
    bool ready  = (g_ridePowerN >= 3 && g_ridePowerEma_W >= 20.0f);

    _gfx->fillRect(0, ry, 320, CELL_H, C_BG);
    _gfx->drawRect(0, ry, 320, CELL_H, C_ACCENT);
    _gfx->drawRect(1, ry+1, 318, CELL_H-2, C_ACCENT);

    // Left half — time estimate
    char ts[16]; _summaryTimeStr(ts, sizeof(ts), totalWh);
    _gfx->setTextSize(2);
    _gfx->setTextColor(riding && ready ? C_GOOD : (riding ? C_DIM : C_ACCENT));
    _gfx->setCursor(6, ry + 4);
    _gfx->print(ts);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(6, ry + 26);
    if (riding && ready) {
        char pw[30];
        snprintf(pw, sizeof(pw), "@ %.0f W avg (%u samples)", g_ridePowerEma_W, (unsigned)g_ridePowerN);
        _gfx->print(pw);
    } else if (riding) {
        _gfx->print("(measuring power draw...)");
    } else {
        _gfx->print("start riding for estimate");
    }

    // Divider
    _gfx->drawFastVLine(160, ry + 6, CELL_H - 12, C_DIM);

    // Right half — capacity breakdown
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(166, ry + 4);
    char hdr[16]; snprintf(hdr, sizeof(hdr), "%uP capacity:", nValid);
    _gfx->print(hdr);

    _gfx->setTextColor(C_GOOD);
    _gfx->setCursor(166, ry + 16);
    char wh[20]; snprintf(wh, sizeof(wh), "%.0f Wh left", totalWh);
    _gfx->print(wh);

    _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(166, ry + 28);
    char mah[22]; snprintf(mah, sizeof(mah), "%.0f mAh left", totalAh * 1000.0f);
    _gfx->print(mah);

    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(166, ry + 40);
    char des[32];
    snprintf(des, sizeof(des), "Design: %.0fWh/%.0fmAh",
             nValid * PACK_DESIGN_WH, nValid * PACK_DESIGN_AH * 1000.0f);
    _gfx->print(des);

    uint16_t sumSoc = 0;
    for (uint8_t j = 0; j < NUM_PACKS; j++) if (packs[j].valid) sumSoc += packs[j].soc;
    _gfx->setCursor(166, ry + 54);
    char av[16]; snprintf(av, sizeof(av), "avg SoC: %u%%", nValid ? sumSoc/nValid : 0);
    _gfx->print(av);
}

// Narrow panel — fills one 160×77 cell slot
static void _drawSummaryNarrow(uint16_t cx, uint16_t cy, uint8_t nValid,
                                float totalWh, float totalAh) {
    bool riding = (logCurrentMode() == LOG_RIDE);
    bool ready  = (g_ridePowerN >= 3 && g_ridePowerEma_W >= 20.0f);

    _gfx->fillRect(cx, cy, CELL_W, CELL_H, C_BG);
    _gfx->drawRect(cx, cy, CELL_W, CELL_H, C_ACCENT);
    _gfx->drawRect(cx+1, cy+1, CELL_W-2, CELL_H-2, C_ACCENT);

    char ts[16]; _summaryTimeStr(ts, sizeof(ts), totalWh);
    _gfx->setTextSize(2);
    _gfx->setTextColor(riding && ready ? C_GOOD : (riding ? C_DIM : C_ACCENT));
    _gfx->setCursor(cx + 4, cy + 4);
    _gfx->print(ts);

    _gfx->setTextColor(C_GOOD);
    _gfx->setCursor(cx + 4, cy + 26);
    char wh[14]; snprintf(wh, sizeof(wh), "%.0fWh left", totalWh);
    _gfx->print(wh);

    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(cx + 4, cy + 50);
    _gfx->setTextSize(1);
    if (riding && ready) {
        char pw[16]; snprintf(pw, sizeof(pw), "@ %.0fW avg", g_ridePowerEma_W);
        _gfx->print(pw);
    } else {
        char np[14]; snprintf(np, sizeof(np), "%uP of %u ports", nValid, NUM_PACKS);
        _gfx->print(np);
    }
}

// ── Screen 0: Fleet overview with adaptive empty-cell summary ─────────────────
static void drawScreenFleet() {
    drawHeader();

    // Compute fleet totals once — shared by cells and summary panels
    float totalWh = 0.0f, totalAh = 0.0f;
    uint8_t nValid = 0;
    for (uint8_t j = 0; j < NUM_PACKS; j++) {
        if (!packs[j].valid) continue;
        totalWh += (packs[j].soc / 100.0f) * PACK_DESIGN_WH;
        totalAh += (packs[j].soc / 100.0f) * PACK_DESIGN_AH;
        nValid++;
    }

    // Draw valid pack cells
    for (uint8_t i = 0; i < NUM_PACKS; i++)
        if (packs[i].valid) drawFleetCell(i);

    // Fill empty cell space with fleet summary
    if (nValid == 0) {
        _gfx->fillRect(0, HDR_H, 320, 2 * CELL_H, C_BG);
        _gfx->setTextSize(2); _gfx->setTextColor(C_NODATA);
        _gfx->setCursor(72, 84); _gfx->print("No packs");
    } else if (nValid < NUM_PACKS) {
        bool e0 = !packs[0].valid, e1 = !packs[1].valid;
        bool e2 = !packs[2].valid, e3 = !packs[3].valid;

        if (e2 && e3 && !e0 && !e1) {
            _drawSummaryWide(CY[2], nValid, totalWh, totalAh);   // bottom row free
        } else if (e0 && e1 && !e2 && !e3) {
            _drawSummaryWide(CY[0], nValid, totalWh, totalAh);   // top row free
        } else {
            // Mixed or single empty — narrow panel per empty slot
            for (uint8_t i = 0; i < NUM_PACKS; i++)
                if (!packs[i].valid)
                    _drawSummaryNarrow(CX[i], CY[i], nValid, totalWh, totalAh);
        }
    }

}

// ── Screen 1: Per-pack Detail ─────────────────────────────────────────────────
// Layout (y positions, full 154px content area below header):
//   y22  P# label + pack selector dots (size1 nav UI)
//   y36  SOC% size4 hero (32px tall, ends y68)
//   y72  Alt-A→ V + A in size3  |  Alt-B→ delta + temp in size3
//   y100 Alt-A→ power + Wh size2  |  Alt-B→ CYC + SoH size2
//   y118 Alt-A→ charger status size2 (if present)  |  Alt-B→ sessions size2
static void drawScreenDetail() {
    uint8_t i   = _detailPack;
    uint16_t hc = healthColor(i);
    bool phA    = altPhaseA();

    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();

    // P# + pack selector dots (size1 — navigation chrome)
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

    // Health tag top-right (size1)
    float delta = packs[i].valid ? (packs[i].cellHigh - packs[i].cellLow) : 0.0f;
    const char *htag = !packs[i].valid ? "----" :
                       (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                       (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
    _gfx->setTextColor(hc);
    _gfx->setCursor(280, 22);
    _gfx->print(htag);

    if (!packs[i].valid) {
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_NODATA);
        _gfx->setCursor(80, 88);
        _gfx->print("NO DATA");
        return;
    }

    // SOC — size4 (24×32px) centred
    char soc_s[6];
    snprintf(soc_s, sizeof(soc_s), "%u%%", (unsigned)packs[i].soc);
    int16_t soc_w = (int16_t)strlen(soc_s) * 24;
    _gfx->setTextSize(4);
    _gfx->setTextColor(hc);
    _gfx->setCursor((320 - soc_w) / 2, 36);
    _gfx->print(soc_s);

    uint16_t dmv = (uint16_t)(delta * 1000.0f + 0.5f);
    uint8_t  soh = sohEstimate(i);
    _gfx->setTextSize(3);   // size3 = 18×24px for the primary alternating row

    if (phA) {
        // Alt A (5s) — voltage + current big, power + Wh below
        char va[20];
        snprintf(va, sizeof(va), "%.2fV %+.2fA", packs[i].voltage, packs[i].current);
        _gfx->setTextColor(C_ACCENT);
        _gfx->setCursor(4, 72);
        _gfx->print(va);

        float powerW  = packs[i].voltage * packs[i].current;
        float availWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        char pw[20];
        snprintf(pw, sizeof(pw), "%+.0fW  %.0fWh", powerW, availWh);
        _gfx->setTextSize(2);
        _gfx->setTextColor(packs[i].current >= 0 ? C_CHARGE : C_ACCENT);
        _gfx->setCursor(4, 100);
        _gfx->print(pw);

        if (packs[i].chargerDetected) {
            _gfx->setTextColor(packs[i].chargeDone ? C_GOOD : C_CHARGE);
            _gfx->setCursor(4, 118);
            _gfx->print(packs[i].chargeDone ? "Charge done" : "Charging...");
        }
    } else {
        // Alt B (3s) — delta + temp big, CYC + SoH + sessions below
        char dt[16];
        snprintf(dt, sizeof(dt), "d%umV  %u*C", dmv, (unsigned)packs[i].maxTemp);
        _gfx->setTextColor(C_TEXT);
        _gfx->setCursor(4, 72);
        _gfx->print(dt);

        char cs[18];
        snprintf(cs, sizeof(cs), "CYC%u  SoH%u%%", (unsigned)packs[i].cycles, (unsigned)soh);
        _gfx->setTextSize(2);
        _gfx->setTextColor(soh >= 80 ? C_GOOD : soh >= 60 ? C_WARN : C_POOR);
        _gfx->setCursor(4, 100);
        _gfx->print(cs);

        char sess[22];
        snprintf(sess, sizeof(sess), "+%.1fWh / -%.1fWh", packs[i].whIn, packs[i].whOut);
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(4, 118);
        _gfx->print(sess);
    }
}

// ── Screen 2a: Ride Energy — time left + fleet Wh/mAh ────────────────────────
static void drawScreenRideEnergy() {
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();
    bool phA = altPhaseA();

    float totalWh = 0.0f, totalAh = 0.0f;
    uint8_t n = 0;
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        totalWh += (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        totalAh += (packs[i].soc / 100.0f) * PACK_DESIGN_AH;
        n++;
    }

    // Time estimate size3 (important!)
    bool ready = (g_ridePowerN >= 3 && g_ridePowerEma_W >= 20.0f);
    char timeStr[20];
    if (!ready) {
        strcpy(timeStr, "~-- min");
        _gfx->setTextColor(C_DIM);
    } else {
        uint16_t mins = (uint16_t)(totalWh / g_ridePowerEma_W * 60.0f + 0.5f);
        if (mins >= 60) snprintf(timeStr, sizeof(timeStr), "~%uh%02um", mins/60, mins%60);
        else            snprintf(timeStr, sizeof(timeStr), "~%u min", mins);
        _gfx->setTextColor(C_GOOD);
    }
    _gfx->setTextSize(3);
    _gfx->setCursor(4, 20);
    _gfx->print(timeStr);

    // Power note size1 dim
    _gfx->setTextSize(1);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 48);
    if (ready) {
        char pw[32]; snprintf(pw, sizeof(pw), "@ %.0fW avg (%u samples)", g_ridePowerEma_W, (unsigned)g_ridePowerN);
        _gfx->print(pw);
    } else {
        _gfx->print("(measuring power draw...)");
    }

    // Per-pack rows size2, 20px pitch
    _gfx->setTextSize(2);
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        uint16_t ry = 60 + i * 20;
        _gfx->setCursor(4, ry);
        if (!packs[i].valid) {
            _gfx->setTextColor(C_NODATA);
            char row[10]; snprintf(row, sizeof(row), "P%u  ---", i+1);
            _gfx->print(row);
            continue;
        }
        _gfx->setTextColor(C_TEXT);
        char row[20];
        if (phA) {
            float wh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
            snprintf(row, sizeof(row), "P%u %3u%% %4.0fWh", i+1, (unsigned)packs[i].soc, wh);
        } else {
            float ah = (packs[i].soc / 100.0f) * PACK_DESIGN_AH;
            snprintf(row, sizeof(row), "P%u %3u%% %.1fAh", i+1, (unsigned)packs[i].soc, ah);
        }
        _gfx->print(row);
    }

    // Fleet total
    _gfx->setTextColor(C_GOOD);
    _gfx->setCursor(4, 60 + NUM_PACKS * 20 + 4);
    char tot[22];
    if (phA) snprintf(tot, sizeof(tot), "Fleet %uP: %.0fWh", n, totalWh);
    else     snprintf(tot, sizeof(tot), "Fleet %uP: %.1fAh", n, totalAh);
    _gfx->print(tot);
}

// ── Screen 2: Charging Live ───────────────────────────────────────────────────
// Row pitch 30px: 16px size2 label + 10px bar + 4px gap
static void drawScreenCharging() {
    if (logCurrentMode() == LOG_RIDE) { drawScreenRideEnergy(); return; }
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();

    bool anyCharger = false;
    uint8_t donePacks = 0;
    float totalW = 0.0f;
    bool blinkOn = (millis() / 250) % 2;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (packs[i].valid && packs[i].chargerDetected) anyCharger = true;
        if (packs[i].valid && packs[i].chargeDone)      donePacks++;
        if (packs[i].valid && packs[i].isCharging)      totalW += packs[i].voltage * packs[i].current;
    }

    if (!anyCharger) {
        _gfx->setTextSize(2);
        _gfx->setTextColor(C_NODATA);
        _gfx->setCursor(60, 84);
        _gfx->print("No charger");
        return;
    }

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        uint16_t ry = 22 + i * 30;
        bool hasData = packs[i].valid && packs[i].chargerDetected;

        _gfx->setTextSize(2);
        _gfx->setTextColor(C_TEXT);
        _gfx->setCursor(4, ry);
        if (!hasData) {
            char pl[10]; snprintf(pl, sizeof(pl), "P%u  ---", i+1);
            _gfx->setTextColor(C_NODATA);
            _gfx->print(pl);
            continue;
        }

        // "P1 87%" size2
        char lbl[10]; snprintf(lbl, sizeof(lbl), "P%u %u%%", i+1, (unsigned)packs[i].soc);
        _gfx->print(lbl);

        // ETA / DONE right-aligned size2
        if (packs[i].chargeDone) {
            _gfx->setTextColor(C_GOOD);
            _gfx->setCursor(256, ry);
            _gfx->print("DONE");
        } else if (packs[i].isCharging && packs[i].current > 0.05f) {
            float etaMin = ((100.0f - packs[i].soc) / 100.0f)
                           * PACK_DESIGN_AH / packs[i].current * 60.0f;
            uint16_t eta = (uint16_t)(etaMin + 0.5f);
            char eta_s[8];
            if (eta >= 60) snprintf(eta_s, sizeof(eta_s), "%uh%02um", eta/60, eta%60);
            else           snprintf(eta_s, sizeof(eta_s), "%um", eta);
            _gfx->setTextColor(C_CHARGE);
            uint16_t ex = 316 - (uint16_t)strlen(eta_s) * 12;
            _gfx->setCursor(ex, ry);
            _gfx->print(eta_s);
        }

        // Fill bar below label
        uint16_t by     = ry + 17;
        uint16_t filled = (uint16_t)((uint32_t)packs[i].soc * BAR_W / 100);
        uint16_t barClr = packs[i].chargeDone ? C_GOOD : C_CHARGE;
        _gfx->fillRect(BAR_X,          by, filled,         BAR_H, barClr);
        _gfx->fillRect(BAR_X + filled, by, BAR_W - filled, BAR_H, C_DIM);
        _gfx->drawRect(BAR_X - 1, by - 1, BAR_W + 2, BAR_H + 2, C_DIM);
        if (packs[i].isCharging && blinkOn && filled < BAR_W)
            _gfx->drawFastVLine(BAR_X + filled, by, BAR_H, 0xFFFF);
    }

    // Summary size2
    _gfx->setTextSize(2);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(4, 22 + NUM_PACKS * 30 + 4);
    char sum[24];
    snprintf(sum, sizeof(sum), "%.0fW  Done:%u/%u", totalW, (unsigned)donePacks, (unsigned)NUM_PACKS);
    _gfx->print(sum);
}

// ── Screen 3: Cell Health / Energy ───────────────────────────────────────────
// Alt-A (5s): P#  SOC%  Wh  GOOD/WARN/POOR
// Alt-B (3s): P#  delta  CYC  SoH%
// Row pitch 24px (16px text + 8px gap), 4 rows from y=44 to y=140
static void drawScreenHealth() {
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);
    drawHeader();
    bool phA = altPhaseA();

    // Title size2
    _gfx->setTextSize(2);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(4, 20);
    _gfx->print(phA ? "FLEET HEALTH" : "CELL DETAIL");

    uint8_t worstPack = 0xFF;
    uint8_t lowestSoH = 255;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        uint16_t ry = 44 + i * 24;
        uint16_t hc = healthColor(i);
        _gfx->setTextSize(2);
        _gfx->setCursor(4, ry);

        if (!packs[i].valid) {
            _gfx->setTextColor(C_NODATA);
            char row[14]; snprintf(row, sizeof(row), "P%u  NO DATA", i+1);
            _gfx->print(row);
            continue;
        }

        uint8_t  soh = sohEstimate(i);
        uint16_t dmv = (uint16_t)((packs[i].cellHigh - packs[i].cellLow) * 1000.0f + 0.5f);
        float   avWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        const char *stag = (dmv >= (uint16_t)(CELL_DELTA_POOR_V * 1000.0f)) ? "POOR" :
                           (dmv >= (uint16_t)(CELL_DELTA_WARN_V * 1000.0f)) ? "WARN" : "GOOD";
        if (soh < lowestSoH) { lowestSoH = soh; worstPack = i; }

        _gfx->setTextColor(hc);
        char row[26];
        if (phA) {
            // P1  87%  401Wh  GOOD
            snprintf(row, sizeof(row), "P%u %3u%% %4.0fWh %s",
                     i+1, (unsigned)packs[i].soc, avWh, stag);
        } else {
            // P1  d43mV  8229c  97%
            snprintf(row, sizeof(row), "P%u d%umV %uc %u%%",
                     i+1, (unsigned)dmv, (unsigned)packs[i].cycles, (unsigned)soh);
        }
        _gfx->print(row);
    }

    // Worst-pack note size1 dim
    _gfx->setTextSize(1);
    if (worstPack != 0xFF && lowestSoH < 90) {
        char rec[44];
        snprintf(rec, sizeof(rec), "P%u lowest SoH: %u%%  (NCR18650BD ref)",
                 worstPack + 1, (unsigned)lowestSoH);
        _gfx->setTextColor(C_WARN);
        _gfx->setCursor(4, 148);
        _gfx->print(rec);
    } else {
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(4, 148);
        _gfx->print("Design: 460.8Wh  Ref: NCR18650BD");
    }
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

    pinMode(LIGHT_FET_PIN, OUTPUT);
    digitalWrite(LIGHT_FET_PIN, LOW);

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
    _hw = new Arduino_ST7789(_bus, TFT_RST,
                              3, true, 170, 320, 35, 0, 35, 0);
    if (!psramFound()) Serial.println("[DISP] WARNING: no PSRAM — canvas falls back to heap, WiFi may OOM");
    _canvas = new Arduino_Canvas(320, 170, _hw);
    if (!_canvas) { Serial.println("[DISP] FATAL: canvas alloc failed"); while (1) delay(1000); }
    _canvas->begin();
    _gfx = _canvas;
    _gfx->fillScreen(0);

    // Palette — matches plan spec
    C_BG     = _gfx->color565( 13,  13,  13);
    C_GOOD   = _gfx->color565(  0, 230, 118);
    C_WARN   = _gfx->color565(255, 202,  40);
    C_POOR   = _gfx->color565(255,  82,  82);
    C_CHARGE = _gfx->color565(  0, 188, 212);
    C_TEXT   = _gfx->color565(224, 224, 224);
    C_DIM    = _gfx->color565(160, 160, 160);
    C_NODATA = _gfx->color565(100, 220, 220);
    C_ACCENT = _gfx->color565( 68, 170, 255);
    C_HDR    = _gfx->color565( 14,  14,  28);

    _screen     = 0;
    _detailPack = 0;
    _alertEnd   = 0;
    _dispLast   = 0;

    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    readLocalBattery();

    _canvas->flush();
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
    if (gSleepCountdownActive) return;  // PowerManager owns the display

    uint32_t now = millis();

    checkDisconnects(now);

    bool b1 = digitalRead(BUTTON1_PIN);
    bool b2 = digitalRead(BUTTON2_PIN);
    bool b3 = digitalRead(BUTTON3_PIN);

    // ── Overlay: label picker
    if (_showLabelPick) {
        if (_b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
            _b1Ts = now;
            _labelPickVal = (_labelPickVal >= NUM_LABELS) ? 0 : _labelPickVal + 1;
        }
        if (_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) {
            _b2Ts = now;
            labelSet(_labelPickPort, _labelPickVal);
            _showLabelPick = false;
            _dispLast = 0;  // force immediate full redraw on dismiss
        }
        if (_b3Prev == HIGH && b3 == LOW && (now - _b3Ts) > DEBOUNCE_MS) {
            _b3Ts = now;
            _showLabelPick = false;
            _dispLast = 0;
        }
        _b1Prev = b1; _b2Prev = b2; _b3Prev = b3;
        if (_showLabelPick) { drawLabelPicker(); _canvas->flush(); }
        return;
    }

    // ── Overlay: disconnect modal
    if (_showDisconnect) {
        if (_b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
            _b1Ts = now;
            _showDisconnect = false;
            _showLabelPick  = true;
            _labelPickPort  = _disconnectPort;
        }
        if ((_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) ||
            (_b3Prev == HIGH && b3 == LOW && (now - _b3Ts) > DEBOUNCE_MS)) {
            _b2Ts = _b3Ts = now;
            _showDisconnect = false;
            _dispLast = 0;
        }
        _b1Prev = b1; _b2Prev = b2; _b3Prev = b3;
        if (_showDisconnect) { drawDisconnectModal(); _canvas->flush(); }
        return;
    }

    // ── Normal button handling

    // BTN2+BTN3 combo: toggle light FET (checked first to suppress individual actions)
    static bool     _comboFired = false;
    static uint32_t _comboTs    = 0;
    if (b2 == LOW && b3 == LOW) {
        if (_comboTs == 0) _comboTs = now;
        if (!_comboFired && (now - _comboTs) >= DEBOUNCE_MS) {
            _comboFired = true;
            gLightOn    = !gLightOn;
            digitalWrite(LIGHT_FET_PIN, gLightOn ? HIGH : LOW);
            _b2Ts = now;  // reset BTN2 timer — suppresses screen-change on release
            _b3Ts = now;  // reset BTN3 timer — suppresses screen-change on release
        }
    } else {
        _comboTs    = 0;
        _comboFired = false;
    }

    // BTN1: record press time on falling edge, act on rising edge (release).
    // Ignores releases from a hold >= SLEEP_HOLD_MS — device sleeps before release.
    if (b1 == LOW && _b1Prev == HIGH) _b1Ts = now;
    if (_b1Prev == LOW && b1 == HIGH) {
        uint32_t held = now - _b1Ts;
        if (held >= DEBOUNCE_MS && held < SLEEP_HOLD_MS) {
            if (_screen == 0)      wifiToggle();
            else if (_screen == 1) _detailPack = (_detailPack + 1) % NUM_PACKS;
        }
    }
    _b1Prev = b1;

    // BTN2: next screen — only fires if BTN3 is not also pressed (prevents combo collision)
    if (_b2Prev == HIGH && b2 == LOW && b3 == HIGH && (now - _b2Ts) > DEBOUNCE_MS) {
        _b2Ts   = now;
        _screen = (_screen + 1) % NUM_SCREENS;
    }
    _b2Prev = b2;

    // BTN3: prev screen (short) / label assign (long on screen 0)
    // Release-based; _b3Ts reset during combo ensures release fires with held≈0 → no action.
    if (b3 == LOW && _b3Prev == HIGH && b2 == HIGH) _b3Ts = now;
    if (_b3Prev == LOW && b3 == HIGH) {
        uint32_t held = now - _b3Ts;
        if (held >= LONGPRESS_MS && _screen == 0) {
            _showLabelPick = true;
            _labelPickPort = 0;
            _labelPickVal  = labelGet(0);
        } else if (held >= DEBOUNCE_MS && held < LONGPRESS_MS) {
            _screen = (_screen + NUM_SCREENS - 1) % NUM_SCREENS;
        }
    }
    _b3Prev = b3;

    // Alert flashing needs 250 ms refresh; BMS data is fine at 500 ms
    bool alertActive = (_alertEnd && now < _alertEnd);
    uint32_t refreshMs = alertActive ? 250UL : DISPLAY_REFRESH_MS;

    if (now - _dispLast < refreshMs) return;
    _dispLast = now;

    readLocalBattery();

    switch (_screen) {
        case 0: drawScreenFleet();    break;
        case 1: drawScreenDetail();   break;
        case 2: drawScreenCharging(); break;
        case 3: drawScreenHealth();   break;
    }
    applyAlertOverlay();
    _canvas->flush();   // single atomic write to display — no flicker
}

// ── Sleep countdown overlay ───────────────────────────────────────────────────
// Called by PowerManager.ino. heldMs = elapsed hold time (0–SLEEP_HOLD_MS).
// Pass 0xFFFFFFFF to clear (button released before threshold).
void displaySleepOverlay(uint32_t heldMs) {
    if (heldMs == 0xFFFFFFFFUL) {
        gSleepCountdownActive = false;
        _dispLast = 0;  // force full redraw on next displayLoop cycle
        return;
    }
    gSleepCountdownActive = true;

    uint32_t clamped = heldMs > SLEEP_HOLD_MS ? SLEEP_HOLD_MS : heldMs;
    uint8_t  pct     = (uint8_t)(clamped * 100UL / SLEEP_HOLD_MS);
    uint32_t remMs   = SLEEP_HOLD_MS - clamped;
    uint8_t  secs    = (uint8_t)((remMs + 999UL) / 1000UL);

    _gfx->fillRect(50, 50, 220, 80, C_HDR);
    _gfx->drawRect(50, 50, 220, 80, C_WARN);
    _gfx->drawRect(51, 51, 218, 78, C_WARN);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_WARN);
    _gfx->setCursor(60, 64);
    if (secs == 0) {
        _gfx->print("   Going to sleep...");
    } else {
        char buf[28];
        snprintf(buf, sizeof(buf), " Hold to sleep: %u s left", (unsigned)secs);
        _gfx->print(buf);
    }

    uint16_t barW   = 192;
    uint16_t filled = (uint16_t)((uint32_t)barW * pct / 100);
    _gfx->fillRect(64, 84, filled,        10, C_WARN);
    _gfx->fillRect(64 + filled, 84, barW - filled, 10, C_DIM);
    _gfx->drawRect(63, 83, barW + 2,      12, C_DIM);

    _gfx->setTextSize(1);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(60, 110);
    _gfx->print("   Release BTN1 to cancel");

    _canvas->flush();
}
