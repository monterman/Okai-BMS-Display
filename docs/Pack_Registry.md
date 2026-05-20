# Pack Registry — Okai 10S4P Fleet

Physical packs identified by charge cycle count (CYC fingerprint). The BMS protocol has no UUID field; `chargeCycleCount()` is stable across power cycles and unique within this fleet.

---

## Active Packs

| Fingerprint | Slot | Voltage | SOC | Cell Spread | Cycles | Health |
|---|---|---|---|---|---|---|
| **CYC-8229** | Pack 1 — GPIO 1 (UART1 RX) | 41.45 V | 100% | 43 mV | 8,229 | Excellent |
| **CYC-8246** | Pack 2 — GPIO 16 (UART2 RX) | 40.36 V | 90% | 66 mV | 8,246 | Good |
| **CYC-8256** | Pack 3 — GPIO 17 (SoftSerial RX) | 39.79 V | 84% | 82 mV | 8,256 | Good |

*Snapshot from field session 2026-05-19. SOC and voltage change with use; fingerprint and slot assignment are permanent.*

---

## Health Thresholds

| Cell Spread | Status |
|---|---|
| < 50 mV | Excellent |
| 50–100 mV | Good |
| 100–150 mV | Marginal |
| > 150 mV | Poor — flag on display |

Firmware uses `pack.high() - pack.low()` (V) × 1000 to get spread in mV. Threshold implemented in `Display.ino`.

---

## Pack 4 Slot

GPIO 18 (SoftSerial RX) reserved for a future fourth pack. Slot shows "—" on the display until a pack is connected.

---

## Fingerprint Identification Procedure

1. Connect pack to any RX pin temporarily.
2. Send heartbeat `0x3A 0x13 0x01 0x16 0x79` every 5s — pack won't transmit without it.
3. Read one packet; call `chargeCycleCount()`.
4. Match the count to this registry. If no match, this is a new pack — add it.
5. Assign to permanent slot and re-wire to its dedicated GPIO.

---

## Background Notes

- All three packs are ex-rental units with 8,200+ cycles — well past typical Li-ion spec (300–1,000 cycles) but all currently healthy with no undervoltage flags.
- Discharge FET enabled on all packs; charge FET state varies with charger presence.
- Cell temps measured 23–26°C during the 2026-05-19 bench session (ambient ~22°C).
- `0x2020` in the current field is a BMS idle-state placeholder (no load connected), not a real 8.224 A reading.
