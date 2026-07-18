#pragma once

// ─── Firmware version ────────────────────────────────────────────────────────
#define FW_VERSION "0.3.0"

// ─── Pack count ──────────────────────────────────────────────────────────────
#define NUM_PACKS 4

// ─── UART / SoftSerial pin assignments ───────────────────────────────────────
// T-Display-S3 free GPIOs: 1, 2, 10, 11, 12, 13, 16, 17, 18, 21
// Display parallel bus:    5,6,7,8,9,39,40,41,42,45,46,47,48
// Avoid: 0(BOOT), 3,4(ADC), 35-37(PSRAM), 43(TX0), 44(RX0)

#define PACK1_RX_PIN   1
#define PACK1_TX_PIN   2   // shared TX bus — wire to ALL pack RX lines
#define PACK2_RX_PIN  16
#define PACK2_TX_PIN  PACK1_TX_PIN
#define PACK3_RX_PIN  17
#define PACK3_TX_PIN  PACK1_TX_PIN
#define PACK4_RX_PIN  18
#define PACK4_TX_PIN  PACK1_TX_PIN
#define HEARTBEAT_TX_PIN  PACK1_TX_PIN

// ─── RTC (DS3231) ────────────────────────────────────────────────────────────
// Connect DS3231 module: VCC→3V3, GND→GND, SDA→GPIO11, SCL→GPIO12
// Library: "RTClib" by Adafruit — install via Library Manager
#define RTC_SDA_PIN  11
#define RTC_SCL_PIN  12

// ─── UART baud ───────────────────────────────────────────────────────────────
#define BMS_BAUD 9600

// ─── Heartbeat ───────────────────────────────────────────────────────────────
// Official jsutcliff/OKAI-Battery-Lib sends the unlock every 1 s; the pack only
// tolerates up to 5 s before it stops outputting. 5000 sat exactly on that limit
// and got starved by the cooperative loop → packs dropped. Back to the proven 1 s.
#define HEARTBEAT_INTERVAL_MS 1000UL

// ─── Smart logging ───────────────────────────────────────────────────────────
// Two streams: R_YYYYMMDD_NNN_S.csv (ride) and C_YYYYMMDD_NNN_S.csv (charge)
// NNN = session counter (000-999, wraps), S = segment within session (1,2,3…)
// Falls back to R_SXXX_S.csv / C_SXXX_S.csv when RTC has no time set
#define LOG_RIDE_INTERVAL_MS    5000UL    // 5 s while riding
#define LOG_CHARGE_INTERVAL_MS  30000UL   // 30 s while charging
#define LOG_RIDE_THRESHOLD_A    1.0f      // A discharge → "riding"
#define LOG_RIDE_HYSTERESIS_MS  120000UL  // keep RIDE open 2 min after current drops
#define LOG_MAX_FILE_BYTES      262144UL  // 256 KB per segment → roll to _2, _3…

// ─── Pack labels ─────────────────────────────────────────────────────────────
// Labels 1-8; 0 = unassigned (shows as P1/P2/P3/P4 in filenames)
#define NUM_LABELS 8

// ─── Cell health thresholds ──────────────────────────────────────────────────
#define CELL_DELTA_WARN_V  0.050f   // 50 mV
#define CELL_DELTA_POOR_V  0.100f   // 100 mV

// ─── Pack registry ───────────────────────────────────────────────────────────
#define PACK_HISTORY_LEN    10   // rolling spread/SoH entries stored per pack
#define PACK_CYC_TOLERANCE 500   // max cycle-count drift allowed for re-identification

// ─── Display ─────────────────────────────────────────────────────────────────
#define TFT_BL_PIN      38
#define DISPLAY_REFRESH_MS  500UL

// ─── Onboard battery sense (LilyGo T-Display-S3 18650) ───────────────────────
// GPIO4 = onboard battery ADC, fed through a 2:1 divider (two equal resistors).
// Vbat = analogReadMilliVolts(4) × 2. Single Li-ion range: 3.0 V empty → 4.2 V full.
// GPIO4 is reserved-for-ADC in the pin map above; nothing else uses it.
#define BAT_ADC_PIN     4

// ─── TFT parallel bus (T-Display-S3 ST7789 8-bit) ────────────────────────────
#define TFT_DC   7
#define TFT_CS   6
#define TFT_WR   8
#define TFT_RD   9
#define TFT_RST  5
#define TFT_D0  39
#define TFT_D1  40
#define TFT_D2  41
#define TFT_D3  42
#define TFT_D4  45
#define TFT_D5  46
#define TFT_D6  47
#define TFT_D7  48

// ─── Buttons ─────────────────────────────────────────────────────────────────
#define BUTTON1_PIN      0   // BTN1: WiFi toggle / confirm (short) | sleep (4s hold)
#define BUTTON2_PIN     14   // BTN2: next screen →
#define BUTTON3_PIN     21   // BTN3: prev screen ←  (long-press = label assign)

// ─── Power management ─────────────────────────────────────────────────────────
#define POWER_EN_PIN    15       // LiPo power latch — must stay HIGH while running
#define SLEEP_HOLD_MS   4000UL   // hold BTN1 this long → deep sleep; press BTN1 to wake

// ─── Light FET output (LEGACY — RETIRED) ─────────────────────────────────────
// ⚠ GPIO13 is now NeoPixel status strip 2 (see below). The flag-light MOSFET is
// deprecated and NO LONGER DRIVEN by firmware — the display must not touch GPIO13
// or it fights the strip's RMT signal. Do not wire a light to GPIO13 anymore.
#define LIGHT_FET_PIN  13        // (legacy) superseded by LED2_PIN — not driven

// ─── Status LED bars (NeoPixel WS2812) — 2× 12 px, one data pin each ─────────
// Layout per strip: pack A [px0-4] | gap [px5-6 dark] | pack B [px7-10] | KA [px11].
// px11 on BOTH strips is the keep-alive indicator (white slow-blink = healthy,
// solid red = watchdog-stalled). Low brightness; 3V3 + GND.
#define LED1_PIN  10             // NeoPixel strip 1 (packs 1+2)
#define LED2_PIN  13             // NeoPixel strip 2 (packs 3+4) — takes GPIO13 from retired FET

// ─── Keep-alive watchdog / edge-pixel indicator ─────────────────────────────
// The LED indicator reads g_hbLastMs (published by the Core-1 heartbeat task each
// beat). If no beat within HB_WATCHDOG_MS → indicator goes SOLID RED. 2500 ms is
// > the 1 s cadence + jitter yet well inside the pack's 5 s sleep deadline.
#define HB_WATCHDOG_MS  2500UL
extern volatile uint32_t g_hbLastMs;   // Heartbeat.ino — last keep-alive beat, millis()

// ─── Dynamic home screen ─────────────────────────────────────────────────────
#define HOME_IDLE_MS       180000UL    // 3 min with no button input → auto-return to Home
#define PACK_CONNECTED_MS  5000UL      // no fresh frame in this long → pack is dead/disconnected

// ─── Runtime ("time remaining") estimator ────────────────────────────────────
// Displayed "~X min" = the WORST connected pack (soonest to hit the reserve). Each
// pack independently feeds a motor, so a mean would over-promise. Method: per-pack
// SOC-decline slope (primary) blended with per-pack current/power (cross-check).
#define RUNTIME_SAMPLE_MS    10000UL   // per-pack SOC ring sample cadence
#define RUNTIME_WINDOW_MS    150000UL  // 2.5 min trailing window for the decline slope
#define RUNTIME_MIN_SPAN_MS  45000UL   // need >=45 s of data before showing a number
#define RUNTIME_RESERVE_PCT  15        // per-pack stop-riding reserve (extrapolate to here)

// ─── WiFi AP ─────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID     "OkaiBMS"
#define WIFI_AP_PASSWORD "12345678"

// ─── Pack energy design specs (Panasonic NCR18650BD 10S4P) ───────────────────
#define PACK_DESIGN_AH   12.8f    // 4P × 3.2 Ah rated
#define PACK_NOMINAL_V   36.0f    // 10S × 3.6 V nominal
#define PACK_DESIGN_WH   460.8f   // PACK_DESIGN_AH × PACK_NOMINAL_V

// ─── Log mode (shared between Logger.ino and Display.ino) ────────────────────
typedef enum : uint8_t { LOG_IDLE = 0, LOG_RIDE = 1, LOG_CHARGE = 2 } LogMode;

// ─── Per-pack data ───────────────────────────────────────────────────────────
struct PackData {
    float    voltage;
    float    current;
    float    cellHigh;
    float    cellLow;
    uint8_t  soc;
    uint8_t  maxSoc;           // max achievable SOC / SoH indicator (b[06])
    uint8_t  maxTemp;
    uint16_t cycles;
    uint8_t  rawStatus;
    bool     chargerDetected;
    bool     isCharging;
    bool     chargeDone;
    float    whIn;           // session Wh accumulated (charging)
    float    whOut;          // session Wh accumulated (discharging)
    bool     valid;
    uint32_t lastUpdateMs;
};

// ─── Per-pack registry record ─────────────────────────────────────────────────
struct PackRecord {
    uint16_t regCYC;                           // cycle count at registration (UUID)
    uint8_t  maxSocAtReg;                      // maxSoc at registration (tiebreaker)
    char     firstSeen[12];                    // "YYYY-MM-DD\0"
    uint16_t currentCycles;
    uint16_t sessions;
    float    totalWhCharged;
    float    totalWhDischarged;
    uint16_t spreadHistory[PACK_HISTORY_LEN];  // cell spread mV per session
    uint8_t  sohHistory[PACK_HISTORY_LEN];     // maxSoc % per session
    uint8_t  historyLen;
    bool     known;
    char     cycID[10];                        // "CYC-XXXX\0"
};

// ─── Cross-file globals ───────────────────────────────────────────────────────
extern PackData   packs[NUM_PACKS];    // UART.ino
extern float      g_ridePowerEma_W;    // UART.ino — EMA fleet discharge power
extern uint8_t    g_ridePowerN;        // UART.ino — warmup counter (< 3 = not ready)
extern bool     wifiActive;          // WiFiServer.ino
extern bool     fsReady;             // Logger.ino

// ─── Cross-file function prototypes (PackLabel.ino) ──────────────────────────
// needed by Logger.ino and Display.ino which compile before PackLabel.ino
void     packlabelInit();
uint8_t  labelGet(uint8_t port);
void     labelSet(uint8_t port, uint8_t label);
void     labelStr(uint8_t port, char *buf, size_t len);
bool     timeIsSynced();
time_t   timeNowSec();
void     timeSyncSet(int64_t browserEpochMs);
LogMode  logCurrentMode();           // Logger.ino — prototype for Display.ino

// ─── Cross-file function prototypes (PackRegistry.ino) ───────────────────────
extern PackRecord packRec[NUM_PACKS];
void packRegistryInit();
void packRegistryIdentify(uint8_t port);
void packRegistryRegister(uint8_t port);
void packRegistrySessionUpdate(uint8_t port);
const PackRecord* packRegGet(uint8_t port);
