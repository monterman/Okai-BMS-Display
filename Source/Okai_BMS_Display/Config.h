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
#define BUTTON1_PIN      0          // Boot / WiFi toggle
#define BUTTON2_PIN     14          // spare

// ─── WiFi AP ─────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID     "OkaiBMS"
#define WIFI_AP_PASSWORD "12345678"
