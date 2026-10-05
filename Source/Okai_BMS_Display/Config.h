#pragma once

// ─── Firmware version ────────────────────────────────────────────────────────
#define FW_VERSION "0.3.0"

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
// Two streams: R_YYYYMMDD_NNN_S.csv (ride) and C_YYYYMMDD_NNN_S.csv (charge)
// NNN = session counter (000-999, wraps), S = segment within session (1,2,3…)
// Falls back to R_SXXX_S.csv / C_SXXX_S.csv when RTC has no time set
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
#define LABEL_SEED_LIST      { 1, 4, 6, 0 }   // ports 1-4; 0 = do not seed this port

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

// ─── Cross-file function prototypes (PackRegistry.ino) ───────────────────────
extern PackRecord packRec[NUM_PACKS];
void packRegistryInit();
void packRegistryIdentify(uint8_t port);
void packRegistryRegister(uint8_t port);
void packRegistrySessionUpdate(uint8_t port);
void packRegistrySetLabel(uint8_t port, uint8_t label);  // manual override, pack-bound
uint8_t packRegistryNumber(uint8_t port);                // override ?: autoNum, 0 = unknown
const PackRecord* packRegGet(uint8_t port);
