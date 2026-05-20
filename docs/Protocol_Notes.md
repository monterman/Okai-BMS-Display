# BMS Protocol Field Notes — Ruipu / Okai 10S4P

Observations from bench sessions. Supplements the jsutcliff/OKAI-Battery-Lib source (`RuipuBattery.cpp`).

---

## Heartbeat Requirement

The BMS will not transmit unless it receives an unlock frame every ≤5 seconds:

```
0x3A  0x13  0x01  0x16  0x79
```

Passive sniffing alone (FTDI RX only, no TX) yields silence. The heartbeat must be sent on the BLUE wire before any data appears on the GREEN wire.

**In firmware:** `RuipuBattery::unlock()` handles this. Must be called non-blocking (millis-based). No `delay()` anywhere.

---

## Connector Wiring Notes

| Wire | Function | Notes |
|---|---|---|
| RED | +36V power | Do not connect to logic |
| BLACK | −36V power | Do not connect to logic |
| GREEN | BMS TX (data out) | Open-collector — requires 1kΩ pull-up to 3.3V |
| BLUE | BMS RX (heartbeat in) | 3.3V logic, driven by ESP32/Arduino TX |
| YELLOW | Signal GND | Not directly shorted to power negative when pack is awake |

### Yellow wire behavior

Yellow reads 0V relative to Black (same potential) but shows OL (open-loop) in continuity mode when the pack is awake. This is caused by BMS protection FET topology — the FET isolates the signal ground from the power rail in resistance while keeping the voltage reference intact. The voltage reading is authoritative; the continuity beep is not.

### Green wire pull-up

ESP32 internal pull-up (~45kΩ) is too weak for the open-collector BMS output. External 1kΩ to 3.3V is mandatory per GREEN wire. Without it, the RX pin floats and reads garbage.

---

## Packet Format

36-byte fixed-length frames. Parsed by `RuipuBattery::read()` using `_buf[36]`. Key fields:

| Field | Method | Notes |
|---|---|---|
| Pack voltage | `voltage()` | Full pack voltage in V |
| State of charge | `soc()` | 0–100% |
| Current | `current()` | `0x2020` at idle — see below |
| Max cell voltage | `high()` | Used for spread calculation |
| Min cell voltage | `low()` | Used for spread calculation |
| Charge cycle count | `chargeCycleCount()` | CYC fingerprint — see Pack_Registry.md |
| Discharge FET | `isDischargeFETEnabled()` | Should be true under load |
| Cell undervoltage | `isCellUndervoltage()` | Flag for any cell below threshold |

### 0x2020 idle current

When no load is connected, the current field contains `0x2020`. This decodes to ~8.224 A in the raw parser — it is a BMS idle-state placeholder, not a real current reading. Treat any current reading of exactly 8.224 A with no active load as an artifact.

---

## Logging via FTDI (bench / diagnostic)

For benchtop packet capture when the ESP32 is not yet built:

- **Option B wiring:** Arduino Nano sends heartbeat on BLUE; FTDI FT232 RX listens on GREEN.
- **PuTTY settings:** COM4 · 9600 baud · 8N1 · No flow control · "All session output" logging in binary mode. `0x20` bytes in the stream are genuine protocol padding, not terminal substitution.
- **PowerShell parsing:** Cast bytes to `[int]` before bit-shifting. `[byte] -shl 8` silently overflows in PowerShell 5.1.

---

## Pack Identification

No UUID in the protocol. Use `chargeCycleCount()` as a fingerprint (`CYC-<count>`). Count is stable across power cycles and unique within a small fleet. See `Pack_Registry.md` for current fleet registry.
