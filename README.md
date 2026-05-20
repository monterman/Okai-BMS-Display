# Okai BMS Display

Real-time telemetry display, CSV logger, and pack health tracker for up to four Ruipu/Okai 10S4P lithium battery packs.

**Hardware:** LILYGO T-Display-S3 (ESP32-S3 · 1.9" 320×170 ST7789 TFT)  
**Protocol:** Ruipu/Okai 9600 baud 8N1, 36-byte frames, Dallas/Maxim 1-wire CRC  
**Firmware version:** 0.2.0

---

## What it does

- **4-pack real-time display** — SOC, voltage, current, cell spread, temperature, health rating
- **Ride time estimate** — EMA-smoothed fleet power draw → `~47 min left` across 2/3/4 connected packs
- **Fleet energy totals** — remaining Wh and mAh summed across all connected packs, design capacity reference
- **Smart CSV logging** — auto-detects ride vs charge, timestamps from DS3231 RTC or browser sync, 256 KB file rolling
- **Pack UUID registry** — identifies each physical pack by CYC fingerprint (charge cycle count), persists lifetime Wh charged/discharged, session count, cell-spread and SoH history across reboots
- **WiFi dashboard** — AP mode, live pack table, log download/delete, pack registry view (`/packs`), raw frame dump

---

## Screens

| # | Name | BTN1 action |
|---|------|-------------|
| 0 | **Fleet Overview** — 2×2 grid, all packs at a glance. Empty slots show fleet energy summary. | WiFi toggle |
| 1 | **Per-Pack Detail** — full telemetry for one pack, CYC fingerprint, SoH, session Wh | Cycle P1→P4 |
| 2 | **Ride Energy** (riding) — `~47 min left`, per-pack Wh/mAh, fleet totals + design reference | — |
| 2 | **Charging Live** (charging) — SOC fill bars, charge ETA per pack, total watts | — |
| 3 | **Cell Health** — spread mV, cycle count, SoH%, available Wh, worst-pack flag | — |

**BTN2** = next screen →  **BTN3** = prev screen ←  **BTN3 long-press on Screen 0** = label assignment

See [`docs/Screens.md`](docs/Screens.md) for full layout diagrams and field descriptions.

---

## WiFi Dashboard

Press **BTN1** on Screen 0, connect to AP `OkaiBMS` / `12345678`, open `http://192.168.4.1`

| Route | Description |
|-------|-------------|
| `/` | Live pack status + log file list |
| `/packs` | Lifetime pack registry (CYC ID, Wh totals, SoH history) |
| `/csv?f=/NAME.csv` | Download a log file |
| `/rawdump` | Live 36-byte hex frame dump |

---

## Hardware Setup

| Pack | Port | GPIO RX | GPIO TX |
|------|------|---------|---------|
| Pack 1 | UART1 | GPIO 1 | GPIO 2 (shared TX bus) |
| Pack 2 | UART2 | GPIO 16 | GPIO 2 |
| Pack 3 | SoftSerial | GPIO 17 | GPIO 2 |
| Pack 4 | SoftSerial | GPIO 18 | GPIO 2 |
| RTC DS3231 | I2C | SDA GPIO 11 | SCL GPIO 12 |

**Each pack RX line requires a 1 kΩ pull-up to 3.3 V** (open-collector output from the BMS green wire).  
All four pack RX lines share one TX (GPIO 2) — a single heartbeat reaches all packs simultaneously.

See [`docs/Board_Wiring.md`](docs/Board_Wiring.md) and [`docs/connector-pinout.html`](docs/connector-pinout.html) for full wiring diagrams.

---

## Library Dependencies

Install all via Arduino Library Manager:

| Library | Author | Purpose |
|---------|--------|---------|
| Arduino_GFX_Library | moononournation | TFT driver |
| EspSoftwareSerial | Dirk Kaar | Pack 3 & 4 UART |
| RTClib | Adafruit | DS3231 RTC |

LittleFS is built into the ESP32 Arduino core (no install needed).

---

## Build Settings

- **Board:** LILYGO T-Display-S3 (or ESP32S3 Dev Module)
- **Partition scheme:** `Huge APP` — required; default partition leaves no headroom
- **Flash size:** 16 MB
- **PSRAM:** OPI PSRAM

---

## Key Thresholds

| Parameter | Value |
|-----------|-------|
| Cell spread WARN | 50 mV |
| Cell spread POOR | 100 mV |
| Pack design capacity | 460.8 Wh / 12,800 mAh (NCR18650BD 10S4P) |
| Heartbeat interval | 5 s |
| Ride log interval | 5 s |
| Charge log interval | 30 s |
| Log file max size | 256 KB (rolls to next segment) |
| Pack re-identification tolerance | ± 500 charge cycles |

---

## Docs

| File | Description |
|------|-------------|
| [`docs/Screens.md`](docs/Screens.md) | Full screen layout diagrams and field reference |
| [`docs/Pack_Registry.md`](docs/Pack_Registry.md) | CYC fingerprint fleet registry, known packs |
| [`docs/Protocol_Notes.md`](docs/Protocol_Notes.md) | BMS protocol, heartbeat, 0x2020 artifact, bench tap |
| [`docs/Board_Wiring.md`](docs/Board_Wiring.md) | Physical wiring, pull-up resistors |
| [`docs/UART_Topology.md`](docs/UART_Topology.md) | GPIO and port assignments |
| [`docs/connector-pinout.html`](docs/connector-pinout.html) | Interactive wiring diagrams (open in browser) |
