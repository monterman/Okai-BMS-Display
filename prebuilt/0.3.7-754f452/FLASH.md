# Okai BMS Display — 0.3.7 / `754f452` — flash instructions

Built 2026-10-11, arduino-cli, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, **board-default partitions (no `PartitionScheme`
override — never `huge_app`)**. App **1,290,064 B (41%)** · RAM **69,504 B (21%)**
(`.dram0.data` 23,240 + `.dram0.bss` 46,264, read off the ELF).

**✅ FLASHED AND VERIFIED 2026-10-11** on `a0:f2:62:e1:e9:ec`, app-only at `0x10000`.
The board read back `Okai BMS Display ready — 0.3.7`, `maxgap=1000ms`, LittleFS mounted,
and all three stored networks survived (`monterAC` joined, IP 192.168.1.53). Earlier in
the same session COM4 wedged (`device attached to the system is not functioning`) after
repeated DTR/RTS cycling from serial captures; **nothing was written on those two
attempts** and replugging the USB cleared it. If it happens again, replug — do not retry
into a port that will not open.

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** None is shipped here, on purpose.

Either wipes `ffat` at `0x610000` (every CSV log, the `/packs` registry, the saved clock)
**and** `nvs` at `0x9000` (the saved WiFi passwords).

## ⭐ This is an APP-ONLY flash

Bootloader (`b68c1ed3`) and partition table (`ace02503`) are **byte-identical to what is
already on the chip** — verified before the 0.3.7 write. Per SOP-012, app differs + table
matches means the write is safe and **cannot touch `nvs` or `ffat`**. One file, one offset:

```powershell
$ESPTOOL = "C:\Users\Andress Montero\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.2.0\esptool.exe"
& $ESPTOOL --chip esp32s3 --port COM<N> read-mac        # MUST read a0:f2:62:e1:e9:ec
& $ESPTOOL --chip esp32s3 --port COM<N> --baud 921600 write-flash `
    --flash-mode dio --flash-freq 80m --flash-size 16MB `
    0x10000 okai.app.bin
```

Ports float — **verify by MAC, never by port.** On Windows the Okai is native USB and
shows as `USB Serial Device (COMn)` with `VID_303A&PID_1001`. On the Mint bench it is
`/dev/ttyACM0`. The MAC prints in esptool's preamble **before** anything is written, so
Ctrl+C on a mismatch is safe.

If a full four-image write is ever needed, the other three files are here and the offsets
are `0x0` / `0x8000` / `0xe000`.

| File | Offset | Size | sha256 (first 8) |
|---|---|---|---|
| `okai.bootloader.bin` | `0x0` | 19,984 B | `b68c1ed3` |
| `okai.partitions.bin` | `0x8000` | 3,072 B | `ace02503` |
| `boot_app0.bin` | `0xe000` | 8,192 B | `f94c5d78` |
| `okai.app.bin` | `0x10000` | 1,290,064 B | `33a6ee62` |

`sha256sum -c SHA256SUMS.txt` before writing.

## What is new since 0.3.6

Three of the owner's five gripes about 0.3.6. The other two are open — see
`docs/OPEN-2026-10-11-owner-gripes.md` items 4 and 5.

- **`WiFi` letters back to state colour** (`wifiActive ? C_GOOD : C_DIM`). 0.3.5 drew
  them `C_DIM` unconditionally, so grey letters sat beside a green dot and asserted
  off-and-connected at the same time. The rule, his: **grey = radio off, green = radio
  on**; the dot carries the link on top — green = joined a network or an AP client is
  attached, red = radio on and nothing connected, absent = radio off.
- **The pack number now renders on the charging screen.** `drawScreenCharging()` built
  its own label with a bare `snprintf(pl, …, "P%u  ---", i+1)` instead of calling
  `portPackStr()`, so a pack that was talking but had no charger on that port printed a
  bare `P1`. Now `portPackStr()` + `packLabelColor()`, and the buffer went **10 → 16 B**
  because `"P4 - 8" + "  ---" + NUL` is 12 and the old size would have truncated the very
  thing the fix exists to show.
- **Deep-sleep pad latch released at boot.** `doSleep()` calls
  `gpio_deep_sleep_hold_en()`, which latches **every** pad, and `setup()` released only
  `POWER_EN`. After a wake the buttons stayed electrically frozen at their sleep-time
  level and an internal pull-up cannot fight a hold latch. Now
  `gpio_deep_sleep_hold_dis()` + `gpio_hold_dis()` on all three buttons.
  ⚠️ **Placed AFTER `pinMode(POWER_EN, OUTPUT)` + `digitalWrite(HIGH)` on purpose** —
  drop the global hold while `POWER_EN` is undriven and the board cuts its own LiPo rail.
- **BTN diagnostics**: `[BTN] raw b1=… b2=… b3=… stk=… rst=…` every 3 s, which is what
  settled the BTN2 question below.

## How to use the picker (it is per-port, by design)

| Step | Button | Result |
|---|---|---|
| 1 | **BTN2** | Home → screen 1 (detail) |
| 2 | **BTN1** short tap (<1.5 s) | next **connected** pack; screen 1 names the one shown |
| 3 | **Hold BTN3** 0.8 s | opens the picker **for that pack** |
| 4 | **BTN1 / BTN2 / BTN3** | next number / SAVE (green tick 1.1 s) / exit |

Holding BTN3 on **Home** opens the picker for **port 1 only** (`uint8_t port = 0`).
For a full rebuild use `okai.local` → `/packs` instead: every port on one page, no buttons.

## 🔴 The proof, before the buggy goes anywhere

Keep-alive priority is **20**.

1. **Serial** — `[DIAG]` `maxgap` far below 5000 ms (~1000 is normal).
2. **Both NeoPixel strips** — px11 white blink on each.
3. **The owner confirms the buggy powers up AND STAYS powered.**

Stage 3 is the one that counts. June 2026: perfect 1 s serial cadence, packs still did not
wake. **Serial alone is not proof.** Fallback: `Source/KeepAliveDumb/KeepAliveDumb.ino`.

## Known, accepted, non-blocking

- `psram=0/0` on every `[DIAG]` line. The chip **has** 8 MB OPI PSRAM (esptool banner:
  *"Embedded PSRAM 8MB (AP_3v3)"*) but `psramFound()` is false, so the ~106 kB framebuffer
  sits in internal DRAM. `lilygo_t_display_s3.build.defines=` is empty in `boards.txt`, so
  `-DBOARD_HAS_PSRAM` is never defined — first place to look. Needs its own pass.
- `BTN2!` red in the header = GPIO14 read low for 15 s and its presses are being ignored.
  **2026-10-11, settled by the new `[BTN]` dump: this one is HARDWARE.** First boot after
  flashing printed `raw b1=1 b2=0 b3=1  stk=010  rst=11` — pin 14 low at the pad from
  boot while BTN1 and BTN3 read HIGH through the *identical* `INPUT_PULLUP` block and the
  same sampling function, and `rst=11` is `ESP_RST_USB`, not a sleep wake, so the pad
  latch fixed in this build is not the cause. Probe steps are in
  `docs/OPEN-2026-10-11-owner-gripes.md` § 2; a tact switch can fail closed with nothing
  visible. Cost until fixed: BTN2 is next-screen and SAVE, so walk the screens backwards
  with BTN3 and do pack numbering on `okai.local` → `/packs`, which needs no buttons.
  `FS!` is the other red tag, and that one means nothing is being logged — stop if you
  see it.
- A WiFi join blocks the loop up to ~14 s, freezing the screen and pack reads. **Owner
  accepted this explicitly.** It cannot stall the keep-alive — separate core, higher
  priority, proven across a 9 s block on the bench.
- An SSID containing an apostrophe cannot be removed with **forget**.
- An idle phone on a **dock** fallback AP can keep the home network from being retried
  until a ride or a reboot. Pre-existing, documented, needs its own pass.

## Partition table (unchanged; matches the device)

```
nvs        0x00009000  0x00005000   ← saved WiFi passwords
otadata    0x0000E000  0x00002000
app0       0x00010000  0x00300000
app1       0x00310000  0x00300000
ffat       0x00610000  0x009E0000   ← ALL LOGS AND THE PACK REGISTRY
coredump   0x00FF0000  0x00010000
```
