// Heartbeat.ino — keep-alive packets via shared GPIO2 TX bus
//
// unlock() sends the 5-byte Ruipu keep-alive on pack[0]'s stream (Serial1,
// TX=GPIO2). All 4 pack RX lines are wired to GPIO2 in parallel, so one
// transmission reaches every pack simultaneously.  We then flush the RX
// buffers of packs 2-4 so stale bytes don't corrupt the next read frame.

#include "RuipuBattery.h"
extern RuipuBattery pack[NUM_PACKS];

static uint32_t _hbLast;

void heartbeatInit() {
    _hbLast = 0;
}

void heartbeatLoop() {
    if (millis() - _hbLast < HEARTBEAT_INTERVAL_MS) return;
    _hbLast = millis();

    pack[0].unlock();                                   // TX + flush pack1 RX
    for (uint8_t i = 1; i < NUM_PACKS; i++) {
        pack[i].reset();                                // flush remaining RX only
    }
}
