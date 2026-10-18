# Prebuilt flash images

For flashing from a machine that **cannot compile** — the Linux Mint Surface Go bench has no room
for the ~3 GB ESP32 core. These four files are everything `esptool` needs: **1.2 MB total**, versus
the 16 MB `.merged.bin` that `arduino-cli` also produces.

Built on the Windows bench with `arduino-cli`, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, board-default partition scheme `app3M_fat9M_16MB`.
See [SOP-012] for the Windows bench and [SOP-037] for the Linux one.

## Which set to flash

**Current: `0.3.7-754f452/`.** Each set carries its own `FLASH.md` with that version's
delta, sizes and hashes, and its own `SHA256SUMS.txt`. **Read the set's own `FLASH.md`,
not this file, for whether the write is app-only or all four images** — that distinction
is what protects the logs.

| Set | Commit | State |
|---|---|---|
| **`0.3.7-754f452/`** | `754f452` | **CURRENT — on the chip, verified 2026-10-11.** App-only write at `0x10000` |
| `0.3.6-d43d4e2/` | `d43d4e2` | superseded the same day. Kept as history |
| `0.3.4-e477773/` | `e477773` | **last full four-image set actually written to the chip.** Keep it: if the bootloader or partition table ever needs restoring, these bytes are known to match the device |
| `0.3.0-6d9bb7d/` | `6d9bb7d` | historical. App 1,138,087 B. "Close the audit's flash gate: tight-match seeding, one-record-one-port, session cap" |

0.3.5 was never cut as an image set — it shipped as an app-only write from the build tree.

Every set holds the same four files:

| File | Offset | What |
|---|---|---|
| `okai.bootloader.bin` | `0x0` | second-stage bootloader |
| `okai.partitions.bin` | `0x8000` | partition table |
| `boot_app0.bin` | `0xe000` | otadata — points the bootloader at `app0`. From the core, **not** the sketch build, so a machine without the core installed has no other copy |
| `okai.app.bin` | `0x10000` | the firmware |

`sha256sum -c SHA256SUMS.txt` inside the directory before writing.

## Flash it

This is the real `arduino-cli` recipe for this board, read out of `platform.txt` and `boards.txt`
rather than remembered — same offsets, same flags, same baud:

**Check the version first — the spelling differs and you cannot mix the two:**

```bash
esptool version
```

**esptool v5.x** (what `pipx install esptool` gives you today, and what the core bundles):

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset \
  write-flash -z --flash-mode keep --flash-freq keep --flash-size keep \
  0x0     okai.bootloader.bin \
  0x8000  okai.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 okai.app.bin
```

**esptool v4.x** — underscores throughout, including the option *values*:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode keep --flash_freq keep --flash_size keep \
  0x0     okai.bootloader.bin \
  0x8000  okai.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 okai.app.bin
```

> **Do not mix the spellings.** v5 renamed the subcommand *and* the option names *and* their values
> at once (`write_flash` → `write-flash`, `--flash_mode` → `--flash-mode`, `default_reset` →
> `default-reset`, `read_mac` → `read-mac`). v5 still accepts the v4 spellings as deprecated
> aliases; v4 accepts none of the v5 ones. So an underscore subcommand with hyphenated flags — the
> obvious-looking middle ground — parses on neither. Pick one block and use it whole.
>
> The failure is harmless: a wrong spelling dies at argument parse, before anything is written.

## 🔴 Before you flash

1. **Verify the board by MAC, not by port.** Ports float; the MAC does not. Must return
   **`a0:f2:62:e1:e9:ec`**:
   - v5: `esptool --port /dev/ttyACM0 read-mac`
   - v4: `esptool.py --port /dev/ttyACM0 read_mac`
2. **Do not pass `--erase-all`, and do not flash the merged image.** Either one wipes the `ffat`
   partition at `0x610000` (9.875 MB) — which holds every CSV log, the pack registry in `/packs/`,
   the labels, the clock offset and the session counters. Writing only the four offsets above leaves
   all of it untouched.
3. **`ffat` is confirmed at `0x610000`, size `0x9E0000`**, from
   `esp32/3.3.8/tools/partitions/app3M_fat9M_16MB.csv`. Nothing in the list above comes near it; the
   highest write ends around `0x12B000`.
4. **Keep the partition scheme identical.** A different scheme moves `ffat` and loses the logs.

## What to capture on the first boot after a flash

```
Okai BMS Display ready — <version>     ← confirms what actually landed
[DIAG] ... maxgap=1000ms ...           ← SOP-038. Anything near 5000 and stop
[LOG] LittleFS ready <used> / <total>  ← no `FS!` tag in the header
[WiFi] joined "<ssid>" IP=... RSSI=...  ← stored networks survived the write
[REG] ...                              ← which pack each port was identified as
```

> **The one-shot label seed is disarmed** — `LABEL_SEED_LIST { 0, 0, 0, 0 }` in
> `Config.h`. Older copies of this file told you to capture `[SEED]` lines on the first
> boot; there are none, and there have not been since the seed was zeroed. Pack numbering
> is done by hand now: `okai.local` → `/packs` (no buttons needed), or the on-screen
> picker via **hold BTN3 0.8 s** on the detail screen.

[SOP-012]: ../../myPKA-BREMOTE/Team%20Knowledge/SOPs/SOP-012-arduino-cli-compile-flash.md
[SOP-037]: ../../myPKA-BREMOTE/Team%20Knowledge/SOPs/SOP-037-linux-mint-terminal-bench.md
