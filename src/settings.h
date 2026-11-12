/*
 * CRSF-Relay for RP2040 - persistent settings (former EEProm struct)
 * GPL-3.0-or-later, see crsf.h
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "crsf.h"

struct Settings {
    char    txname[8];
    uint8_t rewrite_name;
    uint8_t link_stat_mode;     // 0=Off 1=Forward 2=Transform 3=Tunnel
    uint8_t failsafe_mode;      // 0=Forward (stop relay) 1=Hold
    uint8_t address;            // own CRSF address
    uint8_t tx_rewrite_address;
    uint8_t rx_rewrite_address;
    uint8_t half_duplex;
    uint8_t baudrate;           // index into relayBaudrates
    uint8_t telemetry_rate;
    uint8_t tempOffTelemetry;   // seconds
    uint8_t own_telemetry;      // new: send own voltage/temperature
    uint8_t telemetryMode[crsf::type::Ping]; // up to 1.0.8: 0=Off 1=Forward 2=Tunnel per type (unused since 1.0.9)
    // ---- added in 1.0.4 (keep new fields at the end: older flash data is migrated)
    uint8_t ardupilot_mode;     // type 0x80 passthrough: 0=Off 1=Forward 2=Tunnel
    // ---- added in 1.0.8
    uint8_t relay_inverted;     // relay side polarity: 0=normal 1=inverted
    // ---- added in 1.0.9
    uint8_t telemetry_mode;     // all telemetry types: 0=Off 1=Forward (replaces telemetryMode[])

    void defaults() {
        std::memset(this, 0, sizeof(*this));
        std::strncpy(txname, "RelayTX", sizeof(txname));
        rewrite_name       = 0;
        link_stat_mode     = 2;
        failsafe_mode      = 0;
        address            = 0xCD;
        tx_rewrite_address = 0xCE;
        rx_rewrite_address = 0xCF;
        half_duplex        = 0;
        baudrate           = 3;
        telemetry_rate     = 2;
        tempOffTelemetry   = 30;
        own_telemetry      = 0;
        for (auto& m : telemetryMode) m = 1;
        ardupilot_mode     = 1;
        relay_inverted     = 0;
        telemetry_mode     = 1;
    }
    // range check after loading from flash
    // 1.0.8: these values have no menu entry any more and are fixed to their defaults
    void fixHidden() {
        Settings d;
        d.defaults();
        std::memcpy(txname, d.txname, sizeof(txname));
        rewrite_name       = d.rewrite_name;
        failsafe_mode      = d.failsafe_mode;   // Forward: relay stops when receiver 1 is lost
        address            = d.address;
        tx_rewrite_address = d.tx_rewrite_address;
        rx_rewrite_address = d.rx_rewrite_address;
        tempOffTelemetry   = d.tempOffTelemetry;
    }

    bool plausible() const {
        if (txname[sizeof(txname) - 1] != '\0') return false;
        if (rewrite_name > 1 || link_stat_mode > 3 || failsafe_mode > 1) return false;
        if (address < 192 || address > 207) return false;
        if (tx_rewrite_address < 192 || tx_rewrite_address > 207) return false;
        if (rx_rewrite_address < 192 || rx_rewrite_address > 207) return false;
        if (half_duplex > 1 || baudrate > 4 || own_telemetry > 1) return false;
        if (telemetry_rate < 1 || telemetry_rate > 10) return false;
        if (tempOffTelemetry < 1 || tempOffTelemetry > 60) return false;
        for (auto m : telemetryMode) if (m > 2) return false;
        if (ardupilot_mode > 2 || relay_inverted > 1 || telemetry_mode > 1) return false;
        return true;
    }
};

constexpr uint32_t relayBaudrates[] = {400'000u, 420'000u, 460'800u, 921'600u, 1'870'000u};
constexpr uint32_t inputBaudrate = 420'000u;

// ---- flash image: [magic u32][version u16][size u16][settings: size bytes][pad to 4][check u32]
// Older images with a smaller settings struct are accepted; new fields keep their defaults.
namespace store {
constexpr uint32_t Magic = 0x52454C59; // "RELY"
constexpr uint16_t Version = 1;
constexpr size_t Header = 8;

inline size_t checkOffset(size_t size) { return (Header + size + 3) & ~size_t(3); }

inline uint32_t fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// returns the image length; buf must hold checkOffset(sizeof(Settings)) + 4 bytes
inline size_t encode(const Settings& s, uint8_t* buf) {
    const uint16_t size = sizeof(Settings);
    const size_t co = checkOffset(size);
    std::memset(buf, 0, co + 4);
    const uint32_t magic = Magic;
    const uint16_t version = Version;
    std::memcpy(buf, &magic, 4);
    std::memcpy(buf + 4, &version, 2);
    std::memcpy(buf + 6, &size, 2);
    std::memcpy(buf + Header, &s, size);
    const uint32_t c = fnv(buf, co);
    std::memcpy(buf + co, &c, 4);
    return co + 4;
}

inline bool decode(const uint8_t* buf, size_t avail, Settings& out) {
    uint32_t magic; uint16_t version, size;
    std::memcpy(&magic, buf, 4);
    std::memcpy(&version, buf + 4, 2);
    std::memcpy(&size, buf + 6, 2);
    if (magic != Magic || version != Version) return false;
    if (size == 0 || size > sizeof(Settings)) return false;
    const size_t co = checkOffset(size);
    if (co + 4 > avail) return false;
    uint32_t c;
    std::memcpy(&c, buf + co, 4);
    if (c != fnv(buf, co)) return false;
    Settings s;
    s.defaults();
    std::memcpy(&s, buf + Header, size);
    if (!s.plausible()) return false;
    out = s;
    return true;
}
}
