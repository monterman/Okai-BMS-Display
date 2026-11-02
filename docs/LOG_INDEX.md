# Log index

One row per raw log of this project. The logs themselves live in `logs/` at the repo root, which is
gitignored: they are kept locally, never in git. Rule: every project keeps its
logs in one folder at its own root, by category, then `YYYY/MM/`, and every file is indexed here.

- **Bench** (`logs/Bench/YYYY/MM/`): bench captures of the display board: serial console logs, registry
  and raw frame dumps, charge logs written by the display, and notes taken at the bench.
- Okai pack logs recorded on BREmote rides are BREmote ride data and are filed with BREmote's logs, not here.

Until 2026-11-02 these 10 files sat in `docs/bench-logs/` and were tracked. They were copied here (each copy
sha256-checked against its source) and removed from the tree; the commits named in the Context column still hold
the originals. The sha256 is of the file as it sat on disk: 7 of the 10 have CRLF line ends there, while git
stored them with LF, so a blob from history hashes differently from its row.

**Columns.** Date: the date in the file's name. Firmware: the build that wrote the file. "(flash record)" means
`6d9bb7d` was flashed on 5 Oct (`8da5ba4`) and no firmware commit came before the commit that added the file.
Flags: "wifi (N)" counts the lines that mention WiFi status; tools leave those files unopened. sha256 (12): the
first 12 hex characters of the file's sha256.

| Date | Folder | File | Format | Firmware | Context | Flags | sha256 (12) |
|---|---|---|---|---|---|---|---|
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-CYC-0020-overwritten-by-pack7.txt` | text | `6d9bb7d` (flash record) | Pack #7 on port 4 overwrote the old CYC-0020 registry record (`eb7c9ff`) | - | `e688b92720d7` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-Okai_CHRG_052-port4-pack7.csv` | CSV, display charge log | `6d9bb7d` (flash record) | Charge log `Okai_CHRG_052`, pack #7 on port 4 (`eb7c9ff`) | - | `653be9e3208d` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-first-boot-6d9bb7d.log` | serial console log | `6d9bb7d` (named in the file) | First boot after flashing `6d9bb7d` at the bench (`8da5ba4`). Pack_Registry.md cites it | wifi (80) | `8f0d060651a9` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-keepalive-under-wifi-load.log` | serial console log | `6d9bb7d` (flash record) | Keep-alive run under WiFi load (`2c0b0d2`) | wifi (80) | `1dad3cab6f6b` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-packs-registry-dump.txt` | text, `/packs` registry dump | `6d9bb7d` (named in the file) | Pack registry dump (`0f4deb9`) | - | `18f82008c293` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-rawdump-3packs.txt` | text, raw frame dump | `6d9bb7d` (flash record) | Raw dump of 3 packs (`2c0b0d2`) | - | `70fb913b8a3b` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-rawdump-4packs-port4-pack2.txt` | text, raw frame dump | `6d9bb7d` (flash record) | Raw dump x3 with pack #2 added on port 4: cycle count 40, new record CYC-40 (`713028b`) | - | `b87eead690a1` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-rawdump-4packs-port4-pack7.txt` | text, raw frame dump | `6d9bb7d` (flash record) | Raw dump x3 with pack #7 on port 4: CYC-20 identified (`8c69277`) | - | `b084ac13f271` |
| 2026-10-05 | `logs/Bench/2026/10` | `2026-10-05-second-boot-6d9bb7d.log` | serial console log | `6d9bb7d` (named in the file name) | Second boot capture (`0f4deb9`) | wifi (31) | `f8f2f4b5a80e` |
| 2026-10-09 | `logs/Bench/2026/10` | `2026-10-09-port-swap-pack6-p2-to-p3.txt` | text | `0.3.3-0ec21e1` (named in the file) | Port-swap test: pack #6 keeps its bytes [19] and [20] from port 2 to port 3 (`415ee32`) | wifi (10) | `72fcdc0634fa` |

## Adding a log

Put it in `logs/<Category>/YYYY/MM/` under a name that starts with its date, check `git check-ignore -v <path>`,
and add its row here in the same commit. Never `git add -f` anything under `logs/`.
