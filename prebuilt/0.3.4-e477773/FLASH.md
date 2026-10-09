# Okai BMS Display — 0.3.4 / `e477773` — flash instructions

Built 2026-10-09, arduino-cli, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, **board-default partitions (no `PartitionScheme`
override — never `huge_app`)**. App **1,289,840 B (40%)** · RAM **69,512 B (21%)**.

**BENCH FLASH: GO.** Seven audit rounds behind this build. Supersedes `0.3.3-0ec21e1`,
which is the version currently on the board — that folder is deleted so there is only ever
one current set.

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** None is shipped here, on purpose.

Either wipes `ffat` at `0x610000` (every CSV log, the `/packs` registry, the saved clock)
**and** `nvs` at `0x9000` (the saved WiFi passwords).

These four images write `0x0`–`0x14AE70`. `ffat` starts at `0x610000` ⇒ **4 MB headroom.**

## 1. Verify the board

MAC must read **`a0:f2:62:e1:e9:ec`**. Ports float — verify by MAC, never by port. On the
Mint bench the Okai is native USB and enumerates as **`/dev/ttyACM0`** (`303a:1001`);
the CH340 boards are the `ttyUSB*` ones.

```
esptool --chip esp32s3 --port /dev/ttyACM0 read-mac      # esptool v5 (tablet runs 5.4.0)
esptool.py --chip esp32s3 --port /dev/ttyACM0 read_mac   # esptool v4
```

A passive alternative needing no esptool: `udevadm info /dev/ttyACM0 | grep ID_SERIAL_SHORT`
prints the MAC straight out of the USB descriptor.

## 2. Check the files, then flash

```
sha256sum -c SHA256SUMS.txt

esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     okai.bootloader.bin \
  0x8000  okai.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 okai.app.bin
```

The MAC prints in esptool's own preamble **before** anything is written, so Ctrl+C on a
mismatch is safe. The flags match the bootloader header already on the chip (byte 2 = 2 →
DIO; byte 3 = `0x4F` → 16 MB / 80 MHz), so the same bytes land.

| File | Offset | Size | sha256 (first 8) |
|---|---|---|---|
| `okai.bootloader.bin` | `0x0` | 19,984 B | `b68c1ed3` |
| `okai.partitions.bin` | `0x8000` | 3,072 B | `ace02503` |
| `boot_app0.bin` | `0xe000` | 8,192 B | `f94c5d78` |
| `okai.app.bin` | `0x10000` | 1,289,840 B | `bc17986a` |

Bootloader, partitions and boot_app0 are byte-identical to 0.3.0/0.3.3.

## 3. 🔴 The proof, before the buggy goes anywhere

Keep-alive priority is **20**. **The proof on record was taken at 18 and does not transfer.**

1. **Serial** — `[DIAG]` `maxgap` far below 5000 ms (~1000 is normal). `0.3.3` measured
   **157/157 lines at 1000 ms**, including a ~9 s main-loop block during a join across which
   the heartbeat advanced nine beats. Watch `maxgap` across an **AP→station switch**.
   Capture `heap=`, the new `min=`, and the new `iram=`.
2. **Both NeoPixel strips** — px11 white blink on each.
3. **The owner confirms the buggy powers up AND STAYS powered.**

Stage 3 is the one that counts. June 2026: perfect 1 s serial cadence, packs still did not
wake. **Serial alone is not proof.** If the buggy is needed first, flash
`Source/KeepAliveDumb/KeepAliveDumb.ino`.

## 4. What is new since 0.3.3

- **Pack number on every screen:** `P3 - 6` on the home rows, charging tiles, cell-health
  rows and the detail header. `P3 - ?` means a pack is talking but is not identified yet —
  that is when to assign a number. The cycle count came off the cell-health row to make room.
- **Link dot** beside `W:ON` — **green** when something is actually connected (a joined
  network *or* an AP with a client), **red** when the radio is on and nothing is connected,
  absent when the radio is off.
- **Picker confirmation:** SAVE now holds a green tick for ~1.1 s naming the number
  ("PACK 6 / SAVED / on port 3") instead of closing instantly, so SAVE and EXIT can no
  longer be confused. Hints reworded to `1:next  2:SAVE  3:exit`.
- **Reconnects by itself after a drop** for 30 minutes, rescanning all saved networks — so
  hotspot-off-in-the-car then home-network-in-the-garage connects with no action. Station
  only on that path; a charger gets unlimited retries.
- **Scan bounded** — association 8 s, scan capped 6 s (was a 60 s library default).
- `FW_VERSION` now actually reads 0.3.4.

## 5. WiFi

**AP is `OkaiBMS` / `12345678`, unchanged and permanent.** It appears whenever no saved
network answers. `http://okai.local` works once joined (confirmed on Android).

On `/wifi`: **add** home and each hotspot — **saving does not disconnect anything** — check
all appear in the list, then press **Join the strongest saved network**. Holds 4. Selection
is by **signal strength**, not list order.

## 6. Bench notes

- **Plug a pack in before powering up.** With no pack it cannot tell a ride from a bench, so
  it starts the AP instead of scanning.
- **Registry wipe: press until the page says `0 remain`** (up to 32 per press).
- **Then packs ONE AT A TIME**, each number from the white marker. The label seed is
  disarmed, so every number comes from the owner.
- **`FS!` red, top right** = LittleFS did not mount ⇒ **nothing is being logged.** Stop.
- `/rawdump` shows all four ports, self-refreshing every 3 s.

## Known, accepted, non-blocking

- A WiFi join blocks the loop up to ~14 s worst case, freezing the screen and pack reads.
  **Owner accepted this explicitly.** It cannot stall the keep-alive — separate core, higher
  priority, proven across a 9 s block on the bench.
- A join longer than 10 s invalidates packs; the recovery edge zeroes that session's Wh
  accounting. The real fix is a non-blocking join, deferred on purpose.
- An SSID containing an apostrophe cannot be removed with **forget** (add/save/join all
  work). Owner confirmed his networks have none.
- An idle phone associated to a **dock** fallback AP can keep the home network from being
  retried until a ride or a reboot. Pre-existing, documented, needs its own pass.

## Partition table (decoded from the shipped image; matches the device)

```
nvs        0x00009000  0x00005000   ← saved WiFi passwords
otadata    0x0000E000  0x00002000
app0       0x00010000  0x00300000
app1       0x00310000  0x00300000
ffat       0x00610000  0x009E0000   ← ALL LOGS AND THE PACK REGISTRY
coredump   0x00FF0000  0x00010000
```
