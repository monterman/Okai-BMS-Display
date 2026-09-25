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
// (The old fixed 2x2 fleet grid — CELL_W/CELL_H/CX/CY — was retired with the
//  adaptive HOME screen; cell geometry is now computed per connected-pack count.)

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

bool gLightOn = false;   // legacy flag only — GPIO13 is now NeoPixel strip 2 (not driven)

// True unless the framebuffer alloc failed (no PSRAM + low heap). When false the
// display is skipped entirely so the LEDs + keep-alive keep running (never hang).
static bool gDisplayOk = true;

// ── Refresh timing ────────────────────────────────────────────────────────────
static uint32_t _dispLast;

// ── Dynamic home: connected-pack helpers ──────────────────────────────────────
// A pack counts as connected only if it has a fresh frame within PACK_CONNECTED_MS.
static uint32_t _lastInputMs = 0;   // last button activity — drives HOME_IDLE_MS auto-return
static inline bool packConnected(uint8_t i) {
    return packs[i].valid && (millis() - packs[i].lastUpdateMs) < PACK_CONNECTED_MS;
}
static inline bool anyConnected() {
    for (uint8_t i = 0; i < NUM_PACKS; i++) if (packConnected(i)) return true;
    return false;
}
// Next connected pack after `cur` (wraps); returns `cur` if none other is connected.
static uint8_t nextConnectedPack(uint8_t cur) {
    for (uint8_t s = 1; s <= NUM_PACKS; s++) {
        uint8_t j = (cur + s) % NUM_PACKS;
        if (packConnected(j)) return j;
    }
    return cur;
}

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
// 2026-07-26 - Judged on the rest-gated delta (Config.h healthDelta), never on
// the live one. Cell spread inflates under load from internal-resistance
// differences alone, which used to turn the screen amber on every throttle punch.
static uint16_t healthColor(uint8_t i) {
    if (!packs[i].valid) return C_NODATA;
    const char *t = healthTag(packs[i]);
    if (strcmp(t, "POOR") == 0) return C_POOR;
    if (strcmp(t, "WARN") == 0) return C_WARN;
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

// ══ Adaptive HOME screen (dynamic, connected-only, contiguous) ════════════════
// Home shows ONLY connected packs (packConnected(), PACK_CONNECTED_MS), packed
// contiguously in ascending port order — no empty slots. Each cell keeps its REAL
// port tag (P1..P4) so a weak pack stays identifiable. Layout + info density adapt
// to the connected count (1/2/3/4). A whole-buggy "time remaining" band (worst
// connected pack) sits at the bottom on the 1/2/3-pack tiers.

// Ordered, contiguous list of CONNECTED packs (ascending real port).
static uint8_t buildConnectedList(uint8_t out[NUM_PACKS]) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < NUM_PACKS; i++) if (packConnected(i)) out[n++] = i;
    return n;
}
static float fleetRemainingWh(const uint8_t* list, uint8_t n) {
    float wh = 0.0f;
    for (uint8_t k = 0; k < n; k++) wh += (packs[list[k]].soc / 100.0f) * PACK_DESIGN_WH;
    return wh;
}

// ── Time-remaining estimator (WORST connected pack) ───────────────────────────
// Displayed value = min over connected packs of each pack's own time-to-reserve.
// Per-pack SOC-decline slope (primary) blended with per-pack current/power (cross-
// check). Resets cleanly when the connected set changes. Compute-plane only.
static struct { uint32_t t; uint8_t soc[NUM_PACKS]; } _rtRing[20];
static uint8_t  _rtCount    = 0;
static uint32_t _rtLastSamp = 0;
static uint32_t _rtSetMask  = 0xFFFFFFFF;
static float    _pkPwrEma[NUM_PACKS] = {0};   // per-pack discharge-power EMA (W)
static uint8_t  _pkPwrN[NUM_PACKS]   = {0};
static uint32_t _pkPwrLast = 0;

// Called every loop from displayLoop() — self-gated. Never touches the keep-alive.
static void runtimeSample() {
    uint32_t now = millis();
    uint8_t list[NUM_PACKS]; uint8_t n = buildConnectedList(list);
    uint32_t mask = 0; for (uint8_t k = 0; k < n; k++) mask |= (1u << list[k]);
    if (mask != _rtSetMask) {                       // connected set changed → restart clean
        _rtSetMask = mask; _rtCount = 0; _rtLastSamp = 0;
        for (uint8_t i = 0; i < NUM_PACKS; i++) { _pkPwrEma[i] = 0; _pkPwrN[i] = 0; }
    }
    if (n == 0) { _rtCount = 0; return; }

    // Per-pack discharge-power EMA @1 s (filters the 0x2020 / 8.224 A idle placeholder)
    if (now - _pkPwrLast >= 1000UL) {
        _pkPwrLast = now;
        for (uint8_t k = 0; k < n; k++) {
            uint8_t p = list[k];
            float cur = packs[p].current;
            if (fabsf(cur - 8.224f) < 0.05f) continue;     // idle placeholder — skip
            float draw = -packs[p].voltage * cur;          // W, >0 only while discharging
            if (draw < 0) draw = 0;
            const float A = 0.15f;
            _pkPwrEma[p] = (_pkPwrN[p] == 0) ? draw : A * draw + (1.0f - A) * _pkPwrEma[p];
            if (_pkPwrN[p] < 255) _pkPwrN[p]++;
        }
    }

    // Per-pack SOC ring @RUNTIME_SAMPLE_MS
    if (_rtCount == 0 || (now - _rtLastSamp) >= RUNTIME_SAMPLE_MS) {
        _rtLastSamp = now;
        uint8_t keep = 0;                              // drop samples older than the window
        for (uint8_t i = 0; i < _rtCount; i++)
            if (now - _rtRing[i].t <= RUNTIME_WINDOW_MS) _rtRing[keep++] = _rtRing[i];
        if (keep >= 20) { for (uint8_t i = 1; i < 20; i++) _rtRing[i-1] = _rtRing[i]; keep = 19; }
        _rtRing[keep].t = now;
        for (uint8_t i = 0; i < NUM_PACKS; i++) _rtRing[keep].soc[i] = packs[i].soc;
        _rtCount = keep + 1;
    }
}

// Minutes until pack p hits the reserve, or -1 if not yet computable.
static float packRuntimeMins(uint8_t p) {
    int socNow = packs[p].soc;
    if (socNow <= RUNTIME_RESERVE_PCT) return 0.0f;
    float socMins = -1.0f;                             // primary: SOC decline slope
    if (_rtCount >= 2) {
        uint32_t span = _rtRing[_rtCount-1].t - _rtRing[0].t;
        float    drop = (float)_rtRing[0].soc[p] - (float)_rtRing[_rtCount-1].soc[p];
        if (span >= RUNTIME_MIN_SPAN_MS && drop > 0.05f)
            socMins = (socNow - RUNTIME_RESERVE_PCT) / (drop / (span / 60000.0f));
    }
    float powMins = -1.0f;                             // cross-check: per-pack current/power
    if (_pkPwrN[p] >= 3 && _pkPwrEma[p] >= 20.0f)
        powMins = ((socNow - RUNTIME_RESERVE_PCT) / 100.0f * PACK_DESIGN_WH) / _pkPwrEma[p] * 60.0f;
    if (socMins > 0 && powMins > 0) return 0.5f * socMins + 0.5f * powMins;
    if (socMins > 0) return socMins;
    if (powMins > 0) return powMins;
    return -1.0f;
}

// Whole-buggy string = the WORST (soonest-to-reserve) connected pack.
static void runtimeString(char* out, size_t len) {
    if (logCurrentMode() == LOG_CHARGE) { snprintf(out, len, "CHARGING"); return; }
    uint8_t list[NUM_PACKS]; uint8_t n = buildConnectedList(list);
    if (!n) { snprintf(out, len, "~-- min"); return; }
    float worst = -1.0f;
    for (uint8_t k = 0; k < n; k++) {
        uint8_t p = list[k];
        if (packs[p].soc <= RUNTIME_RESERVE_PCT) { snprintf(out, len, "LOW P%u", p + 1); return; }
        float m = packRuntimeMins(p);
        if (m >= 0 && (worst < 0 || m < worst)) worst = m;
    }
    if (worst < 0) { snprintf(out, len, "~-- min"); return; }   // warming up
    if (worst > 599) { snprintf(out, len, ">9h"); return; }
    uint16_t m = (uint16_t)(worst + 0.5f);
    if (m >= 60) snprintf(out, len, "~%uh%02um", m / 60, m % 60);
    else         snprintf(out, len, "~%u min", m);
}

// ── Cell primitives ───────────────────────────────────────────────────────────
static void drawSocBarH(int x, int y, int w, int h, uint8_t soc, uint16_t col) {
    if (soc > 100) soc = 100;
    _gfx->drawRect(x, y, w, h, C_DIM);
    _gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_BG);
    int fw = ((w - 2) * soc) / 100;
    if (fw > 0) _gfx->fillRect(x + 1, y + 1, fw, h - 2, col);
}
static void drawSocBarV(int x, int y, int w, int h, uint8_t soc, uint16_t col) {
    if (soc > 100) soc = 100;
    _gfx->drawRect(x, y, w, h, C_DIM);
    _gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_BG);
    int fh = ((h - 2) * soc) / 100;
    if (fh > 0) _gfx->fillRect(x + 1, y + h - 1 - fh, w - 2, fh, col);   // fill bottom-up
}
static void drawCellTags(int x, int y, int w, uint8_t p, uint8_t tagSize, bool showHealth) {
    char b[6];
    _gfx->setTextSize(tagSize); _gfx->setTextColor(C_TEXT);
    _gfx->setCursor(x + 4, y + 4);
    snprintf(b, sizeof(b), "P%u", p + 1); _gfx->print(b);       // REAL port number
    if (showHealth) {
        const char *ht = healthTag(packs[p]);   // rest-gated (Config.h)
        _gfx->setTextColor(healthColor(p));
        _gfx->setCursor(x + w - 4 - 4 * 6 * tagSize, y + 4);    // 4 chars, 6px/char/size
        _gfx->print(ht);
    }
}

// Draw one connected pack into a rect at a density tier (1..4 = packs on screen).
static void drawHomeCell(int x, int y, int w, int h, uint8_t p, uint8_t tier) {
    uint16_t hc  = healthColor(p);
    uint16_t dmv = (uint16_t)((packs[p].cellHigh - packs[p].cellLow) * 1000.0f + 0.5f);
    uint8_t  soc = packs[p].soc;
    char b[20];
    _gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_BG);
    _gfx->drawRect(x, y, w, h, hc);

    if (tier == 1) {                                   // full detail
        drawCellTags(x, y, w, p, 2, true);
        _gfx->setTextSize(6); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc); _gfx->setCursor(x + 8, y + 24); _gfx->print(b);
        _gfx->setTextSize(3); _gfx->setTextColor(C_ACCENT);
        snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); _gfx->setCursor(x + 174, y + 28); _gfx->print(b);
        _gfx->setTextColor(C_TEXT);
        fmtAmps(b, sizeof(b), packs[p].current, 1, "A");   // "<0.0A" when a balancing trickle rounds away _gfx->setCursor(x + 174, y + 56); _gfx->print(b);
        drawSocBarH(x + 10, y + 80, w - 20, 24, soc, hc);
        _gfx->setTextSize(2); _gfx->setTextColor(C_DIM);
        snprintf(b, sizeof(b), "%.0fW", packs[p].voltage * packs[p].current); _gfx->setCursor(x + 10,  y + 114); _gfx->print(b);
        snprintf(b, sizeof(b), "%u*C", (unsigned)packs[p].maxTemp);           _gfx->setCursor(x + 96,  y + 114); _gfx->print(b);
        snprintf(b, sizeof(b), "d%umV", dmv);                                 _gfx->setCursor(x + 176, y + 114); _gfx->print(b);
    } else if (tier == 2) {                            // large half
        drawCellTags(x, y, w, p, 2, true);
        _gfx->setTextSize(5); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc); _gfx->setCursor(x + 6, y + 24); _gfx->print(b);
        _gfx->setTextSize(2); _gfx->setTextColor(C_ACCENT);
        snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); _gfx->setCursor(x + 6, y + 68); _gfx->print(b);
        drawSocBarH(x + 6, y + 90, w - 12, 20, soc, hc);
        _gfx->setTextColor(C_TEXT);
        fmtAmps(b, sizeof(b), packs[p].current, 1, "A");   // "<0.0A" when a balancing trickle rounds away _gfx->setCursor(x + 6, y + 116); _gfx->print(b);
        snprintf(b, sizeof(b), "%u*C", (unsigned)packs[p].maxTemp); _gfx->setCursor(x + w - 56, y + 116); _gfx->print(b);
    } else if (tier == 3) {                            // column: %, voltage, tall bar
        drawCellTags(x, y, w, p, 2, false);
        _gfx->setTextSize(3); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc); _gfx->setCursor(x + 6, y + 24); _gfx->print(b);
        _gfx->setTextSize(2); _gfx->setTextColor(C_ACCENT);
        snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); _gfx->setCursor(x + 6, y + 50); _gfx->print(b);
        drawSocBarV(x + (w - 44) / 2, y + 74, 44, 60, soc, hc);
    } else {                                           // 4-pack slim column: fat bar + big %
        drawCellTags(x, y, w, p, 1, false);
        uint8_t ps = (soc >= 100) ? 2 : 3;             // shrink one step so "100%" fits 79 px
        _gfx->setTextSize(ps); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc);
        _gfx->setCursor(x + (w - (int)strlen(b) * 6 * ps) / 2, y + 22); _gfx->print(b);
        drawSocBarV(x + (w - 36) / 2, y + 50, 36, 78, soc, hc);
        _gfx->setTextSize(1); _gfx->setTextColor(C_ACCENT);
        snprintf(b, sizeof(b), "%.1fv", packs[p].voltage);
        _gfx->setCursor(x + (w - (int)strlen(b) * 6) / 2, y + 136); _gfx->print(b);
    }
}

// Bottom band (tiers 1-3): whole-buggy worst-pack time remaining + fleet Wh.
static void drawRuntimeBand(int y) {
    _gfx->fillRect(0, y, 320, 170 - y, C_HDR);
    char rt[16]; runtimeString(rt, sizeof(rt));
    bool warm = (strstr(rt, "--") != NULL);
    _gfx->setTextSize(2);
    _gfx->setTextColor(warm ? C_DIM : C_GOOD);
    _gfx->setCursor(6, y + 1); _gfx->print(rt);
    uint8_t list[NUM_PACKS]; uint8_t n = buildConnectedList(list);
    char wh[16]; snprintf(wh, sizeof(wh), "%.0f Wh", fleetRemainingWh(list, n));
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(320 - (int)strlen(wh) * 12 - 6, y + 1); _gfx->print(wh);
}

// ── Screen 0: HOME — dynamic, connected-only, contiguous, adaptive density ────
static void drawScreenFleet() {
    drawHeader();
    _gfx->fillRect(0, HDR_H, 320, 170 - HDR_H, C_BG);   // clear content region

    uint8_t list[NUM_PACKS]; uint8_t n = buildConnectedList(list);
    const int Y0 = HDR_H;                               // 16
    if (n == 0) {
        _gfx->setTextSize(2); _gfx->setTextColor(C_NODATA);
        _gfx->setCursor(40, 80); _gfx->print("No packs connected");
        return;
    }
    const bool band   = (n <= 3);                       // 4-pack drops the band (bars win)
    const int  BAND_Y = 154;
    const int  H = band ? (BAND_Y - Y0) : (170 - Y0);   // 138 (with band) or 154 (4-pack)

    if (n == 1) {
        drawHomeCell(0, Y0, 320, H, list[0], 1);
    } else if (n == 2) {
        drawHomeCell(0,   Y0, 158, H, list[0], 2);
        drawHomeCell(162, Y0, 158, H, list[1], 2);
    } else if (n == 3) {
        drawHomeCell(0,   Y0, 105, H, list[0], 3);
        drawHomeCell(107, Y0, 105, H, list[1], 3);
        drawHomeCell(214, Y0, 106, H, list[2], 3);
    } else {                                            // n == 4
        for (uint8_t k = 0; k < 4; k++)
            drawHomeCell(k * 80, Y0, 79, H, list[k], 4);
    }
    if (band) drawRuntimeBand(BAND_Y);
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
    // Rest-gated: show and judge the last at-rest spread (Config.h)
    float delta = packs[i].valid ? healthDelta(packs[i]) : 0.0f;
    const char *htag = !packs[i].valid ? "----" : healthTag(packs[i]);
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
        // 2026-07-26 - current via fmtAmps() so a balancing trickle reads "<0.00A"
        // rather than "+0.00A", which looked like nothing was happening at 40 mA.
        char va[24], amps[12];
        fmtAmps(amps, sizeof(amps), packs[i].current, 2, "A");
        snprintf(va, sizeof(va), "%.2fV %s", packs[i].voltage, amps);
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

        // 2026-07-26 - three states instead of two. "Balancing" is the phase after
        // bulk where the BMS still trickles current so the passive balancer can bleed
        // the high cells; it used to be labelled "Charging..." which read as wrong
        // next to a near-zero current, and at 99% SOC it never reached "Charge done".
        if (packs[i].chargerDetected) {
            // 2026-07-26 - owner-chosen wording. "Bulk charging" names the actual
            // BMS phase and, more usefully, marks the point to unplug when in a
            // hurry: bulk carries ~90% of the capacity, the balancing taper that
            // follows adds the last few percent and takes as long or longer.
            const char *st = packs[i].isBalancing ? "Balancing"
                           : packs[i].chargeDone  ? "Charge complete"
                                                  : "Bulk charging";
            _gfx->setTextColor(packs[i].chargeDone ? C_GOOD : C_CHARGE);
            _gfx->setCursor(4, 118);
            _gfx->print(st);
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
        // Rest-gated: the mV shown is the last at-rest spread, so the number and
        // the tag beside it always agree (Config.h healthDelta/healthTag).
        uint16_t dmv = (uint16_t)(healthDelta(packs[i]) * 1000.0f + 0.5f);
        float   avWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        const char *stag = healthTag(packs[i]);
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

    // GPIO13 (LIGHT_FET) retired — it is now NeoPixel status strip 2 (Led.ino / ledInit()).
    // Display must NOT drive GPIO13 or it fights the strip's RMT signal.

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
    // Harden: the 320x170 RGB565 framebuffer is ~106 KB. With no PSRAM it comes from
    // internal DRAM and can fail. begin() returns false / getFramebuffer() is null on
    // failure. Degrade gracefully (disable the display) instead of hanging — the LEDs
    // and the Core-1 keep-alive must keep running no matter what.
    bool canvasOk = _canvas && _canvas->begin() && _canvas->getFramebuffer();
    if (!canvasOk) {
        Serial.println("[DISP] FATAL: no framebuffer (no PSRAM + low heap) — display DISABLED, keep-alive continues");
        gDisplayOk = false;
        return;
    }
    gDisplayOk = true;
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
    if (!gDisplayOk) return;            // framebuffer alloc failed — LEDs + keep-alive still run

    runtimeSample();                    // keep the worst-pack runtime window warm on every screen

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
    if (b1 == LOW || b2 == LOW || b3 == LOW) _lastInputMs = now;   // any press resets the idle timer

    // BTN2+BTN3 combo: toggle the (now indicator-only) light flag — GPIO13 is NeoPixel strip 2
    static bool     _comboFired = false;
    static uint32_t _comboTs    = 0;
    if (b2 == LOW && b3 == LOW) {
        if (_comboTs == 0) _comboTs = now;
        if (!_comboFired && (now - _comboTs) >= DEBOUNCE_MS) {
            _comboFired = true;
            gLightOn    = !gLightOn;   // flag only — GPIO13 is NeoPixel strip 2 now (no FET drive)
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
            else if (_screen == 1) _detailPack = nextConnectedPack(_detailPack);  // cycle connected packs only
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

    // ── Dynamic home: charging auto-switch + idle auto-return + connected-only detail ──
    // Charging auto-switch fires ONCE on the rising edge (screen 2) so the user can still
    // navigate away; when charging stops it returns Home if still on the charging screen.
    bool charging = (logCurrentMode() == LOG_CHARGE);
    static bool _wasCharging = false;
    if (charging && !_wasCharging)              { _screen = 2; _dispLast = 0; }   // charging started
    else if (!charging && _wasCharging && _screen == 2) { _screen = 0; _dispLast = 0; } // charging ended
    _wasCharging = charging;
    // Idle auto-return to the dynamic Home (Fleet) — never while charging.
    if (!charging && _screen != 0 && (now - _lastInputMs) > HOME_IDLE_MS) { _screen = 0; _dispLast = 0; }
    // Detail (screen 1) must never sit on a disconnected pack.
    if (_screen == 1 && !packConnected(_detailPack) && anyConnected()) {
        _detailPack = nextConnectedPack(_detailPack); _dispLast = 0;
    }

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
    if (!gDisplayOk) return;   // no framebuffer — skip drawing, sleep still proceeds

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
