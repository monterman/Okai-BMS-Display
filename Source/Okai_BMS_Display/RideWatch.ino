// RideWatch.ino — the SOP-038 interlocks, and nothing else.
//
// ─── WHY THIS FILE EXISTS (2026-10-09) ───────────────────────────────────────
// The "never raise WiFi while riding" rule was wired to logCurrentMode() — i.e. to the
// LOGGING subsystem. SOP-038's priority order is:
//
//     keep-alive > telemetry > display > logging > WiFi
//
// so the interlock protecting the highest priority was derived from the second LOWEST.
// That inversion produced real failures, all found in the 2026-10-09 audit:
//
//   * loggerLoop() early-returns on !fsReady ABOVE mode detection, so a mount failure
//     freezes the mode at IDLE for the whole power cycle. Riding never becomes true and
//     the radio stays up for every ride.
//   * detectMode() returns LOG_CHARGE on the chargerDetected BIT ALONE, before it tests
//     for a ride. Pack #1's documented fault is exactly that — charger detected,
//     +0.000 A, for 2 h 12 min — so that one pack made a whole ride report as CHARGE,
//     which skipped the ride shutdown AND re-raised the radio every 60 s on the
//     charge-retry branch.
//
// Neither is a logging bug worth "fixing" in the logger: a charger attached and pushing
// zero amps is precisely the evidence that diagnosed pack #1, and the CSV must keep
// recording it. The bug was asking the logger a safety question. So the interlock gets
// its own file, sampled every loop pass, reading ONLY pack telemetry.
//
// ─── AND WHY IT IS A SAMPLER, NOT A PREDICATE ────────────────────────────────
// The first load interlock was written as a single function that both sampled and
// answered. Its "has any pack drawn current in the last 60 s" window was a lie: the
// timestamp was written only inside the function, and the function was called only at
// the moment of the sleep decision — so it was an INSTANTANEOUS snapshot wearing a
// 60 s label. Splitting update from query makes that class of bug impossible to write:
// rideWatchUpdate() is called unconditionally from loop(), and the query functions are
// pure reads.

#include "Config.h"
#include "OkaiBMS.h"

// Any pack above this, either direction, counts as "in use" for the sleep interlock.
#define RW_LOAD_A           1.0f
#define RW_LOAD_WINDOW_MS   60000UL

// The BMS emits 0x2020 == 8.224 A as an IDLE PLACEHOLDER, not a reading (OkaiBMS.h:43).
// Display.ino has filtered it since July. The load interlock did not, which inverted it:
// an idle pack publishing the placeholder read as 8 A of load, so sleep would have been
// refused forever on a bench with nothing happening.
#define RW_PLACEHOLDER_A    8.224f
static inline bool rwIsPlaceholder(float a) {
    return fabsf(a - RW_PLACEHOLDER_A) < 0.05f;
}

static uint32_t sRwRideUntil   = 0;   // discharge seen → now + hysteresis
static uint32_t sRwLastLoadMs  = 0;   // last pass on which any pack moved real current
static bool     sRwEverLoaded  = false;
static bool     sRwSeenFrame   = false;   // at least one valid pack frame since boot
static bool     sRwDischSeen   = false;   // any discharge at all since boot (M-4 boot gate)

// Call from loop(), EVERY pass, straight after uartLoop() so it sees fresh frames.
void rideWatchUpdate() {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        if ((now - packs[i].lastUpdateMs) > PACK_CONNECTED_MS) continue;  // stale frame
        sRwSeenFrame = true;

        float a = packs[i].current;
        if (rwIsPlaceholder(a)) continue;

        if (a < -LOG_RIDE_THRESHOLD_A) {          // real discharge → riding
            sRwRideUntil = now + LOG_RIDE_HYSTERESIS_MS;
            sRwDischSeen = true;
        }
        float mag = (a < 0) ? -a : a;
        if (mag > RW_LOAD_A) {                    // real current, either direction
            sRwLastLoadMs = now;
            sRwEverLoaded = true;
        }
    }
}

// ── Queries: pure reads, safe to call anywhere ───────────────────────────────

// SOP-038 ride interlock. True while any pack has discharged past the threshold within
// the hysteresis window. Independent of fsReady, of _mode, and of the charger bit.
bool rideSuspected() {
    return sRwRideUntil && (int32_t)(millis() - sRwRideUntil) < 0;
}

// Sleep interlock: has any pack moved real current recently? A rider on the water has
// packs under load; sleeping there halts the keep-alive and the packs cut output 5 s
// later, which is a vehicle stopping, not a screen turning off.
bool packsLoadedRecently() {
    return sRwEverLoaded && (millis() - sRwLastLoadMs) < RW_LOAD_WINDOW_MS;
}

// Genuinely charging: a charger is attached AND real current is going IN. Deliberately
// stricter than the logger's LOG_CHARGE, which keys on the charger bit alone — that is
// right for logging (pack #1's zero-amp fault must be recorded) and wrong for a safety
// gate, because the same reading appears on a stationary pack that is not charging and
// on a fault that lasts hours.
bool chargeActive() {
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        if ((millis() - packs[i].lastUpdateMs) > PACK_CONNECTED_MS) continue;
        if (!packs[i].chargerDetected) continue;
        float a = packs[i].current;
        if (rwIsPlaceholder(a)) continue;
        if (a > kBalanceCurrentA) return true;      // 0.150 A clears the ±0.2 A offset band
    }
    return false;
}

// M-4 boot gate: POSITIVE evidence that no ride is in progress, for the deferred boot
// join. "!rideSuspected()" is not enough at t=4 s — the hysteresis has had no chance to
// arm, so a freshly-booted board mid-ride looks identical to one on the bench. This
// requires that pack frames are actually arriving AND that nothing has discharged since
// boot. A rider coasting with the throttle fully closed still defeats it, which is why
// the station attempt stays off the keep-alive's task and why the window is short.
bool bootJoinSafe() {
    return sRwSeenFrame && !sRwDischSeen && !rideSuspected();
}
