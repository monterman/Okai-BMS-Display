# Okai BMS Display — 0.3.1 / `385cfea` — flash instructions

Built 2026-10-09 from `Source/Okai_BMS_Display` on the Windows PC, arduino-cli with
`esp32:esp32@3.3.8`, FQBN `esp32:esp32:lilygo_t_display_s3`, **board-default partitions
(no `PartitionScheme` override — never `huge_app`)**.

App **1,285,520 B (40% of the 3 MB slot)** · RAM **63,936 B (19%)**.

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** (None is shipped here on purpose.)

Either wipes `ffat` at `0x610000` — every CSV log, the pack registry in `/packs/`, the
label seed marker and the saved clock. It also wipes `nvs` at `0x9000`, which is where the
**home WiFi password** lives.

The four images below write `0x0`–`0x149D90`. `ffat` begins at `0x610000`, so there is
**4 MB of headroom** and the logs are never touched by a correct flash.

## Verify the board first

MAC must read **`a0:f2:62:e1:e9:ec`**. Ports float; verify by MAC, not by port number.

```
esptool --chip esp32s3 --port <PORT> read-mac      # esptool v5 spelling
esptool.py --chip esp32s3 --port <PORT> read_mac   # esptool v4 spelling
```

> The Mint tablet runs **esptool 5.4.0**, which renamed subcommands **and** options:
> `write-flash`, `read-mac`, `--flash-mode`, `--flash-freq`, `--flash-size`.

## Flash (app-only region writes, no erase)

```
esptool --chip esp32s3 --port <PORT> --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     okai.bootloader.bin \
  0x8000  okai.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 okai.app.bin
```

| File | Offset | Size |
|---|---|---|
| `okai.bootloader.bin` | `0x0` | 19,984 B |
| `okai.partitions.bin` | `0x8000` | 3,072 B |
| `boot_app0.bin` | `0xe000` | 8,192 B — from the core, **not** the sketch build |
| `okai.app.bin` | `0x10000` | 1,285,520 B |

Checksums in `SHA256SUMS.txt`. Verify before writing:
`sha256sum -c SHA256SUMS.txt`

## Partition table shipped here (decoded, identical to the device's)

```
nvs        off=0x00009000 size=0x00005000   ← home WiFi credentials live here
otadata    off=0x0000E000 size=0x00002000
app0       off=0x00010000 size=0x00300000
app1       off=0x00310000 size=0x00300000
ffat       off=0x00610000 size=0x009E0000   ← ALL LOGS AND THE PACK REGISTRY
coredump   off=0x00FF0000 size=0x00010000
```

## 🔴 After flashing — the SOP-038 proof, before anything else matters

The keep-alive task priority changed **18 → 20** in this build. The proof on record was
taken at 18 and **does not transfer**. All three stages are required:

1. **Serial** — watch `[DIAG]` for `maxgap`. Must stay far below 5000 ms; ~1000 is normal.
   Also log `heap=` / `min=` while you are there.
2. **Both NeoPixel strips** — px11 white blink on each.
3. **The owner physically confirms the buggy powers up AND STAYS powered.**

Stage 3 is not optional. In June 2026 the serial cadence was a perfect 1 s and the packs
still did not wake. Serial alone is **not** proof.

If the buggy is needed before this proof is run, flash
`Source/KeepAliveDumb/KeepAliveDumb.ino` instead — keep-alive only, nothing else.

## Bench notes for this build

- **Plug a pack in before powering up.** With no pack the board cannot tell a ride from a
  bench, so it comes up on its own AP rather than scanning. It now recovers by itself once
  frames appear, but starting with a pack attached skips the detour.
- **First boot has no stored WiFi.** It comes up as `OkaiBMS-XXXXXX` (WPA2, MAC-suffixed so
  a phone will not auto-join). Join it, open `http://192.168.4.1/wifi`, enter the home SSID
  and password **once**. Thereafter it joins by itself and answers at `http://okai.local`.
  Credentials go to NVS, so they are not in source control and survive later flashes.
- **Registry wipe: press it until the page says `0 remain`.** One press deletes up to 32
  records and the page now states what is left. Rebuilding on leftovers re-blends the wear
  history the wipe exists to destroy.
- **Then plug packs in ONE AT A TIME** and set each number from the white marker.
- `FS!` in red at the top right means LittleFS did not mount — **nothing is being logged.**
