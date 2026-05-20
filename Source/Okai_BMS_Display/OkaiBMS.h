/*
  OkaiBMS.h — Full-decode library for Ruipu/Okai 10S4P BMS packs
  Protocol: 9600 baud 8N1, 36-byte telemetry frame, CRC = Dallas/Maxim 1-wire (poly 0x8C)
  Keep-alive: send 0x3A 0x13 0x01 0x16 0x79 every ≤5 s on pack RX (BLUE wire)

  Decoded from field capture 2026-05-19 / 2026-05-20 across five packs (CYC 54–8256).
  Supersedes jsutcliff/RuipuBattery — field names kept compatible where possible.
*/

#pragma once
#include "Arduino.h"

class OkaiBMS {
public:
    explicit OkaiBMS(Stream *stream);

    // ── Protocol ──────────────────────────────────────────────────────────────
    void     unlock();              // send keep-alive heartbeat (non-blocking)
    bool     read();                // returns true when a new valid packet is ready
    void     reset();               // flush stream + clear rx buffer
    const byte* buf() const;        // raw 36-byte frame (valid after read()==true)

    // ── Status flags (byte [3]) ───────────────────────────────────────────────
    uint8_t  statusByte()         const;  // raw status byte
    bool     isChargeFETOn()      const;  // bit 0 — charge FET enabled
    bool     isDischargeFETOn()   const;  // bit 1 — discharge FET enabled
    bool     isChargerDetected()  const;  // bit 2 — charger present on BLUE wire
    bool     isChargerOK()        const;  // bit 3 — charger voltage in range
    bool     isCellUndervoltage() const;  // bit 4 — any cell below threshold
    bool     isChargingBulk()     const;  // bit 5 — bulk-charge phase active

    // ── State of charge & health (bytes [5], [6]) ────────────────────────────
    uint8_t  soc()     const;  // state of charge, 0–100 %
    uint8_t  maxSoc()  const;  // max achievable SOC (SoH indicator %) [b06]
                               // 100 = new pack, <100 = reduced capacity

    // ── Pack voltage (bytes [21–22], little-endian, ÷1000 = V) ───────────────
    uint16_t rawVoltage() const;
    float    voltage()    const;   // V

    // ── Pack current (bytes [25–26], int16 little-endian, ÷1000 = A) ─────────
    // Positive = charging, negative = discharging.
    // Note: 0x2020 (8.224 A) is a BMS idle placeholder — not a real reading.
    int16_t  rawCurrent() const;
    float    current()    const;   // A (signed)

    // ── Cell voltages (bytes [29–32], little-endian, ÷1000 = V) ─────────────
    uint16_t rawCellHigh()    const;
    uint16_t rawCellLow()     const;
    float    cellHigh()       const;   // V — highest cell
    float    cellLow()        const;   // V — lowest cell
    uint16_t cellSpread_mV()  const;   // cellHigh − cellLow in mV (health metric)

    // ── Cell temperatures (bytes [7–10], °C) ─────────────────────────────────
    uint8_t  tempCellMax() const;  // [b07] — hottest sensor
    uint8_t  tempCellAvg() const;  // [b08] — average cell temp
    uint8_t  tempFET()     const;  // [b09] — discharge FET temp
    uint8_t  tempMCU()     const;  // [b10] — BMS microcontroller temp
    uint8_t  tempMax()     const;  // highest of all four sensors
    uint8_t  tempMin()     const;  // lowest of all four sensors

    // ── Cycle count & CYC fingerprint (bytes [11–12], little-endian) ─────────
    uint16_t cycleCount() const;
    // Writes "CYC-XXXX\0" into buf (needs ≥10 chars). Returns buf.
    char*    cycleID(char* buf, size_t len) const;

    // ── Charger state (byte [13]) ─────────────────────────────────────────────
    enum ChargerState : uint8_t {
        CHARGER_NONE          = 0x00,  // no charger / discharging
        CHARGER_BEGIN         = 0x19,  // charger detected, pre-charge
        CHARGER_BULK          = 0x7C,  // bulk / CV charging
    };
    ChargerState chargerState()  const;  // [b13]
    bool         chargerActive() const;  // [b17] == 0x04

    // ── Rated capacity (byte [20]) ────────────────────────────────────────────
    // [b20] × 200 mAh = rated pack capacity.  Observed: 0x40 = 64 → 12 800 mAh.
    uint16_t ratedCapacity_mAh() const;

    // ── Protocol / hardware constants ─────────────────────────────────────────
    uint8_t  packetType()  const;   // [b01] — always 0x16 for telemetry
    uint8_t  modelByte1()  const;   // [b33] — constant 0x52 (not fully decoded)
    uint8_t  modelByte2()  const;   // [b34] — constant 0x2C (not fully decoded)

    // ── Backward-compat aliases (jsutcliff/RuipuBattery names) ───────────────
    float    high()         const { return cellHigh(); }
    float    low()          const { return cellLow();  }
    uint8_t  maxTemp()      const { return tempMax();  }
    uint8_t  minTemp()      const { return tempMin();  }
    uint8_t  maxCellTemp()  const { return tempCellMax(); }
    uint8_t  avgCellTemp()  const { return tempCellAvg(); }
    uint8_t  dischargeFETTemp()      const { return tempFET(); }
    uint8_t  microcontrollerTemp()   const { return tempMCU(); }
    uint16_t chargeCycleCount()      const { return cycleCount(); }
    uint8_t  rawStatus()             const { return statusByte(); }
    bool     isChargerOKCompat()     const { return isChargerOK(); }
    uint16_t rawLow()                const { return rawCellLow(); }
    uint16_t rawHigh()               const { return rawCellHigh(); }

private:
    byte    _crc(const byte* data, byte len) const;
    Stream* _stream;
    uint8_t _bytesRead;
    byte    _buf[36];
};
