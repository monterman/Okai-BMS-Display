#pragma once

// ─── Firmware version ────────────────────────────────────────────────────────
#define FW_VERSION "0.2.0"

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
#define HEARTBEAT_INTERVAL_MS 5000UL

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
#define BUTTON1_PIN      0   // BTN1: WiFi toggle / confirm action
#define BUTTON2_PIN     14   // BTN2: next screen →
#define BUTTON3_PIN     21   // BTN3: prev screen ←  (long-press = label assign)

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
