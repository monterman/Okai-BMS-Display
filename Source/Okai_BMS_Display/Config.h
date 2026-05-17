#pragma once

// ─── Firmware version ────────────────────────────────────────────────────────
#define FW_VERSION "0.1.0"

// ─── Pack count ──────────────────────────────────────────────────────────────
#define NUM_PACKS 4

// ─── UART / SoftSerial pin assignments ───────────────────────────────────────
// T-Display-S3 free GPIOs: 1, 2, 16, 17, 18, 21
// Display parallel bus occupies: 5,6,7,8,9,38,39,40,41,42,45,46,47,48
// Avoid: 0(BOOT), 3,4(ADC/strapping), 10-13(PSRAM/Flash), 14(BTN2), 15(PWR_EN),
//        43(TX0), 44(RX0)

// UART1 — Pack 1 (hardware)
#define PACK1_RX_PIN   1
#define PACK1_TX_PIN   2   // shared TX bus — wire to ALL pack RX lines

// UART2 — Pack 2 (hardware)
#define PACK2_RX_PIN  16
#define PACK2_TX_PIN  PACK1_TX_PIN   // same shared TX

// EspSoftwareSerial — Pack 3
#define PACK3_RX_PIN  17
#define PACK3_TX_PIN  PACK1_TX_PIN   // same shared TX

// EspSoftwareSerial — Pack 4
#define PACK4_RX_PIN  18
#define PACK4_TX_PIN  PACK1_TX_PIN   // same shared TX

// Shared heartbeat TX — one ESP32 pin drives all 4 pack RX lines in parallel
#define HEARTBEAT_TX_PIN  PACK1_TX_PIN

// ─── UART baud ───────────────────────────────────────────────────────────────
#define BMS_BAUD 9600

// ─── Heartbeat ───────────────────────────────────────────────────────────────
// Handled by RuipuBattery::unlock() — call every 5s per pack, non-blocking
#define HEARTBEAT_INTERVAL_MS 5000UL

// ─── Logging ─────────────────────────────────────────────────────────────────
#define LOG_INTERVAL_MS  10000UL
#define LOG_FILENAME     "/bms_log.csv"
#define LOG_CSV_HEADER   "Timestamp,PackID,SOC,Voltage,Current,CellHigh,CellLow,MaxTemp\n"

// ─── Cell health thresholds (Volts — high() and low() return float V) ────────
// Panasonic 18650 10S packs ~10 years old; delta flags imbalance/cell degradation
#define CELL_DELTA_WARN_V  0.050f   // 50 mV — watch
#define CELL_DELTA_POOR_V  0.100f   // 100 mV — flag POOR

// ─── Display ─────────────────────────────────────────────────────────────────
#define TFT_BG_COLOR    TFT_BLACK
#define TFT_GOOD_COLOR  TFT_GREEN
#define TFT_WARN_COLOR  TFT_YELLOW
#define TFT_POOR_COLOR  TFT_RED
#define TFT_BL_PIN      38          // backlight enable (HIGH = on)

// ─── Buttons ─────────────────────────────────────────────────────────────────
// All three buttons are active-LOW (press pulls GPIO to GND, use INPUT_PULLUP).
// Button 1 + 2: solder wires from onboard PCB button pads to external
//               waterproof momentary switches mounted on enclosure.
// Button 3: solder wire from GPIO 21 header pin to third external switch.
// Switch LEDs: wire always-on — LED+ → 150Ω → 5V rail, LED− → GND. No GPIO needed.
#define BUTTON1_PIN      0          // WiFi AP toggle (onboard boot btn → external)
#define BUTTON2_PIN     14          // User action   (onboard btn2   → external)
#define BUTTON3_PIN     21          // User action   (GPIO 21 header → external)

// ─── WiFi AP ─────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID     "OkaiBMS"
#define WIFI_AP_PASSWORD "12345678"

// ─── Display refresh ─────────────────────────────────────────────────────────
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

// ─── Pack energy design specs (Panasonic NCR18650BD in 10S4P config) ─────────
// Cell:  3.6 V nominal, 3.2 Ah rated → 11.52 Wh per cell
// Pack:  10S × 3.6 V = 36 V nominal;  4P × 3.2 Ah = 12.8 Ah → 460.8 Wh
// Used for: ETA, available-energy display, SoH % vs brand-new
#define PACK_DESIGN_AH   12.8f    // brand-new pack rated Ah
#define PACK_NOMINAL_V   36.0f    // nominal pack voltage (10S × 3.6 V)
#define PACK_DESIGN_WH   460.8f   // PACK_DESIGN_AH × PACK_NOMINAL_V

// ─── Per-pack data (updated by uartLoop, read by display/logger/wifi) ─────────
struct PackData {
    float    voltage;         // V  total pack
    float    current;         // A  + = charging, − = discharging
    float    cellHigh;        // V  highest cell
    float    cellLow;         // V  lowest cell
    uint8_t  soc;             // 0-100 %
    uint8_t  maxTemp;         // °C
    uint16_t cycles;          // charge cycle count
    uint8_t  rawStatus;       // BMS status byte (byte 3)
    bool     chargerDetected; // charger is plugged in
    bool     isCharging;      // bulk-charging actively
    bool     chargeDone;      // charger plugged + 100% + not bulk
    float    whIn;            // Wh accumulated charging  (session, since boot)
    float    whOut;           // Wh accumulated discharging (session, since boot)
    bool     valid;           // true after first good frame
    uint32_t lastUpdateMs;    // millis() of last successful read
};

// ─── Cross-file globals (defined in their respective .ino files) ──────────────
extern PackData packs[NUM_PACKS];   // UART.ino
extern bool     wifiActive;          // WiFiServer.ino
extern bool     fsReady;             // Logger.ino
