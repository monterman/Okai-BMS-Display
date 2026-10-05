# Prebuilt flash images

For flashing from a machine that **cannot compile** — the Linux Mint Surface Go bench has no room
for the ~3 GB ESP32 core. These four files are everything `esptool` needs: **1.2 MB total**, versus
the 16 MB `.merged.bin` that `arduino-cli` also produces.

Built on the Windows bench with `arduino-cli`, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, board-default partition scheme `app3M_fat9M_16MB`.
See [SOP-012] for the Windows bench and [SOP-037] for the Linux one.

## `0.3.0-6d9bb7d/`

| File | Offset | Size | What |
|---|---|---|---|
| `okai.bootloader.bin` | `0x0` | 20 KB | second-stage bootloader |
| `okai.partitions.bin` | `0x8000` | 4 KB | partition table |
| `boot_app0.bin` | `0xe000` | 8 KB | otadata — points the bootloader at `app0`. From the core, **not** the sketch build, so a machine without the core installed has no other copy |
| `okai.app.bin` | `0x10000` | 1.1 MB | the firmware (1,138,087 bytes, 36 % of the 3 MB app slot) |

`SHA256SUMS.txt` is in the directory. Verify before flashing.

Source commit: **`6d9bb7d`** — "Close the audit's flash gate: tight-match seeding,
one-record-one-port, session cap". The working tree was clean and the binaries are newer than every
source file, so they are that commit exactly.

## Flash it

This is the real `arduino-cli` recipe for this board, read out of `platform.txt` and `boards.txt`
rather than remembered — same offsets, same flags, same baud:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset \
  write_flash -z --flash-mode keep --flash-freq keep --flash-size keep \
  0x0     okai.bootloader.bin \
  0x8000  okai.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 okai.app.bin
```

> **`write_flash` with an UNDERSCORE, deliberately.** `platform.txt` writes it as `write-flash`
> because the core bundles esptool 5.2.0, which renamed the subcommands to hyphens. **esptool v4
> accepts only the underscore form**, and v5 kept the underscore as an alias — so the underscore
> works on both and the hyphen does not. Use the underscore unless you know you are on v5+.

## 🔴 Before you flash

1. **Verify the board by MAC, not by port.** `esptool --port /dev/ttyACM0 read-mac` must return
   **`a0:f2:62:e1:e9:ec`**. Ports float; the MAC does not.
2. **Do not pass `--erase-all`, and do not flash the merged image.** Either one wipes the `ffat`
   partition at `0x610000` (9.875 MB) — which holds every CSV log, the pack registry in `/packs/`,
   the labels, the clock offset and the session counters. Writing only the four offsets above leaves
   all of it untouched.
3. **`ffat` is confirmed at `0x610000`, size `0x9E0000`**, from
   `esp32/3.3.8/tools/partitions/app3M_fat9M_16MB.csv`. Nothing in the list above comes near it; the
   highest write ends around `0x12B000`.
4. **Keep the partition scheme identical.** A different scheme moves `ffat` and loses the logs.

## What to capture on the first boot after this flash

The one-shot label seed fires once, on this boot only, and prints the binding this project has
wanted since July:

```
[SEED] port1 CYC-0037 = #1 (owner's printed label)
[SEED] port2 CYC-XXXX = #4 (owner's printed label)
[SEED] port3 CYC-XXXX = #6 (owner's printed label)
```

**Save that output and paste it into `docs/Pack_Registry.md`.** If a port instead prints
*"match too loose to label"*, that is the audit's gate 4 refusing to guess — not a failure. Use the
on-screen picker for that pack.

[SOP-012]: ../../myPKA-BREMOTE/Team%20Knowledge/SOPs/SOP-012-arduino-cli-compile-flash.md
[SOP-037]: ../../myPKA-BREMOTE/Team%20Knowledge/SOPs/SOP-037-linux-mint-terminal-bench.md
