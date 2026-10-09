# Okai BMS Display — 0.3.2 / `49bf523` — flash instructions

Built 2026-10-09, arduino-cli, `esp32:esp32@3.3.8`, FQBN
`esp32:esp32:lilygo_t_display_s3`, **board-default partitions (no `PartitionScheme`
override — never `huge_app`)**. App **1,288,464 B (40%)** · RAM **63,936 B (19%)**.

Supersedes `0.3.1-385cfea` (AP name reverted to `OkaiBMS`; `/wifi` handles several
networks in one sitting).

## 🔴 Two ways to destroy the logs. Neither is recoverable.

1. **Never pass `--erase-all`.**
2. **Never flash a merged image.** None is shipped here, on purpose.

Either wipes `ffat` at `0x610000` (every CSV log, the `/packs` registry, the saved clock)
and `nvs` at `0x9000` (the **saved WiFi passwords**).

These four images write `0x0`–`0x14A910`. `ffat` starts at `0x610000` ⇒ **4 MB headroom.**
A correct flash touches neither the logs nor the credentials.

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

The MAC prints in esptool's own preamble **before** it writes anything, so Ctrl+C on a
mismatch is safe — nothing has been sent to flash at that point.

| File | Offset | Size |
|---|---|---|
| `okai.bootloader.bin` | `0x0` | 19,984 B |
| `okai.partitions.bin` | `0x8000` | 3,072 B |
| `boot_app0.bin` | `0xe000` | 8,192 B — from the core, **not** the sketch build |
| `okai.app.bin` | `0x10000` | 1,288,464 B |

## 3. 🔴 The proof, before the buggy goes anywhere

Keep-alive task priority changed **18 → 20** in this line of builds. **The proof on record
was taken at 18 and does not transfer.** All three stages:

1. **Serial** — `[DIAG]` `maxgap` must stay far below 5000 ms (~1000 is normal). Capture
   `heap=` and `min=` while you are there; nobody has ever measured heap on this board.
2. **Both NeoPixel strips** — px11 white blink on each.
3. **The owner confirms the buggy powers up AND STAYS powered.**

Stage 3 is the one that counts. June 2026: perfect 1 s serial cadence, packs still did not
wake. **Serial alone is not proof.** If the buggy is needed first, flash
`Source/KeepAliveDumb/KeepAliveDumb.ino` instead — keep-alive only.

## 4. WiFi — the access point is unchanged and always there

**SSID `OkaiBMS`, password `12345678`.** Same as it has always been, deliberately: this is
the guaranteed way back into the box. It appears whenever no saved network answers.

First boot has no saved networks, so it comes up as that AP. Join it →
`http://192.168.4.1/wifi` →

- **Add** your home network, then each phone hotspot. **Saving does not disconnect
  anything** — add all of them one after another. Holds **4**.
- The page lists what is stored, with a **forget** button each (SSIDs only; saved
  passwords are never shown back).
- Press **Join the strongest saved network** when finished.
- Selection is by **signal strength**, not list order — there is no ranked priority.

Afterwards it joins by itself at boot and whenever a charge starts, and answers at
**`http://okai.local`**. Credentials live in NVS, so they are not in source control and
they survive a reflash. It never joins while packs are discharging.

## 5. Bench notes

- **Plug a pack in before powering up.** With no pack it cannot tell a ride from a bench,
  so it starts the AP instead of scanning. It recovers once frames arrive; this skips it.
- **Registry wipe: press until the page says `0 remain`.** One press clears up to 32
  records and the page reports what is left. Rebuilding on leftovers re-blends the wear
  history the wipe exists to destroy.
- **Then packs ONE AT A TIME**, each number from the white marker. The label seed is
  disarmed in this build, so every number now comes from the owner.
- **`FS!` in red, top right** = LittleFS did not mount ⇒ **nothing is being logged.** Stop.
- `/rawdump` shows all four ports and refreshes itself every 3 s — a newly plugged pack
  appears within about 3 seconds.

## Partition table (decoded from the shipped image; matches the device)

```
nvs        0x00009000  0x00005000   ← saved WiFi passwords
otadata    0x0000E000  0x00002000
app0       0x00010000  0x00300000
app1       0x00310000  0x00300000
ffat       0x00610000  0x009E0000   ← ALL LOGS AND THE PACK REGISTRY
coredump   0x00FF0000  0x00010000
```
