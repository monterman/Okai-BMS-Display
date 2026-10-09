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

// 2026-10-06 - STUCK-BUTTON DETECTION. A button held low for BTN_STUCK_MS is not a press;
// nobody holds a button for fifteen seconds. It is water, a jammed cap or a failed switch.
//
// Two reasons this is worth real code rather than a comment:
//   1. The owner's reported fault was a stuck BTN3, and until now a stuck button produced
//      silently broken navigation with nothing on screen to say why.
//   2. A stuck BTN1 is a SAFETY issue, not a usability one. A 4 s BTN1 hold sleeps the
//      board, and deep sleep halts the keep-alive, which stops the packs outputting power
//      five seconds later — Rex's K-2, graded CRITICAL and latching. Refusing to sleep on
//      a button we have decided is faulty closes it.
//
// Indices are 0=BTN1, 1=BTN2, 2=BTN3. Cleared the moment the pin reads high again, so a
// genuinely long press is forgiven as soon as it ends.
#define BTN_STUCK_MS 15000UL
static bool     _btnStuck[3]    = { false, false, false };
static uint32_t _btnLowSince[3] = { 0, 0, 0 };

// Read by PowerManager.ino so a faulty BTN1 cannot sleep the board.
bool btnIsStuck(uint8_t i) { return (i < 3) ? _btnStuck[i] : false; }

static void updateStuckButtons(uint32_t now, bool b1, bool b2, bool b3) {
    const bool low[3] = { b1 == LOW, b2 == LOW, b3 == LOW };
    static const char* names[3] = { "BTN1", "BTN2", "BTN3" };
    for (uint8_t i = 0; i < 3; i++) {
        if (!low[i]) {
            if (_btnStuck[i])
                Serial.printf("[BTN] %s released after being stuck — back in service\n", names[i]);
            _btnStuck[i]    = false;
            _btnLowSince[i] = 0;
            continue;
        }
        if (_btnLowSince[i] == 0) { _btnLowSince[i] = now; continue; }
        if (!_btnStuck[i] && (now - _btnLowSince[i]) > BTN_STUCK_MS) {
            _btnStuck[i] = true;
            Serial.printf("[BTN] %s STUCK LOW for %lu s — ignoring it%s\n",
                          names[i], (unsigned long)((now - _btnLowSince[i]) / 1000UL),
                          (i == 0) ? " and BLOCKING SLEEP (a sleep here would cut traction power)"
                                   : "");
        }
    }
}
#define DEBOUNCE_MS   50UL
#define LONGPRESS_MS 800UL   // hold BTN3 to enter label assign

// Set true by displaySleepOverlay() while sleep countdown is active.
// displayLoop() yields the display to PowerManager while this is true.
bool gSleepCountdownActive = false;


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

// ── Overlay state ─────────────────────────────────────────────────────────────
// 2026-10 - The pack-disconnect modal is GONE. It fired after 2 min of pack absence
// and then sat on the display forever with no timeout, which is the single worst
// offender against the rule below: it covered Home until a button was physically
// pressed — on a sealed box, on the water. Nothing is lost by removing it. The
// dropout is still recorded in the CSV by writePackEdges() (Logger.ino) with a
// timestamp, and a missing pack now simply vanishes from the Home gauges.
//
// The label picker survives as the one overlay, because it is user-invoked rather
// than self-raising, and it now self-dismisses after OVERLAY_TIMEOUT_MS.
static bool     _showLabelPick  = false;
static uint8_t  _labelPickPort  = 0;
static uint8_t  _labelPickVal   = 0;   // 0=unassigned, 1-8=label
static uint32_t _overlayShownMs = 0;   // last time the overlay opened or took a button

// One ask per insertion. When PackRegistry cannot tell two stored packs apart it sets
// packRec[].ambiguous; we prompt once, then stay quiet until that port is re-seated,
// so a pack it can never resolve does not nag on every refresh.
static bool _ambiguousAsked[NUM_PACKS];

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

// drawDisconnectModal() removed 2026-10 — see the overlay-state note above.

// ── Overlay: label picker ─────────────────────────────────────────────────────
// 2026-10 - Redrawn at readable sizes. The old version used setTextSize(1) — a 6x8
// px font — for the title and the button hints on a 320x170 panel, which the owner
// could not read. Everything here is size 2 or larger, and the value itself is size 6,
// matching the Home gauges it sits over.
static void drawLabelPicker() {
    const int BX = 10, BY = 30, BW = 300, BH = 112;
    _gfx->fillRect(BX, BY, BW, BH, C_HDR);
    _gfx->drawRect(BX,     BY,     BW,     BH,     C_ACCENT);
    _gfx->drawRect(BX + 1, BY + 1, BW - 2, BH - 2, C_ACCENT);

    // Title — which pack we are naming, in the owner's terms
    _gfx->setTextSize(2);
    _gfx->setTextColor(C_ACCENT);
    _gfx->setCursor(BX + 12, BY + 10);
    _gfx->print("WHICH PACK?");

    // Why we are asking, when the registry could not tell the packs apart
    if (packRec[_labelPickPort].ambiguous) {
        _gfx->setTextSize(1);
        _gfx->setTextColor(C_WARN);
        _gfx->setCursor(BX + 12, BY + 32);
        char why[44];
        snprintf(why, sizeof(why), "Port %u - cycles too close to tell apart",
                 _labelPickPort + 1);
        _gfx->print(why);
    } else {
        _gfx->setTextSize(1);
        _gfx->setTextColor(C_DIM);
        _gfx->setCursor(BX + 12, BY + 32);
        char why[40];
        snprintf(why, sizeof(why), "Port %u - set the pack number", _labelPickPort + 1);
        _gfx->print(why);
    }

    // The value, big
    char valStr[4];
    if (_labelPickVal == 0) strcpy(valStr, "--");
    else snprintf(valStr, sizeof(valStr), "%u", _labelPickVal);
    _gfx->setTextSize(6);
    _gfx->setTextColor(C_TEXT);
    int16_t vw = (int16_t)strlen(valStr) * 36;
    _gfx->setCursor(160 - vw / 2, BY + 46);
    _gfx->print(valStr);

    // Button hints — size 2, one word each so they fit
    _gfx->setTextSize(2);
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(BX + 12,  BY + 92);  _gfx->print("1:next");
    _gfx->setTextColor(C_GOOD);
    _gfx->setCursor(BX + 112, BY + 92);  _gfx->print("2:OK");
    _gfx->setTextColor(C_DIM);
    _gfx->setCursor(BX + 196, BY + 92);  _gfx->print("3:cancel");
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

    // 2026-10-06 - This slot used to show "LGT", an indicator for a light FET that was
    // retired when GPIO13 became NeoPixel strip 2. It reported a flag that drove nothing,
    // while the chord that toggled it was silently disabling screen navigation.
    //
    // The slot now earns its place: it names a button we have stopped trusting. The owner
    // spent a session unable to change screens with nothing on-screen explaining why, so
    // the fault that caused it is now visible at a glance.
    _gfx->setCursor(210, 4);
    {
        // 2026-10-09 - FS! OUTRANKS A STUCK BUTTON HERE, and the reason is the pack #1
        // diagnosis. If LittleFS fails to mount, loggerLoop() returns immediately: no CSV
        // row is ever written, the mode stays IDLE, and NOTHING on the screen or the
        // dashboard says so — the owner would ride a full session believing he was
        // logging and find an empty card afterwards. Four uploaded CSV logs are what
        // settled pack #1's charge-path fault; silent logging loss is how that evidence
        // stops existing. A stuck button is an annoyance, a dead log is a lost diagnosis.
        if (!fsReady) {
            _gfx->setTextColor(C_POOR);
            _gfx->print("FS!");
        } else {
            int8_t stuck = -1;
            for (uint8_t i = 0; i < 3; i++) if (btnIsStuck(i)) { stuck = (int8_t)i; break; }
            if (stuck >= 0) {
                char s[8]; snprintf(s, sizeof(s), "BTN%d!", (int)stuck + 1);
                _gfx->setTextColor(C_POOR);
                _gfx->print(s);
            }
        }
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

// Separate, much slower ring for the charge direction — see CHG_* in Config.h for why
// the discharge ring is unusable here (200 s window vs ~0.37 %/min of SOC movement).
static struct { uint32_t t; uint8_t soc[NUM_PACKS]; } _chgRing[CHG_RING_LEN];
static uint8_t  _chgCount    = 0;
static uint32_t _chgLastSamp = 0;

// Called every loop from displayLoop() — self-gated. Never touches the keep-alive.
static void runtimeSample() {
    uint32_t now = millis();
    uint8_t list[NUM_PACKS]; uint8_t n = buildConnectedList(list);
    uint32_t mask = 0; for (uint8_t k = 0; k < n; k++) mask |= (1u << list[k]);
    if (mask != _rtSetMask) {                       // connected set changed → restart clean
        _rtSetMask = mask; _rtCount = 0; _rtLastSamp = 0;
        _chgCount  = 0;    _chgLastSamp = 0;        // the charge slope is set-sensitive too
        for (uint8_t i = 0; i < NUM_PACKS; i++) { _pkPwrEma[i] = 0; _pkPwrN[i] = 0; }
    }
    if (n == 0) { _rtCount = 0; _chgCount = 0; return; }

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

    // Per-pack SOC ring @CHG_SAMPLE_MS — the charge-direction slope. Sampled
    // unconditionally rather than only while charging, so the window is already warm
    // the moment a charger is plugged in instead of making the owner wait 5 min.
    if (_chgCount == 0 || (now - _chgLastSamp) >= CHG_SAMPLE_MS) {
        _chgLastSamp = now;
        uint8_t keep = 0;
        for (uint8_t i = 0; i < _chgCount; i++)
            if (now - _chgRing[i].t <= CHG_WINDOW_MS) _chgRing[keep++] = _chgRing[i];
        if (keep >= CHG_RING_LEN) {
            for (uint8_t i = 1; i < CHG_RING_LEN; i++) _chgRing[i-1] = _chgRing[i];
            keep = CHG_RING_LEN - 1;
        }
        _chgRing[keep].t = now;
        for (uint8_t i = 0; i < NUM_PACKS; i++) _chgRing[keep].soc[i] = packs[i].soc;
        _chgCount = keep + 1;
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

// ── Time-to-full estimator ────────────────────────────────────────────────────
// 2026-10 - The mirror of packRuntimeMins(), run in the charge direction, so the owner
// can plan a multi-pack charge instead of guessing.
//
// SLOPE IS PRIMARY, DELIBERATELY. The obvious calculation — remaining capacity divided
// by charge current — is optimistic exactly where it matters: lithium charges at
// constant current to roughly 80-90 %, then holds constant voltage while the current
// tapers away, so the last stretch takes far longer than the linear maths predicts.
// Measuring how fast SOC is actually climbing absorbs that taper for free. The
// capacity/current figure is kept only as a cross-check for the early flat region.
//
// Returns minutes, or -1 while the ring is still warming up.
static float packChargeMins(uint8_t p) {
    uint8_t socNow = packs[p].soc;
    if (socNow >= 100) return 0.0f;

    float socMins = -1.0f;
    if (_chgCount >= 2) {
        uint32_t span = _chgRing[_chgCount-1].t - _chgRing[0].t;
        // [0] is the OLDEST sample, [_chgCount-1] the newest — so a climb is new minus
        // old, the exact inverse of the discharge path's drop. Uses the SLOW ring: on a
        // 4.5 h charge the fast discharge ring sees ~1 % and would just read noise.
        float    rise = (float)_chgRing[_chgCount-1].soc[p] - (float)_chgRing[0].soc[p];
        // 2026-10-06 - Require at least TWO SOC ticks. SOC is 1%-quantised, so one tick is a
        // slope with ~100% uncertainty: the owner saw "8 hours" built from 1% in 13 min on a
        // charge that was really ~3.7 h out. Two ticks halves the quantisation error.
        if (span >= CHG_MIN_SPAN_MS && rise >= 2.0f)
            socMins = (100.0f - socNow) / (rise / (span / 60000.0f));
    }

    float curMins = -1.0f;
    float amps = packs[p].current;                      // + = into the pack
    // The BMS emits 0x2020 / 8.224 A as an idle placeholder (OkaiBMS.h:43), which
    // runtimeSample() already filters. Unfiltered here it sails past the 0.2 A gate and,
    // during the 5 min slope warm-up, produces a confident ~47 min estimate for a charge
    // that actually takes ~4.5 h — and then poisons the 50/50 blend afterwards.
    if (fabsf(amps - 8.224f) < 0.05f) amps = 0.0f;
    // 2026-10-06 - PLAUSIBILITY GATE. Byte [20] decodes as capacity x 200 mAh, and the real
    // pack measures ~12.7 Ah (0.854 A for 6% over 53 min), matching a byte of 64. But across
    // this fleet it reads 0, 0, 4, 4 - i.e. 0 mAh or 800 mAh, both impossible for a 10S4P.
    // With 800 mAh the current path returns ~23 min for a pack hours from full, and the
    // 50/50 blend below only halves that error. A capacity we do not believe is worse than
    // none, because the SOC slope on its own is sound.
    if (amps > 0.2f && packs[p].capacityMah >= PACK_CAPACITY_MIN_MAH)
        curMins = ((100.0f - socNow) / 100.0f * (float)packs[p].capacityMah)
                  / (amps * 1000.0f) * 60.0f;

    if (socMins > 0 && curMins > 0) return 0.5f * socMins + 0.5f * curMins;
    if (socMins > 0) return socMins;
    if (curMins > 0) return curMins;
    return -1.0f;
}

// Compact per-cell string for the Home gauges: "1h20", "45m", "BAL", "FULL", "--".
// Balancing is reported by name rather than as a time: SOC barely moves during the
// constant-voltage tail, so any extrapolation there reads as near-infinite.
static void chargeTimeStr(uint8_t p, char* out, size_t len) {
    if (packs[p].chargeDone || packs[p].soc >= 100) { snprintf(out, len, "FULL"); return; }
    if (packs[p].isBalancing) { snprintf(out, len, "BAL");  return; }
    float m = packChargeMins(p);
    if (m < 0)   { snprintf(out, len, "--");  return; }   // warming up
    if (m > 599) { snprintf(out, len, ">9h"); return; }
    uint16_t mm = (uint16_t)(m + 0.5f);
    if (mm >= 60) snprintf(out, len, "%uh%02u", mm / 60, mm % 60);
    else          snprintf(out, len, "%um", mm);
}

// Whole-buggy string = the WORST (soonest-to-reserve) connected pack.
static void runtimeString(char* out, size_t len) {
    // While charging this used to print the literal word "CHARGING", throwing the
    // estimate away at the one moment it is most useful. Now it reports when the LAST
    // pack finishes — the number that says when you can actually walk away.
    if (logCurrentMode() == LOG_CHARGE) {
        uint8_t cl[NUM_PACKS]; uint8_t cn = buildConnectedList(cl);
        float slowest = -1.0f; bool anyWorking = false;
        for (uint8_t k = 0; k < cn; k++) {
            uint8_t p = cl[k];
            // A pack at 100 % that has not yet asserted chargeDone used to count as
            // "working" and contribute packChargeMins()==0, so one finished pack made the
            // band read "FULL IN 0 min" while another sat at 30 % with hours to go.
            if (packs[p].chargeDone || packs[p].soc >= 100) continue;
            anyWorking = true;
            float m = packChargeMins(p);
            if (m >= 0 && m > slowest) slowest = m;
        }
        if (!anyWorking)      snprintf(out, len, "ALL FULL");
        else if (slowest < 0) snprintf(out, len, "CHARGING");       // still warming up
        else if (slowest > 599) snprintf(out, len, "FULL IN >9h");
        else {
            uint16_t m = (uint16_t)(slowest + 0.5f);
            if (m >= 60) snprintf(out, len, "FULL IN %uh%02um", m / 60, m % 60);
            else         snprintf(out, len, "FULL IN %u min", m);
        }
        return;
    }
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

    // 2026-10 - While charging, each cell swaps its least useful readout for TIME TO
    // FULL, so a multi-pack charge can be planned at a glance. The percentage keeps
    // its full size on every tier — the line that gives way is voltage (tiers 3/4) or
    // amps/watts (tiers 1/2), all of which matter less than "when is this done".
    const bool chgMode = (logCurrentMode() == LOG_CHARGE);
    char ct[10] = {0};
    if (chgMode) chargeTimeStr(p, ct, sizeof(ct));

    if (tier == 1) {                                   // full detail
        drawCellTags(x, y, w, p, 2, true);
        _gfx->setTextSize(6); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc); _gfx->setCursor(x + 8, y + 24); _gfx->print(b);
        _gfx->setTextSize(3); _gfx->setTextColor(C_ACCENT);
        snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); _gfx->setCursor(x + 174, y + 28); _gfx->print(b);
        _gfx->setTextColor(C_TEXT);
        // "<0.0A" when a balancing trickle rounds away.
        // 2026-10 - the setCursor/print below had been absorbed into the trailing
        // comment on this line, so current never rendered on the 1-pack home tier.
        fmtAmps(b, sizeof(b), packs[p].current, 1, "A");
        _gfx->setCursor(x + 174, y + 56); _gfx->print(b);
        drawSocBarH(x + 10, y + 80, w - 20, 24, soc, hc);
        _gfx->setTextSize(2); _gfx->setTextColor(C_DIM);
        if (chgMode) { _gfx->setTextColor(C_GOOD); _gfx->setCursor(x + 10, y + 114); _gfx->print(ct); _gfx->setTextColor(C_DIM); }
        else { snprintf(b, sizeof(b), "%.0fW", packs[p].voltage * packs[p].current); _gfx->setCursor(x + 10,  y + 114); _gfx->print(b); }
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
        // Same swallowed-by-comment bug as tier 1 — fixed 2026-10.
        if (chgMode) { _gfx->setTextColor(C_GOOD); _gfx->setCursor(x + 6, y + 116); _gfx->print(ct);
                       _gfx->setTextColor(C_TEXT); }   // restore, or the temp below inherits green
        else { fmtAmps(b, sizeof(b), packs[p].current, 1, "A");
               _gfx->setCursor(x + 6, y + 116); _gfx->print(b); }
        snprintf(b, sizeof(b), "%u*C", (unsigned)packs[p].maxTemp); _gfx->setCursor(x + w - 56, y + 116); _gfx->print(b);
    } else if (tier == 3) {                            // column: %, voltage, tall bar
        drawCellTags(x, y, w, p, 2, false);
        _gfx->setTextSize(3); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc); _gfx->setCursor(x + 6, y + 24); _gfx->print(b);
        _gfx->setTextSize(2);
        if (chgMode) { _gfx->setTextColor(C_GOOD); _gfx->setCursor(x + 6, y + 50); _gfx->print(ct); }
        else { _gfx->setTextColor(C_ACCENT);
               snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); _gfx->setCursor(x + 6, y + 50); _gfx->print(b); }
        drawSocBarV(x + (w - 44) / 2, y + 74, 44, 60, soc, hc);
    } else {                                           // 4-pack slim column: fat bar + big %
        drawCellTags(x, y, w, p, 1, false);
        uint8_t ps = (soc >= 100) ? 2 : 3;             // shrink one step so "100%" fits 79 px
        _gfx->setTextSize(ps); _gfx->setTextColor(hc);
        snprintf(b, sizeof(b), "%u%%", soc);
        _gfx->setCursor(x + (w - (int)strlen(b) * 6 * ps) / 2, y + 22); _gfx->print(b);
        drawSocBarV(x + (w - 36) / 2, y + 50, 36, 78, soc, hc);
        _gfx->setTextSize(1);
        if (chgMode) { _gfx->setTextColor(C_GOOD); snprintf(b, sizeof(b), "%s", ct); }
        else { _gfx->setTextColor(C_ACCENT); snprintf(b, sizeof(b), "%.1fv", packs[p].voltage); }
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
    char plabel[10]; portPackStr(i, plabel, sizeof(plabel));
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
        // 2026-10-09 - show the PACK NUMBER beside the port: "P3-6 87% 401Wh". The owner's
        // point, and it is right: the port is the socket, the number in white marker is the
        // battery, and only the second one is useful while holding one.
        char pp[10]; portPackStr(i, pp, sizeof(pp));
        char row[28];
        if (phA) {
            float wh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
            snprintf(row, sizeof(row), "%s %3u%% %4.0fWh", pp, (unsigned)packs[i].soc, wh);
        } else {
            float ah = (packs[i].soc / 100.0f) * PACK_DESIGN_AH;
            snprintf(row, sizeof(row), "%s %3u%% %.1fAh", pp, (unsigned)packs[i].soc, ah);
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
        char pp[10]; portPackStr(i, pp, sizeof(pp));
        char lbl[18]; snprintf(lbl, sizeof(lbl), "%s %u%%", pp, (unsigned)packs[i].soc);
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
        char pp3[10]; portPackStr(i, pp3, sizeof(pp3));
        char row[34];
        if (phA) {
            // P3-6  87%  401Wh  GOOD
            snprintf(row, sizeof(row), "%s %3u%% %4.0fWh %s",
                     pp3, (unsigned)packs[i].soc, avWh, stag);
        } else {
            // P3-6  d43mV  97%  — the cycle count is GONE from here on purpose. The owner:
            // "the CYC... I really don't care to see. That doesn't matter to me because I
            // don't know how to read that." It is a registry diagnostic, still in the CSV,
            // on /packs and on the per-pack detail screen. The space buys the pack number.
            snprintf(row, sizeof(row), "%s d%umV %u%%",
                     pp3, (unsigned)dmv, (unsigned)soh);
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
    _showLabelPick  = false;
    _overlayShownMs = 0;

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

// ── HOME POLICY — the rule nothing may bypass ─────────────────────────────────
// Home (screen 0, the adaptive pack gauges) is THE display. Everything else is a
// detour the device must undo by itself.
//
// This runs before every early return that can persist — i.e. above the overlay returns,
// which is the placement that matters. It sits BELOW two short-lived guards it must not
// fight: gSleepCountdownActive (PowerManager owns the display while BTN1 is held, <=4 s
// and self-clearing) and !gDisplayOk (framebuffer alloc failed, nothing is drawn at all).
// Neither can strand the display, so neither needs the policy. The previous code put the
// idle auto-return
// below the overlay early-returns, so an overlay suppressed the very mechanism meant
// to dismiss it — and the auto-return was additionally gated on !charging, so while
// a charger was connected the display never came home at all. Between them, the
// device could sit on the wrong screen indefinitely, on a sealed box, on the water.
//
// Two invariants, no exceptions:
//   1. No overlay survives OVERLAY_TIMEOUT_MS without input.
//   2. No screen other than Home survives HOME_IDLE_MS without input. Charging is
//      NOT an exemption — plugging in may jump to the charging screen, but the same
//      timer brings it home.
static void enforceHomePolicy(uint32_t now) {
    if (_showLabelPick && (now - _overlayShownMs) > OVERLAY_TIMEOUT_MS) {
        Serial.printf("[DISP] picker TIMED OUT port%u after %lu s (no change)\n",
                      _labelPickPort + 1, (unsigned long)(OVERLAY_TIMEOUT_MS / 1000UL));
        _showLabelPick = false;
        _dispLast      = 0;
    }
    if (_screen != 0 && (now - _lastInputMs) > HOME_IDLE_MS) {
        _screen   = 0;
        _dispLast = 0;
    }
}

// Open the label picker for a specific port, pre-filled with its current number.
#define PICKER_GRACE_MS 400UL   // ignore button edges for this long after the picker opens
static void openLabelPicker(uint8_t port, uint32_t now) {
    if (port >= NUM_PACKS) return;
    _showLabelPick  = true;
    _labelPickPort  = port;
    _labelPickVal   = labelGet(port);
    _overlayShownMs = now;
    _dispLast       = 0;
    // 2026-10-06 - Force a genuine release-then-press. The picker used to honour the button
    // state from BEFORE it opened, so one leftover or noisy edge confirmed the pre-filled
    // "--" and burned the single prompt for that pack. Reproduced 2/2 boots on 2026-10-05:
    // port 1 logged "ambiguity left unresolved by owner" 1.3 s after asking with nobody
    // touching the device. Seeding prev=LOW means a press is only seen after the pin has
    // gone high again.
    _b1Prev = _b2Prev = _b3Prev = LOW;
    Serial.printf("[DISP] label picker OPEN for port%u (pre-filled #%u)\n",
                  port + 1, (unsigned)_labelPickVal);
}

void displayLoop() {
    if (gSleepCountdownActive) return;  // PowerManager owns the display
    if (!gDisplayOk) return;            // framebuffer alloc failed — LEDs + keep-alive still run

    runtimeSample();                    // keep the worst-pack runtime window warm on every screen

    uint32_t now = millis();

    bool b1 = digitalRead(BUTTON1_PIN);
    bool b2 = digitalRead(BUTTON2_PIN);
    bool b3 = digitalRead(BUTTON3_PIN);

    // Decide which buttons we still trust BEFORE anything acts on them.
    updateStuckButtons(now, b1, b2, b3);

    // Any press counts as activity. This MUST stay above the overlay early-return —
    // it used to sit below it, so a press the overlay consumed never reset the idle
    // timer and the overlay effectively froze the home countdown.
    // EDGE, not level. Testing the level re-armed both timers on every loop pass for as
    // long as any button was down — so a button held, jammed, or SHORTED BY WATER (the
    // actual threat model: a sealed box on open water) pinned _lastInputMs to now and
    // defeated both invariants permanently. A stuck button could park the display off
    // Home, or leave the label picker covering it with no 15 s escape — precisely the
    // failure the disconnect modal was deleted for.
    static bool _anyPrev = false;
    const bool anyDown = (b1 == LOW || b2 == LOW || b3 == LOW);
    if (anyDown && !_anyPrev) {                      // press edge only
        _lastInputMs = now;
        if (_showLabelPick) _overlayShownMs = now;   // an edit in progress never times out
    }
    _anyPrev = anyDown;

    // Unconditional. Above every early return. See the note on enforceHomePolicy().
    enforceHomePolicy(now);

    // ── The one thing allowed to raise an overlay by itself ──────────────────
    // The registry could not tell which stored pack this is (two too close in cycle
    // count). Guessing would file a weak pack's decline against a healthy one, so ask
    // instead — once per insertion. The 15 s timeout still applies, and declining just
    // leaves the pack showing its port tag with no history attributed.
    // Re-arm on packs[i].valid, NOT packConnected(i). packConnected goes false after 5 s
    // of silence but .valid survives to 10 s (UART.ino), and packRegistryIdentify only
    // re-runs on the .valid rising edge — so re-arming on packConnected reopened the
    // picker on any 5-10 s frame glitch while .ambiguous was still set from the original
    // identify. On a marginal connector that is a near-continuous train of 15 s overlays
    // covering Home and eating every button: a self-raising overlay, which is exactly
    // what the disconnect modal was deleted for being.
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) { _ambiguousAsked[i] = false; continue; }   // re-arm only on a real re-seat
        if (packRec[i].ambiguous && !_ambiguousAsked[i] && !_showLabelPick) {
            _ambiguousAsked[i] = true;
            openLabelPicker(i, now);
        }
    }

    // ── Overlay: label picker
    if (_showLabelPick) {
        // Nothing counts during the grace window - see openLabelPicker().
        const bool pickReady = (now - _overlayShownMs) > PICKER_GRACE_MS;
        if (pickReady && _b1Prev == HIGH && b1 == LOW && (now - _b1Ts) > DEBOUNCE_MS) {
            _b1Ts = now;
            _labelPickVal = (_labelPickVal >= NUM_LABELS) ? 0 : _labelPickVal + 1;
        }
        if (pickReady && _b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS) {
            _b2Ts = now;
            Serial.printf("[DISP] picker CONFIRM port%u = #%u\n",
                          _labelPickPort + 1, (unsigned)_labelPickVal);
            labelSet(_labelPickPort, _labelPickVal);
            _showLabelPick = false;
            _dispLast = 0;  // force immediate full redraw on dismiss
        }
        if (pickReady && _b3Prev == HIGH && b3 == LOW && (now - _b3Ts) > DEBOUNCE_MS) {
            _b3Ts = now;
            Serial.printf("[DISP] picker DISMISSED port%u (no change)\n", _labelPickPort + 1);
            _showLabelPick = false;
            _dispLast = 0;
        }
        _b1Prev = b1; _b2Prev = b2; _b3Prev = b3;
        if (_showLabelPick) { drawLabelPicker(); _canvas->flush(); }
        return;
    }

    // ── Normal button handling
    // (disconnect-modal block removed 2026-10 — a missing pack now just vanishes from
    //  the Home gauges; the dropout is still recorded in the CSV by writePackEdges())

    // 2026-10-06 - BTN2+BTN3 "light" COMBO DELETED, and the b3==HIGH gate on BTN2 with it.
    //
    // THE BUG THE OWNER REPORTED: "buttons 2 and 3 don't work to change screens... button 2
    // turns on a little indicator called LGT... whether WiFi is on or not, I cannot change
    // screens afterwards."
    //
    // Every symptom follows from BTN3 reading stuck LOW, and the software turned ONE stuck
    // button into THREE broken functions:
    //   - BTN2's screen-change required `b3 == HIGH` to avoid colliding with this combo, so
    //     a low b3 disabled screen navigation permanently.
    //   - This combo then fired on every BTN2 press instead, which is why the only thing the
    //     button appeared to do was flip LGT.
    //   - BTN3 itself needs a FALLING edge; a pin already low never produces one, so it did
    //     nothing at all.
    //   - BTN1 kept working because it has no cross-button gate. The owner observed exactly
    //     that asymmetry.
    //
    // gLightOn drove NOTHING. GPIO13 became NeoPixel strip 2 when the light FET was retired
    // (see ledInit()), so the flag and its on-screen "LGT" were pure vestige. A dead feature
    // was disabling a live one. Deleting it removes the gate, and BTN2/BTN3 become
    // independent — a stuck button can now only break ITSELF.
    //
    // If a real light is ever wired, it comes back as its own control, not as a chord.

    // BTN1: record press time on falling edge, act on rising edge (release).
    // Ignores releases from a hold >= SLEEP_HOLD_MS — device sleeps before release.
    if (b1 == LOW && _b1Prev == HIGH) _b1Ts = now;
    if (_b1Prev == LOW && b1 == HIGH) {
        uint32_t held = now - _b1Ts;
        // R-3: WiFi used to toggle on a BARE SHORT PRESS, and the station join now blocks
        // loop() for up to 15 s. A 100-400 ms water glitch on BTN1 would therefore freeze
        // pack reads, logging, LEDs and button sampling mid-ride. A deliberate hold cannot
        // be produced by a glitch, and sits clear of the 4 s sleep arm.
        #define WIFI_TOGGLE_HOLD_MS 1500UL
        if (held >= DEBOUNCE_MS && held < SLEEP_HOLD_MS) {
            if (_screen == 1 && held < WIFI_TOGGLE_HOLD_MS) {
                _detailPack = nextConnectedPack(_detailPack);   // cycle connected packs
            } else if (_screen == 0 && held >= WIFI_TOGGLE_HOLD_MS) {
                wifiToggle();
            }
        }
    }
    _b1Prev = b1;

    // BTN2: next screen. No cross-button gate any more — see the deleted-combo note above.
    // A stuck BTN3 used to disable this permanently; now the two are independent.
    if (_b2Prev == HIGH && b2 == LOW && (now - _b2Ts) > DEBOUNCE_MS && !_btnStuck[1]) {
        _b2Ts   = now;
        _screen = (_screen + 1) % NUM_SCREENS;
    }
    _b2Prev = b2;

    // BTN3: prev screen (short) / label assign (long on screen 0 or 1)
    if (b3 == LOW && _b3Prev == HIGH) _b3Ts = now;
    if (_b3Prev == LOW && b3 == HIGH && !_btnStuck[2]) {
        uint32_t held = now - _b3Ts;
        if (held >= LONGPRESS_MS && (_screen == 0 || _screen == 1)) {
            // 2026-10 - Was screen 0 only, and hardcoded port 0, so ports 2-4 were
            // reachable ONLY through the disconnect modal — which no longer exists.
            // Screen 1 names the pack it is showing (BTN1 there cycles connected
            // packs, so every port is reachable); screen 0 names the first connected.
            uint8_t port = 0;
            if (_screen == 1) {
                port = _detailPack;
            } else {
                for (uint8_t i = 0; i < NUM_PACKS; i++) {
                    if (packConnected(i)) { port = i; break; }
                }
            }
            openLabelPicker(port, now);
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
    // Seeding _lastInputMs is what makes the jump survive. Without it the idle timer is
    // almost always already expired (the device sits on Home untouched), so
    // enforceHomePolicy snapped straight back on the very next iteration and the
    // charging screen appeared for well under one frame — a flash, not a screen.
    if (charging && !_wasCharging)              { _screen = 2; _dispLast = 0; _lastInputMs = now; }
    else if (!charging && _wasCharging && _screen == 2) { _screen = 0; _dispLast = 0; } // charging ended
    _wasCharging = charging;
    // The idle auto-return used to live here, gated on !charging. Both the position
    // and the gate were wrong: below the overlay early-returns it could be suppressed,
    // and the gate meant the display never came home while a charger was plugged in.
    // It now lives in enforceHomePolicy(), at the top of this function, ungated.
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
