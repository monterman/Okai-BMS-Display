# Okai BMS Display — 0.3.6 / `d43d4e2` — flash instructions

Built 2026-10-11, arduino-cli, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, **board-default partitions (no `PartitionScheme`
override — never `huge_app`)**. App **1,289,808 B (40%)** · RAM **69,504 B (21%)**.

**⚠️ SUPERSEDED — do not flash this set. Use `prebuilt/0.3.7-754f452/`.**
History, for the record: 0.3.6 *was* flashed and verified on 2026-10-11 after the
COM4 wedge below cleared, and **0.3.7 then replaced it the same day**. The "not yet
flashed / board runs 0.3.5" line that used to sit here was written before the write
happened and was never corrected; it is wrong and this is the correction.
The host fault it described was real: COM4 enumerated but refused to open
(`device attached to the system is not functioning`) after repeated DTR/RTS cycling from
serial captures. Replugging the USB cleared it.

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** None is shipped here, on purpose.

Either wipes `ffat` at `0x610000` (every CSV log, the `/packs` registry, the saved clock)
**and** `nvs` at `0x9000` (the saved WiFi passwords).

## ⭐ This is an APP-ONLY flash

Bootloader (`b68c1ed3`) and partition table (`ace02503`) are **byte-identical to what is
already on the chip** — verified before the 0.3.5 write. Per SOP-012, app differs + table
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
| `okai.app.bin` | `0x10000` | 1,289,808 B | `fb66142e` |

`sha256sum -c SHA256SUMS.txt` before writing.

## What is new since 0.3.5

- **Picker heading shows `PORT n` at size 3.** It was `setTextSize(1)` — a 6×8 font, the
  smallest text on an overlay whose whole job is tying the pack in your hand to a socket
  on the buggy. The question is size 2 beside it, the number stays size 6.
- **The picker no longer opens by itself.** The ambiguity prompt that covered Home at
  every boot is deleted. Nothing on the device now raises an overlay without a button.
- **Amber `P3 - ?`** wherever a pack's label is drawn on Home and the detail screen, when
  that pack has no number or the registry cannot tell which stored pack it is. This is
  what replaces the prompt — the state is still visible, just not in your way.
- Hint row moved 2 px down; the size-6 value cell used to overlap it.

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
  Seen 2026-10-10 on two boots, **cleared after a restart**. Red text in that slot always
  means a fault; a restart is a reasonable first move. `FS!` is the other one, and that one
  means nothing is being logged — stop if you see it.
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
