# Display Screens — Okai BMS Display

Hardware: LILYGO T-Display-S3 · 320×170 px ST7789 TFT · landscape orientation  
Navigation: **BTN2** = next screen →  **BTN3** = prev screen ←  **BTN1** = action (screen-dependent)  
Header bar (always visible): firmware version · WiFi + log mode · uptime clock

---

## Screen 0 — Fleet Overview

Primary riding screen. Shows all connected packs at a glance.

```
┌─────────────────────────────────────────────────────────────┐
│ OKAI BMS 0.2.0        WiFi:OFF [R]              00:12:34    │  ← header
├───────────────────────┬─────────────────────────────────────┤
│ P1              GOOD  │ P2              WARN                 │
│       87%             │       90%                           │  ← SOC large
│ 41.1V  -3.2A          │ 40.4V  -3.1A                        │
│ d43mV  24°C           │ d66mV  25°C                         │
│ ~401 Wh avail         │ ~415 Wh avail                       │  ← riding mode
├───────────────────────┴─────────────────────────────────────┤
│ P3              GOOD  │ P4              GOOD                 │
│       84%             │       93%                           │
│ 39.8V  -3.0A          │ 40.7V  -3.0A                        │
│ d82mV  23°C           │ d64mV  24°C                         │
│ ~387 Wh avail         │ ~428 Wh avail                       │
├─────────────────────────────────────────────────────────────┤
│ ~47 min  4P: 1631Wh / 45.3Ah             ● ○ ○ ○           │  ← bottom strip
└─────────────────────────────────────────────────────────────┘
```

### Per-pack cell content

| Row | Content | Color |
|-----|---------|-------|
| Top-left | Port number (P1–P4) | dim |
| Top-right | GOOD / WARN / POOR health tag | green / yellow / red |
| Large center | SOC % | health color |
| Line 1 | Voltage (V) · Current (A, signed) | accent |
| Line 2 | Cell delta (mV) · Max temp (°C) | text |
| Line 3 | **Riding:** `~XXX Wh avail` · **Idle/Charging:** `CYC-XXXX` fingerprint | accent |

Cell border color matches health: green < 50 mV · yellow 50–100 mV · red > 100 mV

### Bottom strip (RIDE mode only)

Shows fleet totals across all connected packs:

- EMA warmed up (≥ 3 samples): `~47 min  3P: 1203Wh / 46.1Ah`
- EMA warming up: `3P fleet: 1203 Wh / 46.1 Ah`

Page-indicator dots shift right to make room for the text.

### Adaptive empty-cell summary

When fewer than 4 packs are connected, empty cell slots display fleet summary instead of "no data":

**2 packs in the same row (e.g. P1+P2), other row completely free → wide panel:**

```
├───────────────────────────────────────────────────────────────┤
│ ~47 min              │ 2P capacity:                           │
│                      │ 1,203 Wh left                         │
│ @ 245 W avg          │ 33,408 mAh left                       │
│ (12 samples)         │ Design: 922Wh/25,600mAh               │
│                      │ avg SoC: 88%                          │
└──────────────────────┴────────────────────────────────────────┘
```

**1 empty slot (3 packs connected) → narrow panel in that cell:**

```
│ ~47 min              │
│ 1,203 Wh left        │
│ 33,408 mAh           │
│ @ 245W avg           │
```

**Not riding:** panels show `Fleet info` / `start riding for estimate`  
**EMA warming up:** panels show `~-- min` until ~30 seconds of ride data collected

### BTN1 action on Screen 0
Toggles the WiFi access point on/off.

### BTN3 long press on Screen 0
Opens the **label picker** for Port 1 — assigns a numeric label (1–8) to track which physical pack is in which slot. The label appears in CSV filenames and the web dashboard.

---

## Screen 1 — Per-Pack Detail

Full telemetry for one pack. Cycle through packs with **BTN1**.

```
┌─────────────────────────────────────────────────────────────┐
│ OKAI BMS 0.2.0        WiFi:OFF                  00:12:34    │
│ P2  ● ○ ○ ○                                        WARN     │
│                                                             │
│                        90%                                  │  ← SOC large
│                                                             │
│ +0W              415 Wh avail                               │
│ 40.36V                             -0.00A                   │
│ d66mV                              25°C                     │
│ CYC-8246                           SoH 82% vs new           │
│ Sess: +0.0Wh / -124.3Wh                                     │
│                                                             │  ← page dots
└─────────────────────────────────────────────────────────────┘
```

| Field | Description |
|-------|-------------|
| SOC % | Large, health-color coded |
| Instant power (W) | Positive = charging (cyan), negative = discharging (accent) |
| Avail Wh | `soc% × 460.8 Wh` design capacity |
| Voltage / Current | Two decimal places each |
| Cell delta (mV) | `cellHigh − cellLow × 1000` |
| Max temp (°C) | Highest of cell, FET, MCU sensors |
| CYC fingerprint | Permanent pack UUID from charge cycle count at registration |
| SoH % vs new | Heuristic: `100 − (delta_mV × 0.3 + cycles × 0.02)`, capped at 0% |
| Session energy | Wh accumulated this session (charge / discharge) |
| Charger status | Shows `Charging...` (cyan) or `Charge complete` (green) when charger detected |

**BTN1:** cycles active pack P1 → P2 → P3 → P4 → P1

---

## Screen 2 — Context-Sensitive (Ride Energy / Charging Live)

This screen adapts to the current log mode.

---

### Screen 2 in RIDE mode — Fleet Ride Energy

```
┌─────────────────────────────────────────────────────────────┐
│ OKAI BMS 0.2.0        WiFi:OFF [R]              00:12:34    │
│ FLEET ENERGY (RIDING)                                       │
│ ~47 min left                                                │  ← textSize 2
│ @ 245 W avg (12 samples)                                    │
│ Pack  SoC    Wh avail    mAh avail                          │
│ P1    87%     401 Wh    11136 mAh                           │
│ P2    90%     415 Wh    11520 mAh                           │
│ P3    ---      ---         ---                              │
│ P4    ---      ---         ---                              │
│ Fleet (2P): 816 Wh  /  22656 mAh                           │  ← green
│ Design(2P): 922 Wh  /  25600 mAh                           │  ← dim
│                                               ○ ● ○ ○      │
└─────────────────────────────────────────────────────────────┘
```

| Element | Description |
|---------|-------------|
| `~47 min left` | `(total available Wh ÷ avg power W) × 60`. Green when EMA ready, dim while warming up |
| Avg power note | EMA of fleet discharge watts, 10% alpha (~30 s warmup). Shows sample count |
| Per-pack rows | Only connected packs show Wh/mAh; disconnected show `---` |
| Fleet total | Sum across all connected packs |
| Design reference | `N × 460.8 Wh` / `N × 12,800 mAh` — full-charge baseline for comparison |

The EMA resets at the start of each new ride session so stale data from a previous session never influences the current estimate.

---

### Screen 2 in CHARGE mode — Charging Live

```
┌─────────────────────────────────────────────────────────────┐
│ OKAI BMS 0.2.0        WiFi:OFF [C]              00:45:11    │
│ P1  87% ████████████████████████████████▌░░░░░░  1h13m     │
│ P2  90% ███████████████████████████████████░░░░  DONE       │
│ P3  ---                                                     │
│ P4  ---                                                     │
│                                                             │
│ Total: 142W  Packs done: 1/4                                │
│                                               ○ ● ○ ○      │
└─────────────────────────────────────────────────────────────┘
```

| Element | Description |
|---------|-------------|
| SOC bar | Filled proportion = SoC%. Animated blinking edge while bulk-charging |
| ETA | `(100% − SoC) × 12.8 Ah ÷ charger current × 60 min`. Hidden when charger not in bulk phase |
| DONE | Shown when charger detected, not in bulk phase, and SoC = 100% |
| Total W | Sum of charge power across all charging packs |
| Packs done | Count of completed charges |

A 1.5-second white border flash fires whenever any pack transitions to charge-complete.

---

## Screen 3 — Cell Health Table

Long-term health snapshot across all packs. Reference: Panasonic NCR18650BD 10S4P.

```
┌─────────────────────────────────────────────────────────────┐
│ OKAI BMS 0.2.0        WiFi:OFF                  00:12:34    │
│ CELL HEALTH  (vs NCR18650BD new)                            │
│ Pack  dV-mV  CYC#  SoH  Avail.Wh  Status                   │
│ P1      43  8229   79%   401Wh    GOOD                      │
│ P2      66  8246   77%   415Wh    WARN                      │
│ P3      82  8256   76%   387Wh    WARN                      │
│ P4      64    54   98%   428Wh    GOOD                      │
│                                                             │
│ P3 worst: 76% SoH, ~36 cyc remaining                       │
│ New BD design: 460.8Wh  Per-cell V: N/A (internal)         │
│                                               ○ ○ ○ ●      │
└─────────────────────────────────────────────────────────────┘
```

| Column | Description |
|--------|-------------|
| dV-mV | Cell spread in mV (`cellHigh − cellLow × 1000`) |
| CYC# | Charge cycle count reported by BMS |
| SoH | Heuristic state-of-health vs brand-new cell. ≥ 80% green · ≥ 60% yellow · < 60% red |
| Avail.Wh | `soc% × 460.8 Wh` |
| Status | GOOD < 50 mV · WARN 50–100 mV · POOR > 100 mV |

The worst-pack line only appears when the lowest SoH is below 90%.

---

## Overlays

### Disconnect modal

Appears 2 minutes after a pack stops responding (debounced to filter brief glitches and short riding stops):

```
┌────────────────────────────────────────────────────────┐
│  Port 3 (pack L5) disconnected                         │
│  Inserting a different pack?                           │
│  BTN1: Yes → assign new label                          │
│  BTN2/BTN3: Dismiss                                    │
└────────────────────────────────────────────────────────┘
```

### Label picker

Triggered by BTN1 on the disconnect modal, or BTN3 long-press on Screen 0:

```
┌──────────────────────────────────────────────┐
│  Port 1 → assign pack label:                 │
│                   3                          │  ← large digit, BTN1 cycles
│  BTN1:cycle  BTN2:confirm  BTN3:cancel       │
└──────────────────────────────────────────────┘
```

Labels 1–8. Label 0 = unassigned (shows as P1/P2/P3/P4 in filenames).

---

## Header Bar

Always drawn at the top of every screen (16 px tall):

```
OKAI BMS 0.2.0        WiFi:ON [R]              00:42:17
│                     │       │                │
version               WiFi    log mode          uptime
```

Log mode tag: `[R]` = RIDE logging active · `[C]` = CHARGE logging active · blank = idle

---

## WiFi Web Dashboard

Activate with **BTN1** on Screen 0. Connect to AP `OkaiBMS` / `12345678` then open `http://192.168.4.1`

| Route | Description |
|-------|-------------|
| `/` | Live pack table (auto-refreshes 5 s) + log file list + RTC auto-sync |
| `/packs` | Pack registry: CYC ID, first seen, lifetime Wh charged/discharged, sessions, SoH history |
| `/rawdump` | Live 36-byte hex frame dump for all ports (refreshes 3 s) |
| `/csv?f=/NAME.csv` | Download a log file |
| `/delete?f=/NAME.csv` | Delete a log file |
| `/clearall` | Delete all log files |
| `/settime?t=N` | Set RTC from browser epoch ms (called automatically by dashboard JS) |
