# UART Topology — Okai BMS Display

## T-Display-S3 Free GPIO Budget

The 1.9" ST7789 uses an **8-bit parallel bus** (not SPI), consuming most high-numbered GPIOs.

| Status | GPIOs |
|---|---|
| Display data bus (D0–D7) | 39, 40, 41, 42, 45, 46, 47, 48 |
| Display control (CS/DC/RST/WR/RD) | 5, 6, 7, 8, 9 |
| Backlight | 38 |
| UART0 (USB-C debug) | 43 (TX), 44 (RX) |
| Buttons | 0 (BTN1), 14 (BTN2), 21 (BTN3) |
| Power enable | 15 |
| Battery ADC | 4 |
| Pack UART RX | 1 (P1), 16 (P2), 17 (P3), 18 (P4) |
| Pack shared TX (heartbeat) | 2 |
| RTC DS3231 (I²C) | 11 (SDA), 12 (SCL) |
| NeoPixel status bars | 10 (strip 1), 13 (strip 2) |
| Strapping — avoid | 3, 45, 46 |
| **Free remaining** | **none clean** — only 43/44 (if debug → USB-CDC) or 3 (strapping, caution) |

> **Corrected 2026-06-07:** GPIO 10–13 are NOT PSRAM (OPI PSRAM is internal to the package). They are
> general-purpose and are now allocated: RTC on 11/12, NeoPixel status bars on 10/13. The earlier "free =
> 1,2,16,17,18,21" was wrong — those are the pack UARTs + BTN3. Authoritative pin map: `Board_Wiring.md`.

## Port Assignment

| Port | Type | Pack | RX Pin | Notes |
|---|---|---|---|---|
| UART1 | Hardware | Pack 1 | GPIO 1 | Full hardware UART |
| UART2 | Hardware | Pack 2 | GPIO 16 | Full hardware UART |
| SoftSerial 1 | EspSoftwareSerial | Pack 3 | GPIO 17 | 9600 baud, reliable |
| SoftSerial 2 | EspSoftwareSerial | Pack 4 | GPIO 18 | 9600 baud, reliable |

## Shared TX — Heartbeat Bus

**GPIO 2** drives all four pack RX lines in parallel (wire all 4 pack RX pins together to GPIO 2).

This matches the user's existing Arduino Nano wiring. The ESP32-S3 drives the shared line. Each pack sees the same keep-alive packet — no per-pack TX needed.

```
ESP32-S3 GPIO2 (TX) ──┬── Pack1 RX
                       ├── Pack2 RX
                       ├── Pack3 RX
                       └── Pack4 RX
```

## Keep-Alive / Heartbeat

Handled by `RuipuBattery::unlock()` — called every 5 s per pack instance (non-blocking, millis-based). Do NOT call `delay()` anywhere.

## Baud Rate

All packs: **9600 baud**

## SoftwareSerial Library

**EspSoftwareSerial** (maintained fork, interrupt-safe on ESP32-S3). Install via Arduino Library Manager: `EspSoftwareSerial` by Dirk Kaar.

## Pin Reference Image

Official LILYGO T-Display-S3 pinout:
`https://github.com/Xinyuan-LilyGO/T-Display-S3/blob/main/image/T-DISPLAY-S3.jpg`
