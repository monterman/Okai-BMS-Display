# Software Developer Brief: Okai 10S4P Telemetry Logger & Diagnostic Display

*Preserved as-is from original brief — 2026-05-16*

## Hardware Target

- **Microcontroller:** LILYGO T-Display-S3 (ESP32-S3, 1.9" ST7789 TFT LCD, Native USB-C)
- **Power Supply:** System is powered continuously by a stable 5V VESC UART rail
- **Inputs:** Up to four Ruipu/Okai 10S4P battery packs broadcasting telemetry at 9600 baud

## Priority 1: The Keep-Alive Heartbeat (Mission Critical)

The system's absolute highest priority is preventing the BMS units from sleeping under load.

A hex sequence (`0x3A, 0x13, 0x01, 0x16, 0x79`) must be transmitted to all connected packs every 5 seconds.

**Constraint:** This routine must be strictly non-blocking. No `delay()` functions can be used anywhere in the firmware. Logging routines, file handling, and screen drawing must never interrupt or delay this heartbeat.

## Priority 2: UART Topology & Protocol Integration

Utilize the jsutcliff/OKAI-Battery-Lib to parse the incoming Ruipu protocol.

The ESP32-S3 must handle up to 4 parallel data streams:

- **UART0:** Reserved for native USB-C debugging and serial CSV extraction
- **UART1:** RX assigned to Pack 1
- **UART2:** RX assigned to Pack 2
- **SoftwareSerial 1 & 2:** Implement robust emulated serial instances for Pack 3 and Pack 4

**Port Mapping:** Because the BMS does not broadcast a unique internal serial ID, the physical RX pin dictates the "Pack ID" in the logs and on the screen.

## Priority 3: Crash-Proof Incremental Logging

Telemetry must be logged to the ESP32-S3's internal flash (LittleFS) every 10 seconds.

**Constraint:** The log must be appended and the file buffer flushed to the flash memory incrementally. In the event of an abrupt power loss, no more than 10 to 30 seconds of data should be lost.

**CSV Format:** `Timestamp, PackID, SOC, Voltage, Current, MaxCellVolt, MinCellVolt, MaxTemp`

## Priority 4: UI Design & Health Heuristics

Utilize the 1.9-inch TFT to display a 4-row interface (one dedicated row per pack).

- **UI Design:** Maximize sunlight readability through a transparent enclosure by using a pure black background with high-contrast, thick fonts
- **Data Fitness Algorithm:** Evaluate the delta between MaxCellVolt and MinCellVolt for each pack. If the delta exceeds a safe threshold (indicating an imbalanced or failing series group), visually flag that pack's status (green "GOOD" → red "POOR")
- **Display Fields Per Row:** `[Pack ID] | [Voltage] | [Current] | [Health Status]`

## Priority 5: Radios & Data Extraction

WiFi and Bluetooth radios must be explicitly disabled on boot to conserve power and reduce thermal load.

Implement an interrupt tied to the LilyGO's built-in tactile button. When pressed, the ESP32 should wake the WiFi radio and host a lightweight asynchronous web server. This will allow the user to connect via a smartphone to easily download the CSV logs after a session without needing to open the waterproof enclosure.
