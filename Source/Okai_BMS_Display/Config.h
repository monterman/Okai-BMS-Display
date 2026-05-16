#pragma once

// ─── Firmware version ───────────────────────────────────────────────────────
#define FW_VERSION "0.1.0"

// ─── Pack count ──────────────────────────────────────────────────────────────
#define NUM_PACKS 4

// ─── UART pins ───────────────────────────────────────────────────────────────
// UART1 — Pack 1
#define PACK1_RX_PIN  18
#define PACK1_TX_PIN  17

// UART2 — Pack 2
#define PACK2_RX_PIN  16
#define PACK2_TX_PIN  15

// EspSoftwareSerial — Pack 3 (choose free GPIO, avoid strapping pins)
#define PACK3_RX_PIN  12
#define PACK3_TX_PIN  11

// EspSoftwareSerial — Pack 4
#define PACK4_RX_PIN  10
#define PACK4_TX_PIN   9

// ─── UART baud ───────────────────────────────────────────────────────────────
#define BMS_BAUD 9600

// ─── Heartbeat ───────────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS 5000UL
static const uint8_t HEARTBEAT_SEQ[] = { 0x3A, 0x13, 0x01, 0x16, 0x79 };
#define HEARTBEAT_LEN 5

// ─── Logging ─────────────────────────────────────────────────────────────────
#define LOG_INTERVAL_MS    10000UL
#define LOG_FILENAME       "/bms_log.csv"
#define LOG_CSV_HEADER     "Timestamp,PackID,SOC,Voltage,Current,MaxCellVolt,MinCellVolt,MaxTemp\n"

// ─── Cell health thresholds (mV) ─────────────────────────────────────────────
#define CELL_DELTA_WARN_MV  50    // yellow — watch
#define CELL_DELTA_POOR_MV  100   // red — flag as POOR

// ─── Display ─────────────────────────────────────────────────────────────────
#define TFT_BG_COLOR    TFT_BLACK
#define TFT_GOOD_COLOR  TFT_GREEN
#define TFT_WARN_COLOR  TFT_YELLOW
#define TFT_POOR_COLOR  TFT_RED

// ─── WiFi AP ─────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID     "OkaiBMS"
#define WIFI_AP_PASSWORD "bmsdata1"
#define WIFI_BUTTON_PIN  0          // LILYGO T-Display-S3 built-in button (GPIO0)
