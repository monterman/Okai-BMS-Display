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
// 2026-10-09 - 60 s → 300 s (audit S-8). A water bridge on BTN1 after the vehicle has
// been at rest for more than the window still sleeps the board, and under K-1 that cut
// latches. Widening the window is the cheapest real reduction in that risk: it costs
// nothing but a longer wait before a DELIBERATE sleep is allowed, and the 60 s timer
// wake (PowerManager.ino) already means deep sleep barely functions as an off switch.
#define RW_LOAD_WINDOW_MS   300000UL

// The BMS emits 0x2020 == 8.224 A as an IDLE PLACEHOLDER, not a reading (OkaiBMS.h:43).
// Display.ino has filtered it since July. The load interlock did not, which inverted it:
// an idle pack publishing the placeholder read as 8 A of load, so sleep would have been
// refused forever on a bench with nothing happening.
#define RW_PLACEHOLDER_A    8.224f
static inline bool rwIsPlaceholder(float a) {
    return fabsf(a - RW_PLACEHOLDER_A) < 0.05f;
}

static uint32_t sRwRideUntil   = 0;   // discharge seen → now + hysteresis
static bool     sRwRideArmed   = false;   // N-3: "armed at all", so a rollover-to-0 deadline
                                          // is not read as "never armed"
static uint32_t sRwLastLoadMs  = 0;   // last pass on which any pack moved real current
static bool     sRwEverLoaded  = false;
static bool     sRwSeenFrame   = false;   // at least one valid pack frame since boot
static bool     sRwDischSeen   = false;   // any discharge at all since boot (M-4 boot gate)
static uint32_t sRwChgSince    = 0;   // first pass of the current unbroken charger-bit run
static bool     sRwChgPresent  = false;

// Call from loop(), EVERY pass, straight after uartLoop() so it sees fresh frames.
void rideWatchUpdate() {
    const uint32_t now = millis();
    bool chgSeenThisPass = false;

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        if ((now - packs[i].lastUpdateMs) > PACK_CONNECTED_MS) continue;  // stale frame
        sRwSeenFrame = true;

        // The charger BIT, independent of current. This is what holds the radio up for a
        // whole charge - see chargerPresent() for why the bit and not the current.
        if (packs[i].chargerDetected) chgSeenThisPass = true;

        float a = packs[i].current;
        if (rwIsPlaceholder(a)) continue;

        if (a < -LOG_RIDE_THRESHOLD_A) {          // real discharge → riding
            sRwRideUntil = now + LOG_RIDE_HYSTERESIS_MS;
            sRwRideArmed = true;
            sRwDischSeen = true;
        }
        float mag = (a < 0) ? -a : a;
        if (mag > RW_LOAD_A) {                    // real current, either direction
            sRwLastLoadMs = now;
            sRwEverLoaded = true;
        }
    }

    // Dwell timer on the charger bit: sRwChgSince marks the start of the current unbroken
    // run of passes in which SOME pack asserted it. Any pass without it resets the run.
    if (chgSeenThisPass) {
        if (!sRwChgPresent) { sRwChgPresent = true; sRwChgSince = now; }
    } else {
        sRwChgPresent = false;
        sRwChgSince   = 0;
    }
}

// ── Queries: pure reads, safe to call anywhere ───────────────────────────────

// SOP-038 ride interlock. True while any pack has discharged past the threshold within
// the hysteresis window. Independent of fsReady, of _mode, and of the charger bit.
bool rideSuspected() {
    // N-3: gate on "was it ever armed", not on sRwRideUntil != 0. Once per 49.7 days
    // now + 120000 wraps to exactly 0, and the old form read that as "never armed" —
    // i.e. it would have reported NOT riding, mid-ride, for one sample.
    return sRwRideArmed && (int32_t)(millis() - sRwRideUntil) < 0;
}

// Any discharge at all since boot. Used as the no-telemetry backstop on the AP-client
// window re-arm: an associated phone must not be able to hold the radio up once this
// board has ever seen the vehicle move.
bool rideEverSeen() { return sRwDischSeen; }

// ── The charger BIT, with no current test. THIS is the WiFi gate. ─────────────
// 2026-10-09 - chargeActive() below was briefly used to gate the radio and that was
// WRONG, caught in audit. It requires current > kBalanceCurrentA (0.150 A), which makes
// it the exact logical complement of UART.ino's chargeDone — so it goes false for the
// ENTIRE taper/balancing phase, which Display.ino notes "takes as long or longer" than
// bulk and which draws tens of milliamps. The owner's requirement is the opposite:
//
//   "If we are charging PAX, Wi-Fi should stay on because we're not writing, we're
//    charging" — and the dock is exactly where he pulls logs from.
//
// Worst case traced: a link drop during the taper (his garage is at the edge of
// coverage) tears the radio down, and the retry then needs chargeActive() — false —
// so there is no reconnect for the rest of the charge.
//
// Using the bit does NOT reopen G-2. G-2's failure was never the bit itself: it was
// that `charging` held a VETO over the ride shutdown (`riding && !charging`), so a
// latched bit could suppress the shutdown. That veto is deleted. rideSuspected() is
// computed from discharge current and is fully independent of the bit, so pack #1's
// stuck-bit fault cannot defeat it — it can only keep the radio up on a stationary
// vehicle, which is harmless and is what the owner asked for.
bool chargerPresent() { return sRwChgPresent; }

// Same, but held for at least `ms`. Used before INITIATING a join, so a chattering bit
// or a momentary assert cannot trigger a 15 s blocking scan.
bool chargerPresentFor(uint32_t ms) {
    return sRwChgPresent && (millis() - sRwChgSince) >= ms;
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
