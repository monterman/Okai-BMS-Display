// UART.ino — serial port init, OkaiBMS instances, pack polling
//
// Pack 1: Hardware UART1  RX=GPIO1  TX=GPIO2  (shared TX bus)
// Pack 2: Hardware UART2  RX=GPIO16 TX=GPIO2
// Pack 3: EspSoftwareSerial  RX=GPIO17 TX=GPIO2
// Pack 4: EspSoftwareSerial  RX=GPIO18 TX=GPIO2
//
// Install via Library Manager: "EspSoftwareSerial" by Dirk Kaar

#include <SoftwareSerial.h>
#include "OkaiBMS.h"

static SoftwareSerial _ss3, _ss4;
static bool  _wasValid[NUM_PACKS];

float   g_ridePowerEma_W = 0.0f;   // EMA of fleet discharge power (W)
uint8_t g_ridePowerN     = 0;       // sample count; < 3 means warming up

// Global — shared with Heartbeat.ino
OkaiBMS pack[NUM_PACKS] = {
    OkaiBMS(&Serial1),
    OkaiBMS(&Serial2),
    OkaiBMS(&_ss3),
    OkaiBMS(&_ss4),
};

// Global — shared with Display.ino, Logger.ino, WiFiServer.ino
PackData packs[NUM_PACKS];

void uartInit() {
    memset(packs, 0, sizeof(packs));
    memset(_wasValid, 0, sizeof(_wasValid));

    Serial1.begin(BMS_BAUD, SERIAL_8N1, PACK1_RX_PIN, PACK1_TX_PIN);
    Serial2.begin(BMS_BAUD, SERIAL_8N1, PACK2_RX_PIN, PACK2_TX_PIN);
    _ss3.begin(BMS_BAUD, SWSERIAL_8N1, PACK3_RX_PIN, PACK3_TX_PIN);
    _ss4.begin(BMS_BAUD, SWSERIAL_8N1, PACK4_RX_PIN, PACK4_TX_PIN);

    Serial.println("[UART] 4 pack ports open");
}

void uartLoop() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (pack[i].read()) {
            // Accumulate Wh from the previous interval using the OLD V and A
            // (Euler forward: treat the interval as constant at the last reading)
            if (packs[i].valid) {
                uint32_t dtMs = now - packs[i].lastUpdateMs;
                if (dtMs > 0 && dtMs < 30000UL) {
                    float dtHr = dtMs / 3600000.0f;
                    float dWh  = packs[i].voltage * packs[i].current * dtHr;
                    if (dWh > 0.0f) packs[i].whIn  += dWh;
                    else            packs[i].whOut  -= dWh;
                }
            }

            packs[i].soc             = pack[i].soc();
            packs[i].voltage         = pack[i].voltage();
            packs[i].current         = pack[i].current();
            packs[i].cellHigh        = pack[i].high();
            packs[i].cellLow         = pack[i].low();
            packs[i].maxTemp         = pack[i].maxTemp();
            packs[i].cycles          = pack[i].chargeCycleCount();
            packs[i].maxSoc          = pack[i].maxSoc();
            packs[i].rawStatus       = pack[i].rawStatus();
            packs[i].chargerDetected = pack[i].isChargerDetected();
            packs[i].isCharging      = pack[i].isChargingBulk();
            packs[i].chargeDone      = pack[i].isChargerDetected()
                                       && !pack[i].isChargingBulk()
                                       && pack[i].soc() == 100;
            packs[i].valid           = true;
            packs[i].lastUpdateMs    = now;
        }

        // Mark stale if no successful read for > 10 s
        if (packs[i].valid && (now - packs[i].lastUpdateMs) > 10000UL) {
            packs[i].valid = false;
        }

        // Rising edge: pack just appeared → identify against registry
        if (!_wasValid[i] && packs[i].valid) packRegistryIdentify(i);
        _wasValid[i] = packs[i].valid;
    }

    // ── Ride power EMA — resets on each new session, updates while discharging ─
    static LogMode _prvMode = LOG_IDLE;
    LogMode _curMode = logCurrentMode();
    if (_curMode == LOG_RIDE && _prvMode != LOG_RIDE) {
        g_ridePowerEma_W = 0.0f;
        g_ridePowerN     = 0;
    }
    _prvMode = _curMode;
    if (_curMode == LOG_RIDE) {
        float totalW = 0.0f;
        bool  any    = false;
        for (uint8_t j = 0; j < NUM_PACKS; j++) {
            if (packs[j].valid && packs[j].current < -LOG_RIDE_THRESHOLD_A) {
                totalW += packs[j].voltage * (-packs[j].current);
                any = true;
            }
        }
        if (any) {
            const float ALPHA = 0.10f;
            if (g_ridePowerN < 255) g_ridePowerN++;
            g_ridePowerEma_W = (g_ridePowerN == 1) ? totalW
                             : ALPHA * totalW + (1.0f - ALPHA) * g_ridePowerEma_W;
        }
    }
}
