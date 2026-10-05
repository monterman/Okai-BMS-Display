# Pack Registry — Okai 10S4P Fleet

Physical packs identified by charge cycle count (CYC fingerprint). The BMS protocol has no UUID field; `chargeCycleCount()` is stable across power cycles and unique within this fleet.

---

## 🔒 LOCKED PACK IDENTITY — do not ask the owner again

> **Owner instruction, 2026-07-26 (verbatim intent): "you shouldn't be asking me for this pack again ever again."**

| Physical label | Fingerprint | Status | Confirmed |
|---|---|---|---|
| **#1** | **CYC-37** | **ALIVE and accepting charge** — NOT dead | ⭐ owner-confirmed on port 1, 2026-07-26, live BMS readout |
| **#5** | **CYC-78 → 79** | Healthy, stable imbalance | ⭐ owner-confirmed on port 4, 2026-07-26 |

**This supersedes the old "#1 = DEAD / retire, likely CYC-8229" note below — that entry was WRONG.** Pack #1 is functional: observed 2026-07-26 at 41.24 V, SOC 99%, cell Δ 59 mV, 31 °C, drawing +0.16 A on charge. Health flag reads WARN (not FAIL).

**Never re-ask which pack is #1. It is CYC-37. Never re-ask which pack is #5. It is CYC-78/79.**

### 🔒 2026-10-05 — owner's port→label reading, three packs in service

The owner read the printed white labels off the batteries sitting in the ports:

| BMS port | Physical pack label |
|---|---|
| **Port 1** | **#1** |
| **Port 2** | **#4** |
| **Port 3** | **#6** |

**Corrects a provisional note made earlier the same day** which recorded today's port-3 pack as
"#3". It is **#6**. The labels are not contiguous — 1, 4, 6 — so a pack numbered #3 exists and was
simply not in service today.

**This agrees with the locked identity above:** port 1 = **#1** = **CYC-37**, independently
confirmed on port 1 on 2026-07-26. Two readings three months apart, same port, same label.

**Two conflicts it does NOT resolve, left open deliberately:**

- The July line further down reads *"#3 → port 2 (CYC-38)"*, and the locked section reads
  *"#1 = CYC-37"*. **CYC-37 and CYC-38 are one cycle apart — almost certainly the same battery,
  recorded under two different labels.** That is precisely the mis-attribution the ±500 tolerance
  made possible. Do not reconcile these by reasoning; wait for the readout below.
- July had **#4 on port 3**; today #4 is on **port 2**. Packs move between ports, which is the whole
  reason numbers are now bound to the pack and not the port.

**How the label→CYC map gets closed, without asking the owner again.** The one-shot label seed
(`LABEL_SEED_LIST { 1, 4, 6, 0 }`, `Config.h`) writes these numbers into each pack's own registry
record at the first boot after flashing, and prints one line per port as it does:

#### ✅ Readout — first boot of `6d9bb7d`, 2026-10-05 (Mint bench, flashed mid-charge at 87 %)

Verbatim from the serial capture (seconds since port open):

```
[   0.19] [SEED] ARMED v1: port1->#1 port2->#4 port3->#6 port4->#0, window 120 s
[   1.06] [REG] port1 AMBIGUOUS cyc=55 (best +0, runner-up only 0 further) - asking owner
[   1.06] [SEED] port1 not identified - NOT seeding; if the screen asks, answer #1
[   1.61] [REG] port2 registered NEW pack: CYC-45  maxSoc=99%
[   1.94] [SEED] port2 CYC-45 = #4 (owner's printed label)
[   2.15] [REG] port3 AMBIGUOUS cyc=55 (best +0, runner-up only 0 further) - asking owner
[   2.15] [SEED] port3 not identified - NOT seeding; if the screen asks, answer #6
[   2.38] [REG] port1 ambiguity left unresolved by owner
```

| Port | Label | CYC now | Outcome |
|---|---|---|---|
| 1 | **#1** | **55** | ⏳ Not seeded — two existing records both last-seen at 55 (drift +0 / +0). Assign **#1** via the picker |
| 2 | **#4** | **45** | ✅ **#4 = CYC-45** — bound. Registered as a **new** record: no existing record was within 8 cycles |
| 3 | **#6** | **55** | ⏳ Not seeded — same 55/55 collision. Assign **#6** via the picker |

**#1 is now at CYC-55, not 37.** Consistent with the lock above: the counter drifts, +18 cycles
since July. The locked *identity* (#1 is the pack that read CYC-37 in July) stands; only the count moved.

**Open, from this readout:**
- Two packs at an identical cycle count is the case fingerprinting cannot settle. Gate 4 refused both,
  as designed — the old first-match code would have filed one pack's history under the other silently.
- **#4 registering NEW** means either #4 never had a record, or its record sits more than 8 cycles
  from 45. If the latter, #4 now has a split history. Check `/packs` on the dashboard.
- ~~#6 (port 3) did not charge — candidate bad BMS~~ **RETRACTED 2026-10-05: no fault.** The owner
  has only **two chargers**; port 3 had none attached. 0.000 A and a flat 62 % are expected. Status
  `0x02` (bit 1 discharge-FET only) vs `0x2F` (bits 0,1,2,3,5) differs exactly by charge-FET,
  charger-detected, charger-OK and bulk (`OkaiBMS.h:25-30`) — `0x02` is a normal idle pack.
  Still open but **confounded**: port 3 went QUIET (~1.5 s) 10× in 240 s while ports 1–2 never did —
  marginal BMS, or an uncharged pack sleeping more readily. Re-compare only with all ports charging.
  **Do not read a zero-current pack as a fault in single/dual-charger sessions.**

Full capture: `docs/bench-logs/2026-10-05-first-boot-6d9bb7d.log`.

### ⚠️ The CYC fingerprint DRIFTS — match with tolerance, not equality
Pack #5 read **CYC-78** in May and **CYC-79** on 2026-07-27. **The fingerprint is a cycle counter, so it increments every time the pack is charged.** It is unique within the fleet only because the gaps between packs are large (37 · 54 · 65 · 78 · 8229 · 8246 · 8256).

> ### ⚠️ CORRECTION 2026-10 — "the gaps are large" is wrong, and it was load-bearing
> Read those numbers again: the gaps are **17, 11, 13, ... 17, 10** cycles. `PACK_CYC_TOLERANCE`
> was **500** — far wider than the spacing between packs — and `packRegistryIdentify()` accepted
> the **first** record the directory walk returned inside that window, not the closest. A pack
> reading 8246 matched the 8229 record just as readily as its own. **Identity was effectively
> decided by directory order, and lifetime wear history was very likely being filed against the
> wrong battery** — which matters most for the weak pack being watched.
>
> Fixed in the 2026-10 firmware: match on **last-seen** cycles (`currentCycles`, re-anchored on
> every sighting) rather than registration cycles; take the **nearest** candidate; window cut to
> `PACK_CYC_MATCH_WINDOW` = **8**; and when two candidates fall within `PACK_CYC_AMBIGUOUS` = 3 of
> each other the device **refuses to guess** and asks the owner once.

**When decoding a log, match to the NEAREST registry value, never exact** — and expect the number to keep climbing. A pack reading CYC-80 next month is still #5. Re-anchor the registry value on each owner confirmation rather than treating the May snapshot as permanent.

---

## 🔴 FLEET TEST — 2026-09-25: pack #5 RETIRED, every other pack passed

**Test used (owner's, and it works):** one pack at a time on a known-good port, run **VESC motor detection**
from the phone. Detection spins the motor at 10–20 A. A pack whose keep-alive is not reaching it **sleeps and
cuts out above ~4 A**; a healthy pack rides it out. Bench only, no water, repeatable.

| Pack (physical label) | Port in the failing session | Result |
|---|---|---|
| **#5** (CYC-79) | port 3 | ❌ **FAILS — cuts out under load. RETIRED 2026-09-25.** |
| **#2** | port 1 | ✅ passes |
| **#3** | port 2 | ✅ passes |
| **#1** (CYC-37), **#4**, and the two unlabelled spares | — | ✅ all pass |

**What is wrong with #5.** Its keep-alive wire. The owner re-soldered every pack's signal pigtail shortly
before this session; #5's is the one that did not survive. The logs prove the *telemetry* wire is fine —
#5 reported voltage, current and cell data normally in sessions 071/072/073 — so the fault is specific to the
**red / centre wire (BMS RX, the shared keep-alive line)**, not a wholesale TX/RX swap, which would have made
the pack invisible to the display.

**Why one bad pack took the whole system down.** #5 sleeps the moment it is loaded, so the remaining two packs
absorb the entire ~3 kW. They sag harder, approach their own limits, and the session dies. See
[`2026-09-25-pack-dropout-analysis.md`](2026-09-25-pack-dropout-analysis.md).

**Still to determine when #5 is opened for repair — one meter reading decides it:** resistance between the
**red** and **black** pins on its connector, compared against a known-good pack.

- **Open / high, like the others** → the keep-alive simply never reaches #5. It only ever harmed itself, and the
  other packs died from carrying its share of the load.
- **Near zero / much lower than the others** → red is shorted to ground, which drags down the **shared**
  keep-alive bus and starves *every* pack of its heartbeat. That would mean #5 was actively killing the others,
  and the same fault would return the instant it is plugged back in.

Record the answer here when the repair happens.

**Ride configuration until #5 is repaired and re-tested:** use three good packs. Two packs at 3 kW sag hard
enough to be marginal on their own — that is what the load data shows, independent of the #5 fault.

---

## ⭐ REPORTING RULE + SESSION PORT MAP (owner instruction, 2026-07-24)

> **RULE (owner):** logs identify packs by **CYC count** (each port broadcasts it) — use CYC **only** to decode which pack is which, then **ALWAYS report results by the owner's LABEL number (#N), never by CYC.** The owner cares about the label number, not the cycle count. Do the CYC→label mapping silently.

**Dock setup for the 2026-07-24 session — BMS port ↔ physical label ↔ fingerprint (decode key):**

| BMS Port | Physical label | Fingerprint (log decode only) | Notes |
|---|---|---|---|
| **Port 1** | **#4** | CYC-54 | recovering-imbalance pack; owner: "in quarantine but actually good now" |
| **Port 2** | **#2** | CYC-38 | ⭐ owner-confirmed 2026-07-24 (double-checked physically) — CORRECTS the old provisional registry swap; **#2 = 38 cyc, authoritative** |
| **Port 3** | **#3** | CYC-65 | ⭐ owner-confirmed 2026-07-24 — **#3 = 65 cyc, authoritative** (supersedes the earlier "#3=CYC-38" note, which was flagged uncertain) |
| **Port 4** | **#5** | CYC-78 | near-new BUT owner reports it is **also imbalanced, ~like #4** → add to rebalance watch |

- 4 packs paralleled on the dock; voltages **40.1 / 40.2 / 40.3 V** (~0.2 V spread) → minimal inrush, connected fine. Owner confirmed it worked.
- **Registry update:** **#5 (CYC-78)** field state is **imbalanced** (owner-observed), not the earlier bench "Excellent" — treat as needs-rebalance, similar to #4.
- **When reading THIS session's RIDE/CHARGE logs:** map port→label per the table above and report every per-pack result by **#N label only**.

---

## Active Packs

| Fingerprint | Slot | Voltage | SOC | Cell Spread | Cycles | maxSoc | Health |
|---|---|---|---|---|---|---|---|
| **CYC-37** ⭐ | **Pack #1** (LOCKED — see top) | **41.2 V rested** | full | **65–67 mV at rest** | 37 | — | Good — ⚠️ **watch the trend**; 59→79 mV on charge, relaxed to 65 |
| **CYC-8229** | ~~Pack 1~~ — GPIO 1 (UART1 RX) | 41.45 V | 100% | 43 mV | 8,229 | — | Excellent — ⚠️ **NOT pack #1**; label unassigned |
| **CYC-8246** | Pack 2 — GPIO 16 (UART2 RX) | 40.36 V | 90% | 66 mV | 8,246 | — | Good |
| **CYC-8256** | Pack 3 — GPIO 17 (SoftSerial RX) | 39.79 V | 84% | 82 mV | 8,256 | — | Good |
| **CYC-54** | Pack 4 — GPIO 18 (SoftSerial RX) | 40.66 V | 93% | 64 mV | 54 | 99% | Excellent — near new |
| **CYC-78 → 79** ⭐ | **Pack #5** (LOCKED — see top) | 41.32 V | 77% | **72 mV** | 79 | 100% | Good — **spread stable** 73→72 mV since May |
| **CYC-65** | **Pack #3** — rest state 2026-07-26 | **41.2 V** | full | **49 mV** | 65 | — | 🟢 Best of the fleet at rest |

*CYC-8229/8246/8256 snapshot: 2026-05-19. CYC-54/CYC-78 snapshot: 2026-05-20.*
*SOC and voltage change with use; fingerprint and slot assignment are permanent.*

> **maxSoc** (byte [6]): max achievable SOC as % of design — SoH indicator decoded 2026-05-20. Near-new packs read 99–100%; field data not yet captured for ex-rental packs.

---

## Field Data — 2026-07-18 Ride (RIDE logs 039–042)

First real on-water load data, decoded from the OkaiBMS RIDE CSVs. Keyed by **CYC fingerprint** (the SSOT). Cell spread here is a *distribution* (rest vs under-load), not the single bench reading above.

| Fingerprint | Ride slot | Cycles | Rest spread | Load spread (med / max) | DELTA warns | Max current | Field health |
|---|---|---|---|---|---|---|---|
| **CYC-38** | port 2 | 38 | **31 mV** | 55 / 276 mV | WARN 111, POOR 24 / 513 | 32.4 A | 🟢 **Best** — youngest, tightest at rest |
| **CYC-65** | port 1 | 65 | **35 mV** | 48 / 215 mV | WARN 79, POOR 22 / 512 | 32.7 A | 🟢 Healthy workhorse |
| **CYC-54** | port 3 | 54 | **85 mV** | 147 / 205 mV | WARN 41, POOR 4 / 45 (~91% flagged) | 30.2 A | 🟠 **Watch** — imbalanced even at rest |

- **Rest spread** (current < 1 A) is the cleanest SoH signal. Sort the fleet by it: **< 40 mV keep, > 70 mV bench/rebalance.**
- **Load spreads are inflated by an overload condition**, not pack defects alone: the ride ran **only 2 packs** (CYC-65 + CYC-38), so each had to sink **20–27 A** and cells sagged to ~3.4–3.5 V. Splitting the same draw across **3 packs ≈ 13–17 A each, 4 ≈ 10–13 A** — the fix. **Minimum 3 packs, 4 preferred.**
- **CYC-54 changed:** bench 2026-05-20 read 64 mV "Excellent — near new"; field 2026-07-18 reads **85 mV rest + DELTA_WARN on ~91% of samples** (the "orange warning" the rider saw on port 3). Give it a **slow full charge to 100% and let the BMS balance**, then re-measure rest spread. If it stays > 70 mV, restrict to low-current use.

---

## ⚡ Charging diagnosis — 2026-07-26

### "The pack refuses all charge" — check SOC before suspecting a fault
**Symptom:** bench supply set to 42 V, correct voltage measured at the pack terminals, but **current drops to 0 A instantly** at 40 V, 41.5 V and 42 V alike.

**Cause: the pack was simply FULL.** Pack **#3** was at full charge with cells balanced to **49 mV** — healthy. The BMS opened the charge FET because there was nothing to accept. **Not a fault, not a dead pack.**

> **Diagnostic order for next time:** ① measure at the *pack terminals* (a voltmeter reads the supply voltage even through a broken lead — that reading alone proves nothing) → ② if the supply voltage IS present at the pack, the BMS is deliberately blocking → ③ **read SOC and cell spread on the WiFi dashboard before assuming a fault.** Full and balanced = working as designed.

### The live dashboard is the tool for this
`192.168.4.1` (`WiFiServer.ino:41`) already serves a **live pack table auto-refreshing every 5 s**: SOC · Voltage · **Current** · Power · Avail Wh · **CellΔ** · Temp · CYC ID · Health, plus a ⚡ CHARGE / 🚶 RIDE / IDLE mode flag. Everything needed to diagnose a charge refusal, with no firmware change.
*(The device screen 2 "Charging Live" shows total **watts** and per-pack ETA but **not amps** — the data exists in `packs[i].current`, just isn't printed. Optional small addition.)*

### Balancing behaviour observed on pack #1 (CYC-37)
Supply set 42 V → pack sits at **41.24 V** drawing **0.15–0.16 A**, fluctuating. That gap is far too large to be wiring drop at 0.16 A — **the BMS is regulating while the passive balancer bleeds the high cells.** Trickle-in, bleed-off: that is what balancing looks like, and it is the correct behaviour.

**Realistic expectation:** passive balancers bleed at tens of milliamps. Starting spread **59 mV** is already "Good" by the thresholds below, so expect a **modest** improvement over a few hours — not a dramatic one. Re-read CellΔ on the dashboard to measure it rather than assuming.

---

## 🔋 REST-STATE BASELINE — the honest health metric (2026-07-26)

> **Measure spread AT REST, not while charging.** Charging inflates it; resting reveals it.

### 41.2 V resting is a FULL pack — do not read it as a shortfall

Owner voltmeter reading, both packs, after ~12–16 h off charge:

| Pack | Rest voltage | **Per cell** | **Rest spread** | Cycles | Read |
|---|---|---|---|---|---|
| **#1** (CYC-37) | **41.2 V** | 4.12 V | **65–67 mV** | 37 | Good — near the 70 mV watch line |
| **#3** (CYC-65) | **41.2 V** | 4.12 V | **49 mV** | 65 | Excellent-ish |

**4.12 V/cell is textbook post-charge relaxation from 4.20** (cells shed 50–100 mV each over hours) ≈ **90–93% SOC**. It is also **healthier than parking at 42.0 V** — time at the very top is what ages lithium.

> **Do not chase 42.0 V resting. 41.2 V is the correct, expected end state.** The stock dock charger produces it too.

**Matched rest voltage matters operationally:** these packs get paralleled. Both sitting at 41.2 V ⇒ near-zero inrush on connection — same reason the 2026-07-24 dock (40.1 / 40.2 / 40.3 V) connected cleanly.

**Same voltage ≠ same health.** #1 and #3 are both at 41.2 V; the difference between them is entirely internal matching (65 vs 49 mV).

### Relaxation is NOT balancing — do not confuse them
Pack #1 read **79 mV** during trickle charge, then **65–67 mV** after a night disconnected. That drop is **cells settling, not the balancer working** — the charger was off, so nothing was bleeding. Removing current drops the surface-charge/IR inflation and different cells relax by different amounts.

**Consequence: the 2 h trickle session on pack #1 achieved essentially nothing.** Spread never narrowed while current was actually flowing (137 → 29 mA); it only widened, then relaxed once charging stopped.

Two possibilities, not separable in one night:
1. **Passive balancing is simply very slow** — tens of mA needs *many* hours **at the top of charge**, not two
2. **The cell is genuinely down** and won't recover

**The discriminator is trend, not a single session:** track whether pack #1's rest spread creeps upward over the next few charge cycles. **Stable = a characteristic to live with. Climbing = degradation → retire from rotation.**

### Stability beats absolute value
| Pack | May 2026 bench | 2026-07-26 | Verdict |
|---|---|---|---|
| **#5** (CYC-78→79) | 73 mV | **72 mV** | **Unchanged over 2 months — stable, not progressive** |

A spread that sits still is a **characteristic**; one that climbs is **degradation**. Log the trend, not just the number.

⚠️ **Cycle-count context:** #1 (37) and #3 (65) are **barely-used packs**, not the 8,200-cycle ex-rentals. A 65 mV spread at 37 cycles is unremarkable but slightly high for one so young — likely 4P-group manufacturing variation. Not a problem today; it is the reason to watch the trend.

---

## ⚡ BENCH SUPPLY BEHAVIOUR — how to actually push charge (2026-07-26)

### 🔑 The current knob is a CEILING, not a target
Repeatedly confusing in the field: raising the supply's current limit from 0.5 A to 1 A **does nothing**. In constant-voltage mode:

```
current = (supply volts − pack volts) ÷ circuit resistance
```

The current dial only **caps** what the pack chooses to draw. If the pack takes 44 mA, that is what flows whether the dial reads 0.5 A or 5 A.

> **To move more current, raise the VOLTAGE — never above 42.0 V for a 10S pack.**

### A large volts gap with tiny current means something is limiting
Pack #5 observed at **41.32 V drawing 0.044 A** against a supply pushing toward 42 V — a 0.68 V gap yielding 44 mA implies **~15 Ω** somewhere. Far too high for a healthy battery circuit. Three candidates, in diagnostic order:

1. **Poor connection** (lead / connector / clip) — **rule this out first**: measure at the *pack terminals* while current flows and compare to the supply display. A big gap = your leads, not the battery.
2. **BMS deliberately limiting** (taper / balance mode)
3. **Balancer absorbing it** — bleed resistors are a real load; this would be *good* news

### ⚠️ Trickle-only charging from a low SOC is a day-long test
Filling the top **10–12% (~1.4 Ah of 12.8 Ah)** at **0.05 A takes ~28–30 h** — before balancing even starts, since passive balancers typically don't engage until **~4.10–4.15 V/cell**.

> **Correct test design — CC then CV:** charge at **1.5–2 A to ~41.5–41.8 V (≈1 h)**, *then* drop to **0.05 A** and dwell. Spend the waiting time **in the region where the balancer works**, not travelling to it.

### 🔄 IN PROGRESS — pack #5 on the stock dock charger
Moved off the variable supply 2026-07-26. Start state: **CYC-79, 77% SOC, 72 mV, ~41.32 V**. The stock charger delivers bulk current fast *and* is the unit that reliably lands packs at 41.2 V rested — so it tests balancing without the 30 h wait.

**When the CHARGE log lands, extract:**
- **Cell spread vs time**, plotted against current and SOC
- **Whether spread ever narrows while current is flowing** ← the actual question
- Where in the SOC range balancing engages
- Rest spread afterwards vs the **72–73 mV** baseline

✅ **Firmware flashed 2026-07-26** (hash verified, board confirmed by MAC `a0:f2:62:e1:e9:ec`). Pack #5 seen on port 4 at 41.24 V / 99% immediately after boot.

### 📋 OPEN — verification checklist, not yet done
Nobody has confirmed these on the device yet. **Raise them at the start of the next session.**

- [x] ✅ **Health reads GOOD** — owner confirmed 2026-07-27, device showing **75 mV → GOOD**. Under the old 50/100 mV pair that same pack read WARN. Rest-gate + new thresholds verified on hardware.
- [ ] **`Bulk charging` → `Balancing` → `Charge complete`** appear in sequence with the cheap charger
- [ ] **Open `192.168.4.1` once** to re-sync the clock — until then log headers read `restored (stale)`
- [ ] **First `Okai_CHRG_*` file shows real kilobytes** within ~2 min of connecting the charger (proves the crash-safe checkpoint)
- [ ] **Current sign on the charger** — owner reported **−1 A**; on a charger it should be **positive**. If it really is negative, the Balancing/Complete logic is reading the wrong direction and `kBalanceCurrentA` needs revisiting.

~~**Follow-up worth running:** stock charger vs bench supply, same pack, same measurement.~~
❌ **Withdrawn 2026-07-27.** That comparison existed only to validate the bench supply. The supply is being returned (see below), so it has no value.

---

## 🔌 Bench power supply — RETURNED, and why that is safe

**Decision 2026-07-27: the variable supply is not needed.** Recorded so nobody re-derives it.

| Job | Covered by | Verdict |
|---|---|---|
| Routine charging | 3 cheap scooter chargers, 9 packs, 3 per session | ✅ fully covered |
| Landing packs at a healthy rest voltage | Cheap charger reaches **41.2 V rested** unaided | ✅ correct end state |
| Balancing | Cheap charger's green-light taper holds cells at the top | ✅ that *is* the balancing window |
| Fast charging | Not wanted — 9 packs, no rush | n/a |
| **Rebalancing a drifted pack** | **Neither tool.** Bench supply achieved **nothing** in 2 h on pack #1 | ❌ supply is not better |

**The one job it does uniquely:** waking a pack that has self-discharged below the BMS undervoltage lockout, where a dumb charger will not start at all. **That is rare, not guaranteed to work** (many BMS lock out until reset or need direct cell-group charging), and with 9 packs the loss of one is survivable.

> **Nothing in the current test plan requires it.** The pending work is a CHARGE log from the *cheap* charger.

---

### ⚠️ Label reconciliation needed (physical #N ↔ CYC fingerprint)
Rider's physical labels for the 2026-07-18 ride: **#2 → port 1 (CYC-65)**, **#3 → port 2 (CYC-38)**, **#4 → port 3 (CYC-54)**.
- **#4 = CYC-54 = registry "Pack 4"** — consistent. ✓
- **#2 (CYC-65) and #3 (CYC-38) are NEW fingerprints** — they are *not* the ex-rental CYC-8246/CYC-8256 that the May "Pack 2 / Pack 3" rows point to. Physical labels #2/#3 appear to have been reassigned to newer packs, or the numbering diverged from the May registry.
- ~~**#1 = DEAD / retire**~~ — ❌ **WRONG, superseded.** #1 = **CYC-37**, alive and charging. See the LOCKED table at the top.
- ~~**#5 = untested**~~ — ❌ **superseded.** #5 = **CYC-78/79**, owner-confirmed 2026-07-26, bench + charge data recorded above.

**Action for rider:** read the CYC count off each physical pack (heartbeat + `chargeCycleCount()`, see procedure below) and confirm the #N ↔ CYC map so this registry has one number per pack. Until then, CYC fingerprints are authoritative; physical #N labels are provisional.

---

## Health Thresholds

> **Revised 2026-07-26, owner-approved. Spread is judged AT REST ONLY.**

| Cell Spread **at rest** | Firmware verdict |
|---|---|
| < 100 mV | **GOOD** |
| 100–180 mV | **WARN** |
| > 180 mV | **POOR** — flagged on display |

### ⭐ The gate matters more than the numbers
Previously 50/100 mV, evaluated on **every** sample including under load. That was wrong twice over:

1. **Cell spread inflates 2–5× while current flows** — it measures internal-resistance differences, not state of health
2. **It inflates again near full charge** as the cell voltage curve steepens

**Measured proof — pack #3, the healthiest in the fleet:**

| Moment | Current | Δ | Old verdict |
|---|---|---|---|
| Under load | −9.70 A | 160 mV | 🔴 DELTA_POOR |
| 30 s later | −0.07 A | **45 mV** | fine |

**Raising the limit alone would NOT have fixed this** — 160 mV trips 100 mV too. **The gate on current is the actual fix.**

Firmware holds the last at-rest spread and reuses it under load, so the verdict stays stable through a whole ride instead of flickering on every throttle punch.

**Single source of truth:** `Config.h` → `CELL_DELTA_WARN_V` / `CELL_DELTA_POOR_V` / `CELL_REST_CURRENT_A`, consumed via `healthDelta()` and `healthTag()`. Screen, LEDs, web dashboard and CSV all call those helpers — they cannot disagree.

**Calibration context:** healthy Li-ion rests at 10–30 mV · normal service life reaches ~50 mV · commercial BMS alarm points sit at 100–200 mV · a genuinely bad cell shows >200 mV **at rest**, or a spread that **grows every cycle**. Trend beats absolute value.

---

## Log files — naming and schema (firmware 2026-07-26)

```
/Okai_RIDE_20260726_057_1.csv
/Okai_CHRG_20260726_012_1.csv
```

CSV gained a **`Rest`** column (1/0). `Delta_mV` is still written on every row — raw data is never discarded — but **only `Rest=1` rows carry health meaning.** Filter on it rather than inferring from `Current_A`.

⚠️ **Timestamps in logs written before 2026-07-26 are worthless.** `timeNowSec()` restored a stale `millis()` anchor from flash, so elapsed time computed negative and clamped to zero — the clock froze at the last browser sync. Every row of the 2026-07-25 logs reads `2026-07-25T03:00:01`, across three separate sessions. **Use `UpSec` for anything older than this firmware.**

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
