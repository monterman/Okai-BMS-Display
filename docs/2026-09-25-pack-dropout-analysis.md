# Pack dropouts under load — log analysis, 2026-09-25

**Symptom reported:** three packs connected on ports 1/2/3. System ran well, then died. Waited ~2
minutes, it recovered, then died again — repeatedly. Sometimes the display was on and showed
"No packs connected" with all three plugged in. Packs individually read correctly on the bench
with no load.

**Evidence:** six log files pulled from the device via the WiFi dashboard, `069` through `074`.
`074` is the last file on the device. Raw files kept at
`logs/2026-07-26-dying-packs/` (RTC was stale, so the 2026-07-26 datestamps are wrong; `UpSec`
is the reliable clock, per boot).

---

## 1. What each file is

| File | Ports | Logged span | Peak combined draw | Energy | Ending |
|---|---|---|---|---|---|
| **069** | 1, 2, **4** | **1590 s (26 min)** | 2900 W | **217.5 Wh** | clean, all packs at rest |
| 070 | 1, 2 | 190 s | 433 W | 0.8 Wh | idle, at rest |
| 071 | 1, 2, 3 | 120 s | **3123 W** | 41.1 Wh | **abrupt, mid-load at 30 A** |
| 072 | 1, 2, 3 | 110 s | 1499 W | 4.4 Wh | at rest |
| 073 | 1, 2, 3 | 120 s | **3017 W** | 37.1 Wh | **abrupt, mid-load at 30 A** |
| 074 | 1, 2 | 135 s | 1832 W | 20.2 Wh | **abrupt, P1 vanishes mid-load** |

**`069` is NOT from this session.** It uses **port 4**, and LP2 carries **54 cycles** there against
**66 cycles** in 070–074. Cycle counts only increase, so 069 predates the rest by ~12 cycles of use.
It is the healthy-ride benchmark, not today's data.

**Today's session is 070–074 only: ~11 minutes of logged load, ~104 Wh total.**

---

## 2. The headline comparison

| | 069 (older, healthy) | Today (070–074) |
|---|---|---|
| Continuous logged load | **26 minutes** | longest fragment **3 minutes** |
| Energy delivered | **217.5 Wh** | 104 Wh across five fragments |
| Peak combined draw | 2900 W | 3017–3123 W |
| Lowest cell seen | 3.359 V | **3.099 V** |
| Pack dropouts | **none** | P3 and P1 both vanish |
| Ending | clean, at rest | abrupt, mid-load |

**The older ride did twice the work at the same power without a single dropout.**

---

## 3. What is proven by the logs

**3.1 It is not capacity.** Every abrupt ending happens with SOC at **89–100 %**. The packs were
nearly full each time they died.

**3.2 The display ran in at least two separate boots today.** Files 070 and 074 both contain records
for uptimes 478–488 s with contradictory content — 070 has P1 **at rest, 41.27 V, 0 A**; 074 has P1
**at 32.33 V drawing 30 A**. One logger cannot write two contradictory records for the same uptime.
Treating the five uptime ranges as intervals, the minimum consistent partition is **two boots**
(072 → 071 → 074, and 073 → 070); it could be as many as five. So the system went down and came back
at least once mid-session, which matches the owner's account of waiting ~2 minutes for recovery.

**3.3 The packs stop *talking* before they disappear.** The final frames before each death are stale:
identical lines repeated, and physically impossible readings — P1 logged at **+30.4 A** in the same
sample where P2 reads **−31.6 A** (074, up=482). `Logger.ino` re-writes a pack's last received frame
until `PACK_CONNECTED_MS` (5 s) expires, then drops it. So those readings are *the absence of new
telemetry*, not measurements. A BMS tripping purely on over-current normally keeps transmitting —
this looks more like the pack going to sleep or losing its BMS supply.

**3.4 The packs sag hard.** Internal resistance computed from the heaviest samples is
**~0.21–0.25 Ω per pack**, consistently across both the healthy ride and today. At 30 A that is ~7 V
of sag: **41 V at rest → 32–35 V under load**, cells down to 3.10–3.48 V. For a 10S4P that works out
to roughly 88 mΩ per cell — about 2–3× what healthy high-drain cells should show.

**3.5 LP3 is the outlier pack, but not the only one that drops.** Across 071/072/073 it runs
**5–7 °C hotter** than its neighbours under identical load (26–31 °C vs 19–26 °C) and reaches the
worst cell imbalance seen anywhere in the data (**221 mV** in 071). It carries the most cycles (79).
It is absent from 070 and 074. **However, P1 also vanishes** at the end of 074, so the fault is not
exclusively LP3.

---

## 4. Leading explanation (not yet confirmed)

Under ~3 kW the pack voltage collapses to 32 V. The display is fed from the VESC 5 V rail. If that
rail dips, the display reboots; its keep-alive stops; and **every pack sleeps within 5 seconds**
(`docs/Brief.md` Priority 1 — the keep-alive deadline). That single chain would explain all of it:
everything dying together, "No packs connected" with three plugged in, and the ~2-minute recovery.

Supporting, not conclusive:

- Multiple boots are proven (§3.2).
- Telemetry stops before power does (§3.3) — consistent with sleep, less so with a protection trip.
- All packs die together rather than one at a time.
- The display measures **`vbat = 4.71 V`** even on clean USB with no load — 0.3 V low before
  anything is stressed (live `[DIAG]` capture, 2026-09-25).
- The keep-alive itself is provably healthy on the bench: **2901 beats in 2900 s, worst gap 1000 ms**
  against a 5000 ms deadline. But that counter proves transmission, not reception — the beat goes out
  on one shared wire (GPIO 2 → every pack's red lead), which is a single point of failure for all
  packs at once.

**Competing explanation, still open:** the packs are simply marginal at 3 kW and their own protection
is cutting. 0.22 Ω per pack is high; the 26-minute ride at 2900 W succeeded, so they are marginal
rather than dead.

---

## 5. Tests that discriminate, cheapest first

1. **Two-pack run.** Load the system with **LP1 + LP2 only** (the pair present in every file), same
   route, same power. If it stops dying → LP3 is dragging the bus down. If it still dies → the fault
   is systemic (display supply or shared wiring). No PC needed.
2. **USB capture under load.** Leave the display's USB connected to the PC and load the system. The
   firmware already prints everything needed: `[DIAG] … vbat=X.XX V` every 3 s, `[HB] *** LONG GAP …
   packs at risk of sleeping! ***` if the keep-alive slips, and a boot banner on reset.
   - vbat dips **and** a boot banner appears → display supply is the cause. Fix: dedicated BEC or a
     large bulk capacitor on its 5 V input.
   - packs go silent while vbat holds and no reset occurs → pack-side. Fix: pack-side.
3. **Current limit.** Reduce the VESC battery current limit (e.g. 20 A per pack / 60 A total) to keep
   the sag off the cliff. Five-minute change; lets riding continue while the cause is chased.

---

## 6. Defects found in the firmware while analysing

1. **Stale frames are logged as live data.** When a pack stops transmitting, `Logger.ino` re-writes
   its last received frame for up to 5 s instead of marking it stale or omitting it. This produced the
   impossible +30 A readings and disguises exactly the event being hunted. A `stale` flag or a gap in
   the row would make dropouts unmistakable.
2. **No log entry when a pack drops or rejoins.** The pack set is recorded only in the session header,
   so a mid-session dropout is invisible except by inference. A one-line marker would date every
   dropout precisely.
3. **The RTC is stale** ("restored — re-sync from dashboard"), so every filename and timestamp is
   wrong. `UpSec` carried the analysis. Re-syncing from the dashboard would make future logs
   self-dating.

---

## 7. Open

- Run test 1 or 2 above and record the result here.
- If the display supply is implicated: measure `vbat` under load and size the bulk capacitor.
- LP3's health: highest cycles, hottest, worst imbalance — worth a dedicated capacity test against
  LP1 and LP2, and a note in `Pack_Registry.md`.
