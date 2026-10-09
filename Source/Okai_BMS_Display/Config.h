#pragma once

// ─── Firmware version ────────────────────────────────────────────────────────
// 2026-10-09 - bumped only AFTER 0.3.3 was already flashed, because I forgot it: the boot
// banner on the flashed board reads "0.3.0" while running 0.3.3. Harmless but misleading, and
// the only thing that caught it was Bremote3 reading the banner on a bench capture. The
// hashes and the new behaviour are what prove which build is on the board; this string proves
// nothing until it is maintained. Bump it in the SAME commit as any behaviour change.
#define FW_VERSION "0.3.3"

// ─── Pack count ──────────────────────────────────────────────────────────────
#define NUM_PACKS 4

// ─── UART / SoftSerial pin assignments ───────────────────────────────────────
// T-Display-S3 free GPIOs: 1, 2, 10, 11, 12, 13, 16, 17, 18, 21
// Display parallel bus:    5,6,7,8,9,39,40,41,42,45,46,47,48
// Avoid: 0(BOOT), 3,4(ADC), 35-37(PSRAM), 43(TX0), 44(RX0)

#define PACK1_RX_PIN   1
#define PACK1_TX_PIN   2   // shared TX bus — wire to ALL pack RX lines
#define PACK2_RX_PIN  16
#define PACK2_TX_PIN  PACK1_TX_PIN
#define PACK3_RX_PIN  17
#define PACK3_TX_PIN  PACK1_TX_PIN
#define PACK4_RX_PIN  18
#define PACK4_TX_PIN  PACK1_TX_PIN
#define HEARTBEAT_TX_PIN  PACK1_TX_PIN

// ─── RTC (DS3231) ────────────────────────────────────────────────────────────
// Connect DS3231 module: VCC→3V3, GND→GND, SDA→GPIO11, SCL→GPIO12
// Library: "RTClib" by Adafruit — install via Library Manager
#define RTC_SDA_PIN  11
#define RTC_SCL_PIN  12

// ─── UART baud ───────────────────────────────────────────────────────────────
#define BMS_BAUD 9600

// ─── Heartbeat ───────────────────────────────────────────────────────────────
// Official jsutcliff/OKAI-Battery-Lib sends the unlock every 1 s; the pack only
// tolerates up to 5 s before it stops outputting. 5000 sat exactly on that limit
// and got starved by the cooperative loop → packs dropped. Back to the proven 1 s.
#define HEARTBEAT_INTERVAL_MS 1000UL

// ─── Smart logging ───────────────────────────────────────────────────────────
// Two streams: Okai_RIDE_YYYYMMDD_NNN_S.csv and Okai_CHRG_YYYYMMDD_NNN_S.csv
// NNN = session counter, S = segment within session (1,2,3…)
// Falls back to Okai_RIDE_SNNN_S.csv / Okai_CHRG_SNNN_S.csv when RTC has no time set
//
// 2026-10-05 - The counter CLAMPS, it does not wrap; this comment used to say "wraps"
// and was wrong. The cap was 999, which audit finding F-3 showed breaks two things at
// once: every session-999 file looks like the session in progress (so the free-space
// guard treats the whole stream as unprunable), and two session-999 rides on one day
// produce the same filename, which `openFile()` opens "w" — truncating the earlier
// session to a header. Raised to 9999. The format is "%03u", a MINIMUM width, so four
// digits print in full and no filename is truncated.
#define LOG_SESSION_MAX  9999
#define LOG_RIDE_INTERVAL_MS    5000UL    // 5 s while riding
#define LOG_CHARGE_INTERVAL_MS  30000UL   // 30 s while charging
// 2026-07-26 - RAISED 1.0 -> 2.0 A. A pack sitting on the cheap scooter charger
// in its balancing phase was observed drawing ~1 A, landing exactly on the old
// threshold and able to open a spurious RIDE log. Real rides pull 10-25 A per
// pack, so 2 A keeps a wide margin on both sides.
#define LOG_RIDE_THRESHOLD_A    2.0f      // A discharge → "riding"
#define LOG_RIDE_HYSTERESIS_MS  120000UL  // keep RIDE open 2 min after current drops
#define LOG_MAX_FILE_BYTES      262144UL  // 256 KB per segment → roll to _2, _3…

// 2026-10-05 - FREE-SPACE GUARD (auto-delete-oldest), owner's choice over a
// stop-logging-with-a-warning design.
//
// WHAT WAS WRONG: nothing watched free space. LittleFS on the 9 MB ffat slot holds
// roughly 13-14 three-pack outings at FMT 3 row sizes, and when it fills, writes
// simply start failing — the newest session, the one being recorded, is the one lost.
// Silently. That is backwards: the oldest session is always the least valuable.
//
// THE RULE: before opening any log file (new session OR segment roll), if free space
// is under LOG_FS_RESERVE_BYTES, delete the oldest log files until it is not.
//
// AGE IS DECIDED BY SESSION NUMBER, NOT BY THE CLOCK. The session counters in
// /sessions.bin are monotonic and persist across reboots, and both filename forms
// carry them (Okai_RIDE_20260726_057_1.csv and Okai_RIDE_S057_1.csv), so the lowest
// session number is the oldest session whether or not time was ever synced. Sorting
// by name or by mtime would have put the no-clock files in an arbitrary place.
//
// SAFETY: only /Okai_{RIDE,CHRG}_*.csv in the root are ever candidates — the registry
// (/packs/), /labels.bin, /labelseed.bin, /timesync.bin and /sessions.bin are not
// touched. The file being written and anything from the CURRENT session are excluded.
// At most LOG_PRUNE_MAX_FILES deletions per open, so one call cannot stall the loop
// for long; the next open picks up where it left off if that was not enough.
#define LOG_FS_RESERVE_BYTES   524288UL  // keep 512 KB free — 2 full segments of room
#define LOG_PRUNE_MAX_FILES    3         // max deletions in a single openFile() call
// 2026-10-05 - Floor per audit finding F-4: never thin either stream below this many
// files. Balancing by file count is deliberate and kept — rides are 5 s rows and are
// most of the bytes, so thinning the larger stream is what frees space — but without a
// floor a long run of one kind of session could evict the other stream entirely.
#define LOG_PRUNE_KEEP_MIN     2         // always keep this many newest files per stream

// ─── Pack labels ─────────────────────────────────────────────────────────────
// Labels 1-8; 0 = unassigned (shows as P1/P2/P3/P4 in filenames)
#define NUM_LABELS 8

// 2026-10-05 - ONE-SHOT LABEL SEED.
//
// WHY THIS EXISTS: the only way to attach a number to a pack was the BTN3-hold
// picker, and the owner never used it — it was a 6x8 font inside a sealed box. The
// physical packs carry printed white labels, and on 2026-10-05 the owner read them
// off the batteries sitting in the ports:
//
//     port 1 -> label 1      port 2 -> label 4      port 3 -> label 6
//
// So the firmware seeds those numbers itself, with no button presses. A seeded
// number is written into that PACK's registry record (/packs/CYC-XXXX.dat), so from
// then on it follows the battery into whatever port it is plugged into. The port
// mapping below is only how the packs are introduced once.
//
// HOW IT CAN NEVER MISFIRE — three independent gates:
//   1. VERSION. /labelseed.bin stores the version that has already been applied. The
//      seed is live only on the FIRST boot after a flash that raises the version.
//      Every later boot reads the file and stands down, so a reboot with different
//      packs in those ports cannot relabel them.
//   2. WINDOW. Within that one boot, only ports that come alive in the first
//      LABEL_SEED_WINDOW_MS are seeded — i.e. packs already plugged in at power-on,
//      which is the bench arrangement the owner just read the labels off.
//   3. AMBIGUITY. A port whose pack the registry cannot identify with confidence is
//      NOT seeded; the owner is asked instead. Seeding an ambiguous pack would create
//      a duplicate record, which is the exact mis-filing the prompt exists to stop.
//
// TO RE-SEED after moving packs around: raise LABEL_SEED_VERSION, edit the table,
// flash. Set an entry to 0 to leave that port alone.
#define LABEL_SEED_VERSION   1
#define LABEL_SEED_WINDOW_MS 120000UL   // seed only packs present in the first 2 min

// 2026-10-09 - SEED DISARMED, all four entries zero. Two independent reasons, and the
// second one is why zeroing beats trusting the gates:
//
//   1. THE TABLE IS KNOWN WRONG. It was written from the owner's visual read on
//      2026-10-05. On 2026-10-06 he read the same physical packs again and got
//      port 2 = #6 (not #4) and port 4 = #4 (not #6). The 45-cycle pack never moved
//      between those two days — only the reading changed. So one of the two reads is a
//      human error, no firmware gate can tell which, and the registry is about to be
//      wiped and rebuilt BY HAND from the white markers. The seed has no job left.
//
//   2. A FORMAT RE-ARMS IT, AND THEN ALL FOUR GATES FAIL AT ONCE (audit R-15 / F-8).
//      /labelseed.bin lives INSIDE the filesystem a format destroys, so a format-on-mount
//      wipes the version marker and the seed goes live again. In that same moment /packs
//      is empty, so every pack registers fresh with _matchDiff == 0 — which EXEMPTS
//      Gate 4 by construction. Gates 1 and 3 are gone with the file and the empty
//      registry. The one case where the seed could do the most damage is the one case
//      where nothing stops it. Zero entries make that path inert without relying on any
//      gate holding.
//
// TO RE-SEED after moving packs around: raise LABEL_SEED_VERSION, edit the table, flash.
// Set an entry to 0 to leave that port alone. All zeros = the seed never writes anything.
#define LABEL_SEED_LIST      { 0, 0, 0, 0 }   // ports 1-4; 0 = do not seed this port

// 2026-10-05 - GATE 4, added after audit finding F-1. The three gates above stop the
// seed REPEATING; none of them stopped it being CONFIDENTLY WRONG.
//
// WHAT WAS WRONG: the seed trusted `packRec[port].known`, and identification sets that
// for the nearest record within PACK_CYC_MATCH_WINDOW whenever no runner-up is within
// PACK_CYC_AMBIGUOUS. A battery whose own record is missing — deleted, or never written
// because its first sighting was ambiguous — but which sits within 8 cycles of ANOTHER
// pack's last-seen count is matched with no competition and therefore "confidently".
// The seed would then write the owner's printed label into the wrong battery's file.
// That is silent, permanent, survives reboots, and accrues wear history under the wrong
// identity. The ambiguity gate only ever catches TWO records competing, never ONE wrong
// record standing alone.
//
// This fleet's registration counts are 10-17 cycles apart, which looks safe, but
// last-seen counts CONVERGE: a record left at 8229 that charges on to 8240 is then only
// 6 cycles from the 8246 pack. The risk grows with use.
//
// SO: seeding additionally requires a TIGHT match — at most this much forward drift AND
// an exact maxSoc agreement, or a brand-new registration (which cannot be wrong, the
// record having just been created for the pack in front of us). A port that fails is
// left unseeded and says so on serial; fix it with the picker, or plug the right pack in
// and bump LABEL_SEED_VERSION.
#define PACK_SEED_MAX_DRIFT  3   // max cycles since last seen that still allows a seed

// 2026-10-06 - Minimum capacity the charge estimator will believe. A 10S4P Okai pack
// measures ~12.7 Ah (0.854 A for 6% SOC over 53 min on 2026-10-06), which matches frame
// byte [20] = 64 under the x200 mAh decode. But this fleet reports 0, 0, 4, 4 - 0 mAh or
// 800 mAh - so the field is not reliably capacity. Below this floor the estimator ignores
// it and uses the SOC slope alone, which is the sound signal anyway.
#define PACK_CAPACITY_MIN_MAH 5000

// 2026-10-06 - CHARGER CONNECTED BUT NOTHING HAPPENING.
// Pack #1 sat with a charger detected and +0.000 A for TWO HOURS AND TWELVE MINUTES on
// 2026-10-06 (log 065, 814 rows of nothing) and the firmware never said a word. The status
// bits named the fault the whole time: charger detected set, charger-OK clear, charge FET
// never closing. Five minutes of that is diagnosable; two hours is wasted.
#define CHG_STALLED_WARN_MS  300000UL   // charger present, no current, this long -> warn

// ─── Cell health thresholds ──────────────────────────────────────────────────
// 2026-07-26 - RAISED, and now only ever applied to a pack AT REST.
//
// WHAT WAS WRONG: the old 50/100 mV limits were evaluated on every sample,
// including under load. Cell spread inflates 2-5x while current flows (internal
// resistance differences) and again near full charge (the voltage curve steepens),
// so the numbers being judged were not health numbers at all.
//   Measured proof, pack #3 - the healthiest pack in the fleet:
//     -9.70 A -> 160 mV -> flagged DELTA_POOR
//     -0.07 A ->  45 mV -> fine, 30 seconds later
// Raising the limit alone would NOT have fixed that; 160 mV still trips. The
// gate on current is the actual fix. See CELL_REST_CURRENT_A below.
//
// Values: healthy Li-ion rests at 10-30 mV, normal service life reaches ~50 mV,
// commercial BMS alarm points sit at 100-200 mV, and a genuinely bad cell shows
// >200 mV AT REST or a spread that grows every cycle. Owner-approved 2026-07-26.
#define CELL_DELTA_WARN_V  0.100f   // 100 mV — was 50 mV
#define CELL_DELTA_POOR_V  0.180f   // 180 mV — was 100 mV

// Pack counts as "at rest" below this current. Health is judged ONLY on rest
// samples; under load the last rest verdict is held instead of recomputed.
#define CELL_REST_CURRENT_A 1.0f

// ─── Pack registry ───────────────────────────────────────────────────────────
#define PACK_HISTORY_LEN    10   // rolling spread/SoH entries stored per pack
// ─── Pack identification ─────────────────────────────────────────────────────
// The Okai BMS reports NO serial number and NO MAC. Cycle count is the only stable
// per-pack value, so it is the fingerprint — with the known weakness that two packs
// at similar cycle counts are hard to tell apart.
//
// 2026-10 - RETUNED, and the old value was actively wrong for this fleet. The owner's
// packs sit at 37 · 54 · 65 · 78 · 8229 · 8246 · 8256 cycles — gaps of 10 to 17. The
// previous tolerance of 500 was far wider than the spacing between packs, and the scan
// took the FIRST record inside that window rather than the closest, so a pack reading
// 8246 could be filed under the 8229 record. Identity was being decided by directory
// order. Three changes fix it: match on last-seen cycles instead of registration
// cycles, take the NEAREST candidate, and refuse to guess when two are too close.
#define PACK_CYC_MATCH_WINDOW 8  // max forward cycle drift that still counts as the same pack
#define PACK_CYC_AMBIGUOUS    3  // two candidates this close → do not guess, ask instead
#define PACK_SOC_MATCH_SLACK  3  // maxSoc (SoH) may drift this many % and still match

// ─── Display ─────────────────────────────────────────────────────────────────
#define TFT_BL_PIN      38
#define DISPLAY_REFRESH_MS  500UL

// ─── Onboard battery sense (LilyGo T-Display-S3 18650) ───────────────────────
// GPIO4 = onboard battery ADC, fed through a 2:1 divider (two equal resistors).
// Vbat = analogReadMilliVolts(4) × 2. Single Li-ion range: 3.0 V empty → 4.2 V full.
// GPIO4 is reserved-for-ADC in the pin map above; nothing else uses it.
#define BAT_ADC_PIN     4

// ─── TFT parallel bus (T-Display-S3 ST7789 8-bit) ────────────────────────────
#define TFT_DC   7
#define TFT_CS   6
#define TFT_WR   8
#define TFT_RD   9
#define TFT_RST  5
#define TFT_D0  39
#define TFT_D1  40
#define TFT_D2  41
#define TFT_D3  42
#define TFT_D4  45
#define TFT_D5  46
#define TFT_D6  47
#define TFT_D7  48

// ─── Buttons ─────────────────────────────────────────────────────────────────
#define BUTTON1_PIN      0   // BTN1: WiFi toggle / confirm (short) | sleep (4s hold)
#define BUTTON2_PIN     14   // BTN2: next screen →
#define BUTTON3_PIN     21   // BTN3: prev screen ←  (long-press = label assign)

// ─── Power management ─────────────────────────────────────────────────────────
#define POWER_EN_PIN    15       // LiPo power latch — must stay HIGH while running
#define SLEEP_HOLD_MS   4000UL   // hold BTN1 this long → deep sleep; press BTN1 to wake

// ─── Light FET output (LEGACY — RETIRED) ─────────────────────────────────────
// ⚠ GPIO13 is now NeoPixel status strip 2 (see below). The flag-light MOSFET is
// deprecated and NO LONGER DRIVEN by firmware — the display must not touch GPIO13
// or it fights the strip's RMT signal. Do not wire a light to GPIO13 anymore.
#define LIGHT_FET_PIN  13        // (legacy) superseded by LED2_PIN — not driven

// ─── Status LED bars (NeoPixel WS2812) — 2× 12 px, one data pin each ─────────
// Layout per strip: pack A [px0-4] | gap [px5-6 dark] | pack B [px7-10] | KA [px11].
// px11 on BOTH strips is the keep-alive indicator (white slow-blink = healthy,
// solid red = watchdog-stalled). Low brightness; 3V3 + GND.
#define LED1_PIN  10             // NeoPixel strip 1 (packs 1+2)
#define LED2_PIN  13             // NeoPixel strip 2 (packs 3+4) — takes GPIO13 from retired FET

// ─── Keep-alive watchdog / edge-pixel indicator ─────────────────────────────
// The LED indicator reads g_hbLastMs (published by the Core-1 heartbeat task each
// beat). If no beat within HB_WATCHDOG_MS → indicator goes SOLID RED. 2500 ms is
// > the 1 s cadence + jitter yet well inside the pack's 5 s sleep deadline.
#define HB_WATCHDOG_MS  2500UL
extern volatile uint32_t g_hbLastMs;   // Heartbeat.ino — last keep-alive beat, millis()

// ─── Dynamic home screen ─────────────────────────────────────────────────────
// HOME IS THE DEFAULT, ALWAYS. Nothing may hold the display off Home for longer
// than HOME_IDLE_MS, and no overlay may cover it for longer than OVERLAY_TIMEOUT_MS.
// Enforced unconditionally by enforceHomePolicy() at the TOP of displayLoop(), above
// every early return — see Display.ino. Charging is NOT an exemption.
#define HOME_IDLE_MS       30000UL     // 30 s with no button input → auto-return to Home
#define OVERLAY_TIMEOUT_MS 15000UL     // 15 s with no button input → any overlay self-dismisses
#define PACK_CONNECTED_MS  5000UL      // no fresh frame in this long → pack is dead/disconnected

// ─── Runtime ("time remaining") estimator ────────────────────────────────────
// Displayed "~X min" = the WORST connected pack (soonest to hit the reserve). Each
// pack independently feeds a motor, so a mean would over-promise. Method: per-pack
// SOC-decline slope (primary) blended with per-pack current/power (cross-check).
#define RUNTIME_SAMPLE_MS    10000UL   // per-pack SOC ring sample cadence
#define RUNTIME_WINDOW_MS    150000UL  // 2.5 min trailing window for the decline slope
#define RUNTIME_MIN_SPAN_MS  45000UL   // need >=45 s of data before showing a number
#define RUNTIME_RESERVE_PCT  15        // per-pack stop-riding reserve (extrapolate to here)

// ─── Time-to-full estimator (charging) ───────────────────────────────────────
// Charging is an order of magnitude slower than discharging, so it CANNOT share the
// discharge ring. The owner's packs take roughly 4.5 h for a full charge — about
// 0.37 % SOC per minute. The discharge window is 20 samples x 10 s = 200 s, which over
// that charge sees barely 1 % of movement, and SOC is reported at 1 % resolution. The
// "slope" would be pure quantisation noise and the estimate would jump between absurd
// values. Charging gets its own slow ring instead: 16 samples a minute apart = a 16 min
// window, which sees 5-6 % of real movement and yields a stable number.
#define CHG_SAMPLE_MS     60000UL      // SOC ring cadence while charging (1 min)
#define CHG_WINDOW_MS     960000UL     // keep 16 min of history
#define CHG_MIN_SPAN_MS   300000UL     // need >=5 min of data before quoting a time
#define CHG_RING_LEN      16

// ─── WiFi AP ─────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID     "OkaiBMS"
#define WIFI_AP_PASSWORD "12345678"

// ─── Station mode (join the home network) ────────────────────────────────────
// 2026-10-06 - WHY THIS EXISTS, in the owner's words: "I'm tired of doing that.
// This should be on the network so you can actually go in and check it while it's
// charging, instead of having to connect to it via AP."
//
// He is right, and the cost of not having it was concrete: pack #1's charge state was
// diagnosed from a 15-hour-old dump because that was the only data available, and the
// diagnosis was wrong. Live logs are not a convenience, they are what stops stale
// analysis.
//
// 🔴 CREDENTIALS ARE NEVER STORED HERE. The AP password above sits in source control
// already, which is tolerable for a throwaway bench AP and absolutely not for a home
// network. Station credentials live in NVS only, entered once through the /wifi page on
// the AP. Nothing to commit, nothing to leak, and they survive a firmware flash.
//
// 🔴 NEVER WHILE RIDING. Per [[SOP-038]] the keep-alive outranks every feature here, and
// riding is the one time a dropout matters. Charging is stationary, mains-powered and off
// the water, so auto-join is gated to CHARGE sessions only. Evidence this is safe: four
// WiFi-load windows (AP up, two clients, ~250 HTTP requests, one sustained 15 min) all
// held the keep-alive at a worst gap of 1026 ms against a 5000 ms deadline, with no
// out-of-memory reboot. Station mode is a different load profile, so it still needs its
// own g_hbMaxGap proof before it is trusted.
// Ported from foilIQ's proven WifiXfer pattern. SEQUENTIAL, never AP+STA concurrently:
// APSTA allocates both control blocks, and Espressif put station mode alone at ~45 kB of
// heap with APSTA "a lot of RAM". An OOM reboot stops the keep-alive, so it is the real risk.
//
// 🔴 2026-10-09 - MY STATED REASON FOR THIS WAS PROBABLY WRONG. This comment used to read
// "No PSRAM here and a ~106 kB framebuffer already in internal DRAM". The board definition
// says otherwise: `lilygo_t_display_s3.build.psram_type=opi` with
// `memory_type={build.boot}_{build.psram_type}` and **no PSRAM menu option at all**, so OPI
// PSRAM is compiled in unconditionally for this board. The ROM banner on the bench confirms
// the silicon: "Embedded PSRAM 8MB (AP_3v3)". Display.ino:24 has said "framebuffer in PSRAM"
// all along - the two comments contradicted each other and I propagated the wrong one into
// an architectural justification.
//
// Free heap reading ~221 kB rather than megabytes means PSRAM is NOT merged into the default
// heap, so it is reachable via ps_malloc / MALLOC_CAP_SPIRAM only - which is exactly how
// Arduino_Canvas allocates. So the framebuffer is very likely NOT in internal DRAM.
//
// THE DECISION STANDS, on a different and measured basis: with the AP up, internal free heap
// fell to 167,528 B (bench, 2026-10-09) and APSTA's cost is unmeasured on this board. Staying
// sequential is justified by that headroom and by foilIQ's field-proven pattern - not by a
// PSRAM claim I had not checked. `psram=` and `iram=` are now on every [DIAG] line so this
// is never argued from a comment again.
#define WIFI_MAX_NETS         4        // a LIST of networks; WiFiMulti joins the strongest
#define WIFI_STA_CONNECT_MS   15000UL  // scan+join budget before falling back to AP
#define WIFI_STA_RETRY_MS     60000UL  // after a drop or failure, do not hammer the router
#define WIFI_ON_WINDOW_MS     60000UL  // owner's spec: 60 s reachable after every power-on
#define WIFI_MDNS_NAME        "okai"   // http://okai.local, and the router learns "okai"
// Boot-join gate (M-4). EARLIEST: enough time for several 1 Hz pack frames to land, so
// bootJoinSafe() is judging real telemetry rather than an empty table. DEADLINE: if the
// evidence never arrives - no packs plugged in, or a pack discharging - the join is
// abandoned, not attempted blind. The charge-start retry picks it up at the dock.
#define BOOT_JOIN_EARLIEST_MS 5000UL
#define BOOT_JOIN_DEADLINE_MS 30000UL
// Smallest clock correction worth a flash erase + a DS3231 write. An open dashboard tab
// re-syncs every 5 s; 30 s keeps CSV timestamps honest while reducing that to ~nothing.
#define TIME_RESYNC_MIN_DRIFT_SEC 30
// Charger bit must hold this long before a join is INITIATED, so a chattering bit cannot
// trigger a 15 s blocking scan. Holding an existing link up needs no dwell.
#define CHG_WIFI_DWELL_MS     30000UL
// A station link must stay down this long before the radio is torn down. A single missed
// beacon used to force a full deinit/re-init plus a 60 s wait — ~60 cycles/hour at the
// edge of coverage, which is both pointless churn and the heap-leak path R-11 is about.
#define STA_LOST_DEBOUNCE_MS  3000UL
// An associated AP client only DEFERS the home-network recovery while it is actually being
// used. The owner's phone auto-joins "OkaiBMS" (predictable name, his decision), and an
// idle phone in a pocket must not block the recovery for a whole power cycle.
//
// 2026-10-09 - 90 s → 300 s, AND the justification I first wrote for 90 s was WRONG. It
// claimed "the registry rebuild serves requests constantly (the dashboard self-refreshes
// every 5 s and /rawdump every 3 s)". True of `/` and `/rawdump` — but the rebuild runs on
// **`/packs`, which has no self-refresh at all**, and neither does `/wifi`. On those pages
// requests arrive only when he taps. Plug a pack in, read the white marker, fetch the next
// one out of a bag: over 90 s between taps is completely ordinary, and the recovery would
// then have pulled the AP out from under live bench work.
//
// The error is asymmetric, so err long: too long merely delays a convenience recovery for
// an idle phone; too short interrupts the job. 5 minutes of total HTTP silence means
// nobody is working.
#define AP_IDLE_RECOVER_MS    300000UL

// ─── Pack energy design specs (Panasonic NCR18650BD 10S4P) ───────────────────
#define PACK_DESIGN_AH   12.8f    // 4P × 3.2 Ah rated
#define PACK_NOMINAL_V   36.0f    // 10S × 3.6 V nominal
#define PACK_DESIGN_WH   460.8f   // PACK_DESIGN_AH × PACK_NOMINAL_V

// ─── Log mode (shared between Logger.ino and Display.ino) ────────────────────
typedef enum : uint8_t { LOG_IDLE = 0, LOG_RIDE = 1, LOG_CHARGE = 2 } LogMode;

// ─── Per-pack data ───────────────────────────────────────────────────────────
struct PackData {
    float    voltage;
    float    current;
    float    cellHigh;
    float    cellLow;
    uint8_t  soc;
    uint8_t  maxSoc;           // max achievable SOC / SoH indicator (b[06])
    uint8_t  maxTemp;
    uint16_t cycles;
    // Rated pack capacity, frame byte [20] x 200 mAh (0x40 = 12800 mAh). Decoded by
    // OkaiBMS::ratedCapacity_mAh() since the library was written but never surfaced.
    // Needed as the cross-check for time-to-full while charging.
    uint16_t capacityMah;
    // 2026-10 - Health telemetry the library has always decoded and nothing ever read.
    // Only tempCellMax() reached PackData; the other three sensors, the charger state
    // byte and the charger-active flag were all dropped on the floor. TEMP_FET is the
    // interesting one for pack health — thermal stress on the discharge FET is where a
    // tiring pack shows itself first. All of these now reach the CSV.
    uint8_t  tempAvg;          // [b08] average cell temp
    uint8_t  tempFet;          // [b09] discharge FET temp
    uint8_t  tempMcu;          // [b10] BMS MCU temp
    uint8_t  chargerStateRaw;  // [b13] 0x00 none / 0x19 begin / 0x7C bulk
    bool     chargerActive;    // [b17] == 0x04
    uint8_t  rawStatus;
    bool     chargerDetected;
    bool     isCharging;      // bulk-charge phase active (status bit 5)
    bool     chargeDone;      // charger on, bulk finished, current has stopped
    // 2026-07-26 - the state between the two: bulk is over but the BMS is still
    // trickling current to let the passive balancer bleed the high cells. Used to
    // be lumped in with "Charging..." which read as wrong at 40 mA.
    bool     isBalancing;
    float    whIn;           // session Wh accumulated (charging)
    float    whOut;          // session Wh accumulated (discharging)
    bool     valid;
    uint32_t lastUpdateMs;
    // 2026-07-26 - Rest-gated cell health. Spread is only a health signal when no
    // current is moving; under load it measures internal resistance instead. The
    // last rest reading is held and reused so the verdict stays stable mid-ride
    // rather than flickering POOR on every throttle punch.
    bool     atRest;         // |current| < CELL_REST_CURRENT_A this sample
    bool     haveRestDelta;  // a rest sample has been seen since this pack appeared
    float    restDelta;      // cellHigh - cellLow, last measured AT REST (volts)
};

// ─── Per-pack registry record ─────────────────────────────────────────────────
struct PackRecord {
    // 2026-10-06 - THE RECORD CARRIES ITS OWN PATH. Previously the filename was derived
    // from regCYC alone and opened "w", so registering a pack whose CURRENT cycle count
    // equalled some older record's REGISTRATION count silently truncated that record.
    // It destroyed CYC-0020 (17 sessions, 10 history entries) on 2026-10-05 when pack #7
    // went on port 4 reading 20 cycles, and fired again on 2026-10-06 overwriting
    // CYC-0055. Twice in two days, no error logged either time.
    //
    // Derivation cannot be made safe — two packs legitimately share a cycle count (ports 1
    // and 4 both read 56 right now). So registration allocates a filename that does not
    // exist yet and the record remembers it. Empty means "derive it", which is what every
    // pre-2026-10-06 record on disk will do when first loaded.
    char     file[28];                         // "/packs/CYC-0055_2.dat" + NUL
    uint16_t regCYC;                           // cycle count at registration (UUID)
    uint8_t  maxSocAtReg;                      // maxSoc at registration (tiebreaker)
    // ── Identity shown to the human ──────────────────────────────────────────
    // autoNum is assigned ONCE at registration (first free 1..NUM_LABELS, in
    // first-seen order) and never changes. It is bound to the PHYSICAL PACK, not
    // the port, so it follows the battery into whatever port it is plugged into.
    // No user action is required — this is the default identity.
    // label is an OPTIONAL manual override: 0 = "use autoNum", 1..NUM_LABELS = this
    // number instead. Set from the label picker; also bound to the pack.
    uint8_t  autoNum;                          // 1..NUM_LABELS, 0 = not yet assigned
    uint8_t  label;                            // 0 = use autoNum, else manual override
    // True when two stored packs were too close in cycle count to tell apart at
    // plug-in. The device then shows the PORT tag, attributes no history, and asks
    // the owner to confirm once — rather than silently filing one pack's wear
    // under another, which is the failure that matters when watching a weak pack.
    bool     ambiguous;
    char     firstSeen[12];                    // "YYYY-MM-DD\0"
    uint16_t currentCycles;
    uint16_t sessions;
    float    totalWhCharged;
    float    totalWhDischarged;
    uint16_t spreadHistory[PACK_HISTORY_LEN];  // cell spread mV per session
    uint8_t  sohHistory[PACK_HISTORY_LEN];     // maxSoc % per session
    uint8_t  historyLen;
    bool     known;
    char     cycID[10];                        // "CYC-XXXX\0"
};

// ─── Charge-state threshold ───────────────────────────────────────────────────
// 2026-07-26 - Current at or above this counts as "still flowing", which is what
// separates Balancing from Charge complete.
//
// RAISED 0.010 -> 0.150 A. The 10 mA figure was wrong: it assumed the current
// reading is trustworthy near zero. It is not. With the charge FET confirmed OFF
// (status 0x02) and packs genuinely at rest, the RIDE logs of 2026-07-25 show
//   pack #5  +0.19 A      pack #4  -0.25 A      pack #3  -0.07 A
// i.e. a sensor offset band of roughly +/-0.2 A. A 10 mA test sits deep inside
// that band, so "still flowing" would read true forever and Charge complete
// could never appear. 150 mA clears the offset while staying well under the
// ~1 A the cheap charger pushes during its balancing phase.
static const float kBalanceCurrentA = 0.150f;

// ─── Cell health verdict (single source of truth) ─────────────────────────────
// 2026-07-26 - Every consumer (screen, LEDs, web dashboard, CSV) must ask these
// two helpers rather than comparing a live delta against the thresholds itself.
// That is what guarantees the rest-gating actually holds everywhere.

// The delta to JUDGE: the last rest reading if we have one, else the live value.
// Before any rest sample exists the live value is all there is, but a pack sitting
// on the bench reaches rest within a second or two, so this is a brief window.
static inline float healthDelta(const PackData &p) {
    return p.haveRestDelta ? p.restDelta : (p.cellHigh - p.cellLow);
}

// "GOOD" | "WARN" | "POOR"
static inline const char *healthTag(const PackData &p) {
    const float d = healthDelta(p);
    if (d >= CELL_DELTA_POOR_V) return "POOR";
    if (d >= CELL_DELTA_WARN_V) return "WARN";
    return "GOOD";
}

// ─── Current formatting (shared by the display and the web dashboard) ─────────
// 2026-07-26 - WHY THIS EXISTS: while balancing, the pack draws tens of milliamps.
// "%+.1fA" renders 0.041 A as "+0.0A", which reads as "nothing is happening" when
// something very much is. Printing "<0.0A" instead says honestly: there IS current,
// it is just below what this many decimals can show. No extra precision needed —
// for balancing the owner only needs to know it is flowing, not the exact figure.
// Kept here, in the header both files include, so the two surfaces cannot drift.
static inline void fmtAmps(char *buf, size_t n, float amps, uint8_t decimals, const char *suffix)
{
    // Smallest magnitude this many decimals can render as non-zero (0.05 at 1 dp).
    const float floorMag = 0.5f / powf(10.0f, (float)decimals);
    if (amps != 0.0f && fabsf(amps) < floorMag) {
        snprintf(buf, n, "<%.*f%s", (int)decimals, 0.0f, suffix);   // "<0.0A"
    } else {
        snprintf(buf, n, "%+.*f%s", (int)decimals, amps, suffix);   // "+1.5A"
    }
}

// ─── Cross-file globals ───────────────────────────────────────────────────────
extern PackData   packs[NUM_PACKS];    // UART.ino
extern float      g_ridePowerEma_W;    // UART.ino — EMA fleet discharge power
extern uint8_t    g_ridePowerN;        // UART.ino — warmup counter (< 3 = not ready)
extern bool     wifiActive;          // WiFiServer.ino
extern bool     fsReady;             // Logger.ino

// ─── Cross-file function prototypes (PackLabel.ino) ──────────────────────────
// needed by Logger.ino and Display.ino which compile before PackLabel.ino
void     packlabelInit();
uint8_t  labelGet(uint8_t port);
void     labelSet(uint8_t port, uint8_t label);
void     labelStr(uint8_t port, char *buf, size_t len);
bool     timeIsSynced();
bool     timeIsRestored();   // epoch came from flash, not a live browser sync
time_t   timeNowSec();
const char *loggerActiveFile();   // Logger.ino — file currently being written
void     timeSyncSet(int64_t browserEpochMs);
LogMode  logCurrentMode();           // Logger.ino — prototype for Display.ino

// ─── Cross-file function prototypes (RideWatch.ino) ──────────────────────────
// The SOP-038 interlocks. These read PACK TELEMETRY ONLY — never the logger — because
// the priority order is keep-alive > telemetry > display > logging > WiFi, and an
// interlock protecting the top of that list must not be derived from near the bottom.
// rideWatchUpdate() must be called every loop pass; the rest are pure reads.
void rideWatchUpdate();
bool rideSuspected();         // any pack discharging past threshold, within hysteresis
bool packsLoadedRecently();   // real current either way in the last 60 s — sleep interlock
bool chargeActive();          // charger attached AND current actually going in
bool bootJoinSafe();          // positive evidence no ride is running — boot-join gate
bool rideEverSeen();          // any discharge since boot — no-telemetry backstop
bool chargerPresent();        // charger BIT only — THIS is the WiFi gate, not chargeActive()
bool chargeJoinWorthy(uint32_t ms);    // bit + REAL current seen in this run + dwell
bool packFramesSeen();        // any valid fresh pack frame since boot

// ─── Cross-file function prototypes (PackRegistry.ino) ───────────────────────
extern PackRecord packRec[NUM_PACKS];
void packRegistryInit();
void packRegistryIdentify(uint8_t port);
void packRegistryRegister(uint8_t port);
void packRegistrySessionUpdate(uint8_t port);
void packRegistrySetLabel(uint8_t port, uint8_t label);  // manual override, pack-bound
uint8_t packRegistryNumber(uint8_t port);                // override ?: autoNum, 0 = unknown
const PackRecord* packRegGet(uint8_t port);
// 2026-10-06 - Stored-record admin, added because the registry had to be read by decoding
// raw flash with littlefs-python: no route or command could list records for packs that
// were not plugged in. The rebuild needs a real delete, not a flash write-back.
uint8_t packRegistryList(char* out, size_t outLen, const char* csrf);  // <tr> rows; returns count
bool    packRegistryForget(const char* file);            // delete ONE stored record
bool    packRegistrySetLabelByFile(const char* file, uint8_t label);
uint8_t packRegistryForgetAll(void);                     // wipe /packs, returns files removed
uint8_t packRegistryCount(void);                         // records stored now — proves a wipe finished
