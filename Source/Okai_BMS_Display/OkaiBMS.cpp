/*
  OkaiBMS.cpp — Full-decode library for Ruipu/Okai 10S4P BMS packs

  Packet map (36 bytes, CRC over [0..34]):
    [0]        0x3A        frame start
    [1]        0x16        packet type (telemetry response)
    [2]        0x20        constant (pack category — not decoded)
    [3]        STATUS      bit0=cFET  bit1=dFET  bit2=chargerDet  bit3=chargerOK
                           bit4=cellUV  bit5=chargingBulk
    [4]        0x00        reserved
    [5]        SOC         state of charge 0–100 %
    [6]        MAX_SOC     max achievable SOC / SoH indicator %
    [7]        TEMP_CELL   hottest cell sensor °C
    [8]        TEMP_AVG    average cell temp °C
    [9]        TEMP_FET    discharge FET temp °C
    [10]       TEMP_MCU    BMS MCU temp °C
    [11–12]    CYCLES      charge cycle count (little-endian uint16)
    [13]       CHGR_STATE  0x00=none 0x19=begin 0x7C=bulk
    [14]       0x0F        constant (not decoded)
    [15–16]    0x00        reserved
    [17]       CHGR_ACT    0x00=no charger  0x04=charger active
    [18–19]    0x00        reserved
    [20]       CAP200      rated capacity in 200 mAh units (0x40=64 → 12 800 mAh)
    [21–22]    VOLTAGE     pack voltage mV (little-endian uint16)
    [23–24]    0x00        reserved
    [25–26]    CURRENT     pack current mA (little-endian int16, + = charging)
    [27–28]    CURR2       secondary current (correlated; int16; appears with tiny currents)
    [29–30]    CELL_HI     highest cell voltage mV (little-endian uint16)
    [31–32]    CELL_LO     lowest cell voltage mV (little-endian uint16)
    [33]       0x52        model/HW constant (not decoded)
    [34]       0x2C        model/HW constant (not decoded)
    [35]       CRC         Dallas/Maxim 1-wire CRC of [0..34], poly 0x8C
*/

#include "OkaiBMS.h"

static const byte HEARTBEAT[5] = { 0x3A, 0x13, 0x01, 0x16, 0x79 };

OkaiBMS::OkaiBMS(Stream *stream) : _stream(stream), _bytesRead(0) {
    memset(_buf, 0, sizeof(_buf));
}

// Dallas/Maxim 1-wire CRC, polynomial 0x8C
byte OkaiBMS::_crc(const byte* data, byte len) const {
    byte crc = 0x00;
    while (len--) {
        byte extract = *data++;
        for (byte i = 8; i; i--) {
            byte sum = (crc ^ extract) & 0x01;
            crc >>= 1;
            if (sum) crc ^= 0x8C;
            extract >>= 1;
        }
    }
    return crc;
}

void OkaiBMS::unlock() {
    reset();
    _stream->write(HEARTBEAT, sizeof(HEARTBEAT));
}

// Frame-synced read: byte [0] must be 0x3A; CRC validated at 36 bytes.
bool OkaiBMS::read() {
    while (_stream->available()) {
        byte b = (byte)_stream->read();

        // Sync: only start a new frame on 0x3A
        if (_bytesRead == 0 && b != 0x3A) continue;

        _buf[_bytesRead++] = b;

        if (_bytesRead == 36) {
            _bytesRead = 0;
            if (_buf[35] == _crc(_buf, 35)) return true;
            // CRC mismatch — search forward for next 0x3A inside the bad frame
            for (uint8_t i = 1; i < 36; i++) {
                if (_buf[i] == 0x3A) {
                    uint8_t remaining = 36 - i;
                    memmove(_buf, _buf + i, remaining);
                    _bytesRead = remaining;
                    break;
                }
            }
        }
    }
    return false;
}

void OkaiBMS::reset() {
    while (_stream->available()) _stream->read();
    _bytesRead = 0;
}

const byte* OkaiBMS::buf() const { return _buf; }

// ── Status flags ──────────────────────────────────────────────────────────────
uint8_t OkaiBMS::statusByte()         const { return _buf[3]; }
bool    OkaiBMS::isChargeFETOn()      const { return (_buf[3] >> 0) & 1; }
bool    OkaiBMS::isDischargeFETOn()   const { return (_buf[3] >> 1) & 1; }
bool    OkaiBMS::isChargerDetected()  const { return (_buf[3] >> 2) & 1; }
bool    OkaiBMS::isChargerOK()        const { return (_buf[3] >> 3) & 1; }
bool    OkaiBMS::isCellUndervoltage() const { return (_buf[3] >> 4) & 1; }
bool    OkaiBMS::isChargingBulk()     const { return (_buf[3] >> 5) & 1; }

// ── SOC & health ──────────────────────────────────────────────────────────────
uint8_t OkaiBMS::soc()    const { return _buf[5]; }
uint8_t OkaiBMS::maxSoc() const { return _buf[6]; }

// ── Voltage ───────────────────────────────────────────────────────────────────
uint16_t OkaiBMS::rawVoltage() const { return ((uint16_t)_buf[22] << 8) | _buf[21]; }
float    OkaiBMS::voltage()    const { return rawVoltage() / 1000.0f; }

// ── Current ───────────────────────────────────────────────────────────────────
int16_t OkaiBMS::rawCurrent() const {
    return (int16_t)(((uint16_t)_buf[26] << 8) | _buf[25]);
}
float OkaiBMS::current() const { return rawCurrent() / 1000.0f; }

// ── Cell voltages ─────────────────────────────────────────────────────────────
uint16_t OkaiBMS::rawCellHigh()   const { return ((uint16_t)_buf[30] << 8) | _buf[29]; }
uint16_t OkaiBMS::rawCellLow()    const { return ((uint16_t)_buf[32] << 8) | _buf[31]; }
float    OkaiBMS::cellHigh()      const { return rawCellHigh() / 1000.0f; }
float    OkaiBMS::cellLow()       const { return rawCellLow()  / 1000.0f; }
uint16_t OkaiBMS::cellSpread_mV() const {
    uint16_t hi = rawCellHigh(), lo = rawCellLow();
    return (hi >= lo) ? (hi - lo) : 0;
}

// ── Temperatures ──────────────────────────────────────────────────────────────
uint8_t OkaiBMS::tempCellMax() const { return _buf[7];  }
uint8_t OkaiBMS::tempCellAvg() const { return _buf[8];  }
uint8_t OkaiBMS::tempFET()     const { return _buf[9];  }
uint8_t OkaiBMS::tempMCU()     const { return _buf[10]; }

uint8_t OkaiBMS::tempMax() const {
    uint8_t m = _buf[7];
    for (uint8_t i = 8; i <= 10; i++) if (_buf[i] > m) m = _buf[i];
    return m;
}
uint8_t OkaiBMS::tempMin() const {
    uint8_t m = _buf[7];
    for (uint8_t i = 8; i <= 10; i++) if (_buf[i] < m) m = _buf[i];
    return m;
}

// ── Cycle count ───────────────────────────────────────────────────────────────
uint16_t OkaiBMS::cycleCount() const {
    return ((uint16_t)_buf[12] << 8) | _buf[11];
}

char* OkaiBMS::cycleID(char* out, size_t len) const {
    snprintf(out, len, "CYC-%u", (unsigned)cycleCount());
    return out;
}

// ── Charger state ─────────────────────────────────────────────────────────────
OkaiBMS::ChargerState OkaiBMS::chargerState() const {
    switch (_buf[13]) {
        case 0x00: return CHARGER_NONE;
        case 0x19: return CHARGER_BEGIN;
        case 0x7C: return CHARGER_BULK;
        default:   return CHARGER_NONE;
    }
}
bool OkaiBMS::chargerActive() const { return _buf[17] == 0x04; }

// ── Rated capacity ────────────────────────────────────────────────────────────
uint16_t OkaiBMS::ratedCapacity_mAh() const { return (uint16_t)_buf[20] * 200u; }

// ── Protocol / hardware constants ─────────────────────────────────────────────
uint8_t OkaiBMS::packetType()  const { return _buf[1]; }
uint8_t OkaiBMS::modelByte1()  const { return _buf[33]; }
uint8_t OkaiBMS::modelByte2()  const { return _buf[34]; }
