# Okai BMS Display — 0.3.3 / `0ec21e1` — flash instructions

Built 2026-10-09, arduino-cli, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, **board-default partitions (no `PartitionScheme`
override — never `huge_app`)**. App **1,288,384 B (40%)** · RAM **69,496 B (21%)**.

Cleared by four audit rounds. **BENCH FLASH: GO**, no CRITICAL or HIGH findings.
Supersedes 0.3.0/0.3.1/0.3.2 — those are deleted, not archived, because one of them
contained an out-of-bounds write.

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** None is shipped here, on purpose.

Either wipes `ffat` at `0x610000` (every CSV log, the `/packs` registry, the saved clock)
and `nvs` at `0x9000` (the **saved WiFi passwords**).

These four images write `0x0`–`0x14A8C0`. `ffat` starts at `0x610000` ⇒ **4 MB headroom.**

## 1. Verify the board

MAC must read **`a0:f2:62:e1:e9:ec`**. Ports float — verify by MAC, never by port.

```
esptool --chip esp32s3 --port /dev/ttyACM0 read-mac      # esptool v5 (Mint tablet, 5.4.0)
esptool.py --chip esp32s3 --port /dev/ttyACM0 read_mac   # esptool v4
```

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
mismatch is safe.

| File | Offset | Size | sha256 (first 8) |
|---|---|---|---|
| `okai.bootloader.bin` | `0x0` | 19,984 B | `b68c1ed3` |
| `okai.partitions.bin` | `0x8000` | 3,072 B | `ace02503` |
| `boot_app0.bin` | `0xe000` | 8,192 B | `f94c5d78` |
| `okai.app.bin` | `0x10000` | 1,288,384 B | `e45e7f4d` |

## 3. 🔴 The proof, before the buggy goes anywhere

Keep-alive task priority is **20** in this build (was 18). **The proof on record was taken
at 18 and does not transfer.** All three stages:

1. **Serial** — `[DIAG]` `maxgap` far below 5000 ms (~1000 normal). Capture `heap=` and
   `min=` too; heap has never been measured on this board. Watch `maxgap` across an
   AP→station switch specifically.
2. **Both NeoPixel strips** — px11 white blink on each.
3. **The owner confirms the buggy powers up AND STAYS powered.**

Stage 3 is the one that counts. June 2026: perfect 1 s serial cadence, packs still did not
wake. **Serial alone is not proof.** If the buggy is needed first, flash
`Source/KeepAliveDumb/KeepAliveDumb.ino` — keep-alive only.

## 4. WiFi

**AP is `OkaiBMS` / `12345678`, unchanged and permanent.** It appears whenever no saved
network answers. First boot has no saved networks, so expect it.

Join it → `http://192.168.4.1/wifi` →

1. **Add** home, then each phone hotspot. **Saving does not disconnect anything.** Holds 4.
2. Confirm all three appear in the list (each has a **forget** button).
3. Press **Join the strongest saved network**.

Selection is by **signal strength**, not list order — there is no ranked priority.

Afterwards it joins by itself at boot and whenever a charger is attached, and answers at
**`http://okai.local`**. Credentials live in NVS — not in source control, and they survive
a reflash. It never joins while packs are discharging.

## 5. Bench notes

- **Plug a pack in before powering up.** With no pack it cannot tell a ride from a bench,
  so it starts the AP instead of scanning. It recovers on its own once frames arrive.
- **Registry wipe: press until the page says `0 remain`.** One press clears up to 32
  records and the page reports what is left. Rebuilding on leftovers re-blends the wear
  history the wipe exists to destroy.
- **Then packs ONE AT A TIME**, each number from the white marker. The label seed is
  disarmed, so every number comes from the owner.
- **`FS!` red, top right** = LittleFS did not mount ⇒ **nothing is being logged.** Stop.
- `/rawdump` shows all four ports, self-refreshing every 3 s — a newly plugged pack appears
  in ~3 s. For the port-fingerprint test: one pack, screenshot in port 2, move it to
  port 3, screenshot again.

## Known, accepted, non-blocking

- The WiFi join blocks for up to 15 s, freezing the screen and pack reads. **Owner
  accepted this explicitly.** It cannot stall the keep-alive (separate core, higher
  priority).
- An SSID containing an apostrophe cannot be removed with **forget** (add/save/join all
  work; forget no-ops and says so on serial). Owner confirmed his networks have none.

## Partition table (decoded from the shipped image; matches the device)

```
nvs        0x00009000  0x00005000   ← saved WiFi passwords
otadata    0x0000E000  0x00002000
app0       0x00010000  0x00300000
app1       0x00310000  0x00300000
ffat       0x00610000  0x009E0000   ← ALL LOGS AND THE PACK REGISTRY
coredump   0x00FF0000  0x00010000
```
