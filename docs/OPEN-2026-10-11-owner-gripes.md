# Open defects — owner review of 0.3.6, 2026-10-11

Raised by the owner after 0.3.6 was flashed and verified on `a0:f2:62:e1:e9:ec`.
**Nothing here is fixed yet. Nothing has been flashed since 0.3.6-d43d4e2.**
His words are quoted verbatim because every one of these is a real defect and the
wording is the spec.

---

## 1. WiFi label colour contradicts the dot — MY ERROR, fix is known

> "Wi-Fi is grayed out but the dot is green. What the hell does that mean? To me it means
> that Wi-Fi is connected to the local network but the Wi-Fi is off. It's impossible.
> If the Wi-Fi is gray it has to be off. If it's green it's on and then we add the dot."

**Cause.** In 0.3.5 I made the `WiFi` letters `C_DIM` unconditionally. I posed the choice
to him as "neutral grey always vs green when on", recommended neutral, and the
recommendation was wrong: grey letters read as OFF, so grey + green dot asserts
"off and connected" simultaneously. He had already given the correct rule on 2026-10-09
and I overrode it with a question I should not have asked.

**Fix.** `Display.ino` `drawHeader()` — restore the state colour on the text and keep the
dot:

```c
_gfx->setTextColor(wifiActive ? C_GOOD : C_DIM);   // grey = radio OFF, green = radio ON
_gfx->print(kWifiLabel);
```

Dot stays as specified 2026-10-09: green = something connected (joined network OR AP with
a client), red = radio on and nothing connected, absent = radio off. With the letters
state-coloured there is no contradiction left: grey+no dot = off, green+green = connected,
green+red = on but not connected.

**He also offered an alternative** that is one line away if he prefers it later: dot only
ever appears RED (on but not connected) and its absence means connected.

---

## 2. BTN2 red persists — he does NOT accept "hardware", and he has a point

> "I still have button 2 red flashing. That doesn't make sense at all. Is the button stuck
> or not? I restarted it and it appeared again... I don't see a short in the button and the
> screen is reacting. It has to be something with the firmware that is messing up the button."

### ✅ RESOLVED 2026-10-11 by instrumentation — IT IS HARDWARE

0.3.7 added a raw pad dump every 3 s. First boot after flashing:

```
[BTN] raw b1=1 b2=0 b3=1  stk=000  rst=11      (t=3s, 6s, 9s, 12s, 15s...)
[BTN] BTN2 STUCK LOW for 15 s - ignoring it
[BTN] raw b1=1 b2=0 b3=1  stk=010  rst=11
```

**Conclusive, and the controls are what make it conclusive:**

- `b2=0` — GPIO14 is LOW at the pad, continuously, from boot.
- `b1=1` and `b3=1` — BTN1 and BTN3 read HIGH through the **identical** code path, the same
  `pinMode(INPUT_PULLUP)` block and the same sampling function. So the pull-up is working
  and the reader is working. Only pin 14 is low.
- `rst=11` = `ESP_RST_USB`. **Not** `8` (`ESP_RST_DEEPSLEEP`), so the pad-hold latch fixed
  in this build is NOT the cause here. That bug is real and worth having fixed, but it is
  not this.

GPIO14 is being pulled low by something outside the chip. The firmware is behaving
correctly: it detects the condition at 15 s and refuses the button rather than letting one
faulty pin break three functions, which is what happened in the 2026-10-06 incident.

**What to look for on the hardware** — the switch can fail closed internally with nothing
visible, so "no visible short" does not clear it:

1. Measure GPIO14 to GND with the button NOT pressed. A few hundred ohms or less means
   shorted. Open circuit (hundreds of kΩ) means the fault is elsewhere.
2. If it measures shorted, lift one leg of the switch and re-measure. Still shorted =
   a solder bridge or damaged trace/pad. Clear = the tact switch itself has failed closed.
3. Check for flux residue or moisture under the button. This buggy sees water.

**What it costs until fixed:** BTN2 = next-screen and = SAVE in the picker. BTH3 (previous
screen) still works, so all four screens are reachable going backwards, and `/packs` over
WiFi does pack numbering with no buttons at all. He declined a SAVE remap ("no leave
buttons as is"), so that offer stands if he changes his mind.

**Benign side effect, for the record:** for the first 15 s of every boot, before the
detector fires, a permanently-low BTN2 looks like one held press. BTN2 acts on the falling
edge only, so it produces a single screen change at boot and then nothing. Not a loop.

### What had been ruled OUT by reading, before the instrumentation

- **Pin collision: no.** `GPIO14` appears exactly once in the whole config — `BUTTON2_PIN`.
  Pack RX are 1/16/17/18, shared TX 2, NeoPixels 10/13, power latch 15, buttons 0/14/21,
  `TFT_BL` 38, `BAT_ADC` 4, RTC 11/12. Nothing else touches 14.
- **Missing pull-up: no.** `Display.ino:1144-1146` sets `INPUT_PULLUP` on all three.
- **The detector logic itself: looks sound.** 15 s of continuous LOW, cleared the moment
  the pin reads HIGH.

### THE REAL CANDIDATE — deep-sleep pad hold is never released

`PowerManager.ino:54-55`, in `doSleep()`:

```c
gpio_hold_en((gpio_num_t)POWER_EN_PIN);
gpio_deep_sleep_hold_en();          // ← latches EVERY pad, not just POWER_EN
```

and `Okai_BMS_Display.ino:11` on wake only releases ONE pad:

```c
gpio_hold_dis((gpio_num_t)POWER_EN_PIN);
// no gpio_deep_sleep_hold_dis(), no release of GPIO14 / GPIO21 / GPIO0
```

`gpio_deep_sleep_hold_en()` latches all pads through deep sleep. On wake the latch is still
armed and **an internal pull-up cannot fight a hold latch**. If GPIO14 was LOW at the
moment of sleep, it stays electrically LOW after wake until the hold is released — which
this firmware never does.

That predicts exactly what he reports: **not a short, intermittent, cleared by a true power
cycle, back again later.** It also explains why someone previously had to re-assert
`pinMode(BUTTON1_PIN, INPUT_PULLUP)` at `PowerManager.ino:37` — the same class of problem,
patched for one pin only.

**Caveat, stated honestly: not yet proven for his sightings.** The boot logs captured on
2026-10-10 and 2026-10-11 show `rst:0x15 (USB_UART_CHIP_RESET)`, i.e. a USB reset, not a
deep-sleep wake. So the latch theory explains a sleep/wake cycle, and he may not have slept
the board. It is a real bug either way and must be fixed, but it is not confirmed as the
cause of what he is looking at.

### INSTRUMENT BEFORE FIXING — do not guess at this again

Add to the `[DIAG]` line: the **raw** `gpio_get_level(14)`, `digitalRead` of all three
buttons, and the reset reason (`esp_reset_reason()`). That distinguishes, with evidence:

| Observation | Meaning |
|---|---|
| raw level 1 but `_btnStuck[1]` true | detector/state bug — firmware, mine to fix |
| raw level 0, reset reason = deep sleep | the hold latch above — firmware, fix is 2 lines |
| raw level 0, reset reason = power-on/USB | genuinely pulled low externally — hardware |

### Fix to land regardless of the above

In `setup()`, before any `pinMode`:

```c
gpio_deep_sleep_hold_dis();
gpio_hold_dis((gpio_num_t)POWER_EN_PIN);
gpio_hold_dis((gpio_num_t)BUTTON1_PIN);
gpio_hold_dis((gpio_num_t)BUTTON2_PIN);
gpio_hold_dis((gpio_num_t)BUTTON3_PIN);
```

⚠️ **`POWER_EN_PIN` ordering is safety-critical.** `POWER_EN` must stay HIGH or the LiPo
rail drops. Release its hold only *after* `pinMode(OUTPUT)` + `digitalWrite(HIGH)` have
re-driven it, never before. Get this order wrong and the board powers itself off.

---

## 3. Pack label missing on the charging screen

> "I still don't see the port number showing on the main screen in the Charge 1. I see P1
> but I do not see the actual dash and then the actual label so it's not helping."

`portPackStr()` (`PackLabel.ino:274`) returns a bare `P1` on exactly one path:

```c
if (!packs[port].valid) { snprintf(buf, len, "P%u", port + 1); return; }
```

So either that port's `.valid` is false when the charging screen draws it, or the charging
tier has a second label path that does not call `portPackStr()` at all — there is a `pl`
variable near `Display.ino:998` used for the empty-port case. **Read that whole block
before changing anything.** Expected result is `P1 - 6`, or `P1 - ?` in amber when the pack
has no number yet.

---

## 4. Bottom band still reads "CHARGING" with no ETA

> "the lower bar still says charging no eta! what the hell. lost space."

`Display.ino:614`:

```c
else if (slowest < 0) snprintf(out, len, "CHARGING");   // still warming up
```

The word was supposed to be gone — it only survives as the "estimator has no answer yet"
case. He is seeing it persist, which means **`slowest` stays negative**, i.e. the SOC-slope
estimator never converges. With pack 3 sitting at 43 % it should produce a figure, so this
is a bug in the estimator, not in the band.

Investigate `packChargeMins()` / the slope sampler. If an estimate genuinely cannot be had,
the band must still not waste the row on the word "CHARGING" — print something that costs
him nothing, or give the space back.

---

## 5. `/packs` — second column must be HIS pack number, and editable for the LIVE pack

> "the okai.local/packs allows to change the registry that is the past or current logging
> but not the top chart. the idea is to change not the past but the present connected pack
> and the top tabel has 2nd colum with cyc,,, and a number i knwo this si hwo you id nut
> not me i want 2nd colum to have the pack labe number the one i choose the one i wrote in
> white marker! that is the one i can change doh.. fix that."

Two distinct asks:

1. **Top table, column 2: show the pack LABEL number**, not the cycle count. Cycle count is
   how the firmware identifies a pack; it is not how he identifies one. The number in white
   marker is the only identifier he has in his hand. Keep CYC visible somewhere if it is
   cheap, but it does not own column 2.
2. **Make the live/connected pack's number editable from the top table.** Today the
   set-or-delete route (`WiFiServer.ino:988`) operates on a *stored record*. He wants to
   change the number on the pack that is plugged in right now, from the page — which is the
   whole point, since BTN2 (SAVE) has been unreliable.

Relevant: `handlePacks` at `WiFiServer.ino:705`, table header at `:754`
(`<th>auto</th><th>label</th>...`).

---

## Process note — his actual complaint, and it is fair

> "I need you to do a good audit and good coding. This is ridiculous. We're trying to fix
> something and every time you try to fix something you mess it up more."

He is right about the pattern. Items 1 and 2 are both cases of me changing something and
making it worse, and item 1 came from me asking him a question I should have answered from
his own earlier instruction. **Rules for this batch:**

1. **Instrument before fixing** anything not yet proven — item 2 above.
2. **Fix all five in ONE build**, not five flashes.
3. **Rex audit the delta before flashing**, per project convention.
4. **Do not ask him taste questions** that his earlier words already answer. Search the
   record first.

## State of the device

`0.3.6-d43d4e2` is flashed and verified. Keep-alive `maxgap=1000ms`, LittleFS mounted,
WiFi joining `monterAC`. Three networks stored. `psram=0/0` still open (chip has 8 MB OPI
PSRAM, `psramFound()` false, `build.defines=` empty in `boards.txt`). Registry still holds
the stale `#4` on the 46-cycle pack; the owner's wipe-and-rebuild has not been done.
