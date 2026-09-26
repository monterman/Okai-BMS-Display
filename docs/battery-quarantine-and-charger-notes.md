# Battery Quarantine & Charger Notes (Okai packs)

## Charger decision — Wanptek 60V 5A (bench supply) — BUYING 2026-07-24
- **Product:** Wanptek 60V / 5A adjustable bench DC power supply. **~$62 + tax, Amazon** (under $70).
- **Why this (not an RC charger):** the Okai packs are **10S = 42 V**. Every affordable RC/hobby "smart charger" (Hitec RDX2, ToolkitRC M8AC, ISDT K4) is **capped at 6–8S (25–33.6 V)** and physically can't reach 42 V. A 0–60 V bench supply clears 42 V with headroom (also covers a future 12S / 50.4 V). The only smart-charger route to 10–12S is an iCharger X12/DX12 (DC-only + separate PSU = ~$350–400 all-in — overkill).
- **Rejected:** sub-$50 no-name 0–30 V supplies (can't reach 42 V); the whole 6S RC-charger category.

### Setup for holding a pack at the top so its BMS passively rebalances
1. Set **CV = 42.0 V** (10S × 4.2 V — **hard ceiling, never exceed**). Gentler hold: **41.6–41.8 V**.
2. **Current limit low (~0.5–1.5 A).**
3. Connect to the pack's **main + / −** (through the BMS), correct polarity.
4. Leave it: pack draws hard, then tapers to ~0 as it tops out; the **BMS bleeds the high cells** over hours. Watch `dV` fall.
- Advantage over the dumb $14 scooter brick: the bench supply **holds CV continuously** instead of terminating (amber→green), so the balancer never loses the top-of-charge window.

## Pack quarantine log
- **Pack CYC-54 (slot P3) — in quarantine for rebalancing (started ~2026-07-22).**
  - 2026-07-23/24: 98 % SOC, 41.09 V, 23 °C, **dV = 81 mV → WARN** (cell imbalance), CYC-54 = **young pack (~54 cycles)**.
  - Assessment: 81 mV on a 54-cycle pack is almost certainly **recoverable storage drift**, not cell wear. Prognosis good.
  - Target: hold at top of charge → dV trends **under 50 mV (GOOD)**, ideally < 30 mV and holding = done.
  - Method so far: $14 brick + daily unplug/replug (works, slow). Wanptek 60V incoming for a continuous CV hold.
  - **Track:** read `dV` off the Okai display (Screen 1 or 3) daily; expect it to creep down on a young pack. Plateau at ~80 mV for days = a weak cell (then stop).

**Cross-links:** Okai BMS warning thresholds — GOOD < 50 mV · WARN 50–100 mV · POOR > 100 mV (`Config.h`: CELL_DELTA_WARN_V/POOR_V). Logs pulled via WiFi dashboard (`http://192.168.4.1`, `/csv?f=…`).
