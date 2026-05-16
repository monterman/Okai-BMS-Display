# UART Topology — Okai BMS Display

## Port Assignment

| Port | Type | Pack | RX Pin | TX Pin | Notes |
|---|---|---|---|---|---|
| UART0 | Hardware | — | GPIO44 | GPIO43 | Reserved — USB-C debug + CSV extraction |
| UART1 | Hardware | Pack 1 | GPIO18 | GPIO17 | Full hardware UART |
| UART2 | Hardware | Pack 2 | GPIO16 | GPIO15 | Full hardware UART |
| SoftSerial 1 | EspSoftwareSerial | Pack 3 | TBD | TBD | Confirm pin before wiring |
| SoftSerial 2 | EspSoftwareSerial | Pack 4 | TBD | TBD | Confirm pin before wiring |

*TX pins above are used for heartbeat transmission. Update TBD pins before first flash.*

## Heartbeat

Sequence: `0x3A 0x13 0x01 0x16 0x79` — sent to all 4 packs every 5 seconds via their respective TX lines.

TX topology: **separate TX per pack** (4 independent lines). More wiring but allows per-pack fault isolation.

## Baud Rate

All packs: **9600 baud**

## Library

SoftwareSerial implementation: **EspSoftwareSerial** (not the classic Arduino one).  
Reason: maintained, interrupt-safe on ESP32, handles 9600 baud reliably on most GPIO.
