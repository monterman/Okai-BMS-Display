# Okai BMS Display

Telemetry logger and diagnostic display for up to four Ruipu/Okai 10S4P battery packs.

**Hardware:** LILYGO T-Display-S3 (ESP32-S3, 1.9" ST7789 TFT)  
**Status:** In development

## Priority order

1. Non-blocking 5s keep-alive heartbeat to all packs (mission critical)
2. UART topology — UART1/UART2 + EspSoftwareSerial for Pack 3/4
3. Crash-proof incremental LittleFS logging (10s flush)
4. 4-row TFT UI with cell health heuristic
5. Button-triggered WiFi AP + async CSV download

## Defaults

| Setting | Value |
|---|---|
| Cell delta warn | 50 mV |
| Cell delta poor | 100 mV |
| WiFi mode | AP (field-safe, no router needed) |
| Display lib | TFT_eSPI |
| SoftSerial lib | EspSoftwareSerial |

## Docs

- `docs/Brief.md` — original design brief
- `docs/UART_Topology.md` — wiring reference
