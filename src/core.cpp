/*
 * CRSF-Relay for RP2040 - protocol core
 *
 * Port of RC-Relay32 by Wilhelm Meier (WMuCpp: boards/rcRelay32,
 * include_stm32/rc/package_relay_rewrite.h, crsf_2*.h, rc_2.h).
 * Behaviour follows the original; deviations are marked with "RP2040 port:".
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "core.h"
#include "hal.h"
#include "config.h"
#include "mlrs.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace crsf;

namespace core {

Settings settings;

namespace {

// ---------------------------------------------------------------- queues
FrameQueue<32> toHandset; // to receiver #1 (former crsf::messageBuffer)
FrameQueue<16> toModule;  // to module #2   (former relay::messageBuffer)

void sendToHandset(const Frame& f) {
    if (toHandset.push(f)) {
        // counted in stats via queue
    }
}

uint32_t cntInFrames = 0, cntRelayFrames = 0, cntChannelsIn = 0, cntToModule = 0, cntToHandset = 0;

namespace relay { extern bool enabled; }

// ---------------------------------------------------------------- parameter trace (USB console 'p')
bool trace = false;
uint32_t traceNow = 0;
struct TraceReq { uint8_t dev = 0, type = 0, idx = 0, chunk = 0; uint32_t t = 0; bool valid = false; };
TraceReq lastReq[4]; // per device behind the relay (module side address)

// module side address <-> address shown to / used by the handset
uint8_t moduleSide(uint8_t dest) {
    if (dest == settings.tx_rewrite_address) return addr::TX;
    if (dest == settings.rx_rewrite_address) return addr::RX;
    return dest;
}
uint8_t handsetSide(uint8_t src) {
    if (src == addr::TX) return settings.tx_rewrite_address;
    if (src == addr::RX) return settings.rx_rewrite_address;
    return src;
}
TraceReq* traceSlot(uint8_t dev, bool create) {
    TraceReq* oldest = &lastReq[0];
    for (auto& r : lastReq) {
        if (r.valid && r.dev == dev) return &r;
        if (!r.valid || int32_t(r.t - oldest->t) < 0) oldest = &r;
    }
    if (!create) return nullptr;
    *oldest = TraceReq{};
    oldest->dev = dev;
    return oldest;
}

const char* typeName(uint8_t t);
const char* tn(uint8_t t) { const char* n = typeName(t); return n ? n : "?"; }
const char* typeName(uint8_t t) {
    switch (t) {
    case type::Ping: return "Ping";
    case type::Info: return "Info";
    case type::ParamEntry: return "Entry";
    case type::ParamRead: return "Read";
    case type::ParamWrite: return "Write";
    case type::Command: return "Cmd";
    default: return nullptr;
    }
}

// a request from the handset side (before address rewrite)
void traceRequest(const Frame& f, bool own) {
    if (!trace || f.size < 6) return;
    const uint8_t t = f.type();
    const uint8_t dest = f.b[idx::dest];
    const bool hasIdx = (t == type::ParamRead || t == type::ParamWrite) && f.size >= 8;
    const uint8_t i = hasIdx ? f.b[5] : 0;
    const uint8_t c = hasIdx ? f.b[6] : 0;
    if (own) {
        hal::log("P %7lu > %-5s %02X i%u c%u (own)\n", (unsigned long)traceNow, tn(t), dest, i, c);
        return;
    }
    if (!hasIdx) {
        hal::log("P %7lu > %-5s %02X\n", (unsigned long)traceNow, tn(t), dest);
        if (t == type::Ping) for (auto& r : lastReq) if (r.valid) r.t = traceNow;
        return;
    }
    TraceReq& r = *traceSlot(moduleSide(dest), true);
    if (r.valid && r.type == t && r.idx == i && r.chunk == c) {
        hal::log("P %7lu > %-5s %02X i%u c%u  RETRY after %lu ms\n", (unsigned long)traceNow, tn(t), dest, i, c,
                 (unsigned long)(traceNow - r.t));
    } else {
        hal::log("P %7lu > %-5s %02X i%u c%u%s\n", (unsigned long)traceNow, tn(t), dest, i, c,
                 relay::enabled ? "" : "  (relay disabled!)");
    }
    r.type = t; r.idx = i; r.chunk = c; r.t = traceNow; r.valid = true;
}

// an extended frame from the relay side (before address rewrite)
void traceReply(const Frame& f) {
    if (!trace || f.size < 6) return;
    const uint8_t src = f.b[idx::src];
    const TraceReq* r = traceSlot(src, false);
    const uint32_t dt = r ? traceNow - r->t : 0;
    const uint8_t shown = handsetSide(src);
    if (f.type() == type::ParamEntry && f.size >= 8) {
        hal::log("P %7lu < %-5s %02X i%u rem%u  +%lu ms\n", (unsigned long)traceNow, tn(f.type()), shown,
                 f.b[5], f.b[6], (unsigned long)dt);
    } else if (typeName(f.type())) {
        hal::log("P %7lu < %-5s %02X  +%lu ms\n", (unsigned long)traceNow, typeName(f.type()), shown, (unsigned long)dt);
    } else {
        hal::log("P %7lu < t%02X  %02X %02X\n", (unsigned long)traceNow, f.type(), f.b[3], f.b[4]);
    }
}

// ---------------------------------------------------------------- relay (PacketRelayRewrite)
namespace relay {
bool enabled = true; // NOLINT
bool source = true;            // activateSource(true)
uint8_t txAddr = 0xCE;
uint8_t rxAddr = 0xCF;
bool doRewriteName = false;
uint8_t linkStatMode = 1;
uint8_t fwdMode[type::Ping] = {}; // only set via the menu callbacks, like the original
uint8_t apMode = 1;
uint8_t apCounter[18] = {};       // F0: per AppId (0x50xx, low nibble), F2: [16], other: [17]
uint8_t forwardRate = 2;
uint8_t typeCounter[type::Ping] = {};
bool telemOff = false;
uint32_t telemOffSince = 0;
uint32_t autoOnMs = 30'000;
bool half = false;

bool haveChannels = false;
uint8_t channels[ChannelsPayload] = {};
bool chanPending = false;
uint32_t lastUpdate = 0;
constexpr uint32_t updateMs = 10;

void disableTelemetryWithAutoOn(uint32_t now) {
    if (!telemOff) hal::log("# disable telemetry\n");
    telemOff = true;
    telemOffSince = now;
}

bool shouldSendTelemetry(uint8_t t) {
    if (t < type::Ping) {
        if (++typeCounter[t] >= forwardRate) {
            typeCounter[t] = 0;
            return true;
        }
    }
    return false;
}

void enqueueModule(const Frame& f) {
    if (toModule.push(f)) ++cntToModule;
}

// RP2040 port: parameter read requests to devices behind the relay (module #2, its
// receiver, the flight controller behind it, ...) are repeated by the relay if the answer
// does not arrive in time. The Lua script would otherwise wait 5 s for every lost answer
// (timeout for all devices except its own TX module).
struct Pending {
    uint8_t dev = 0;         // module side address (0xEE, 0xEC, 0xC8, ...)
    bool used = false;
    bool active = false;     // waiting for an answer
    Frame req;               // request as sent to the module (rewritten)
    uint8_t index = 0;
    uint32_t sentAt = 0;
    uint32_t lastUse = 0;
    uint8_t retries = 0;
    uint8_t dupIndex = 0;    // answers to be dropped (duplicates caused by our retries)
    uint8_t dupRem = 0;
    uint8_t dupCount = 0;
    uint32_t dupUntil = 0;
    uint32_t rtt = 0;        // smoothed round trip time
};
Pending pending[4];
constexpr uint32_t maxTimeout = 1500;
constexpr uint8_t maxRetries = 3;
uint32_t cntRelayRetries = 0;
uint32_t cntDupDropped = 0;
uint32_t gNow = 0;

Pending* pendingSlot(uint8_t dev, bool create) {
    Pending* victim = &pending[0];
    for (auto& p : pending) {
        if (p.used && p.dev == dev) return &p;
        if (!p.used || (victim->used && int32_t(p.lastUse - victim->lastUse) < 0)) victim = &p;
    }
    if (!create) return nullptr;
    *victim = Pending{};
    victim->dev = dev;
    victim->used = true;
    return victim;
}

uint32_t retryTimeout(const Pending& p) {
    const bool local = (p.dev == addr::TX); // module #2 itself: no radio link in between
    const uint32_t rtt = p.rtt ? p.rtt : (local ? 20u : 300u);
    const uint32_t minT = local ? 100u : 500u;
    return std::min(maxTimeout, std::max(minT, 3 * rtt));
}

void trackRequest(uint8_t dev, const Frame& f) {
    Pending& p = *pendingSlot(dev, true);
    p.active = true;
    p.req = f;
    p.index = f.b[5];
    p.sentAt = gNow;
    p.lastUse = gNow;
    p.retries = 0;
}

void retryTick(uint32_t now) {
    for (auto& p : pending) {
        if (!p.used || !p.active || !enabled) continue;
        if (now - p.sentAt < retryTimeout(p)) continue;
        if (p.retries >= maxRetries) { p.active = false; continue; }
        ++p.retries;
        ++cntRelayRetries;
        p.sentAt = now;
        enqueueModule(p.req);
        if (trace) {
            hal::log("P %7lu = relay retry %u to %02X i%u c%u\n", (unsigned long)now, p.retries,
                     handsetSide(p.dev), p.index, p.req.b[6]);
        }
    }
}

// returns false if the answer is a duplicate and must not reach the handset
bool answerFromModule(const Frame& f) {
    if (f.type() != type::ParamEntry || f.size < 8) return true;
    Pending* pp = pendingSlot(f.b[idx::src], false);
    if (!pp) return true;
    Pending& p = *pp;
    const uint8_t index = f.b[5];
    const uint8_t rem = f.b[6];
    // exact duplicate (same entry, same chunk) of an answer we already passed on
    if (p.dupCount && index == p.dupIndex && rem == p.dupRem && int32_t(p.dupUntil - gNow) > 0) {
        --p.dupCount;
        ++cntDupDropped;
        if (trace) hal::log("P %7lu = duplicate i%u rem%u dropped\n", (unsigned long)gNow, index, rem);
        return false;
    }
    if (p.active && index == p.index) {
        if (p.retries == 0) {
            const uint32_t rtt = gNow - p.sentAt;
            p.rtt = p.rtt ? (p.rtt * 3 + rtt) / 4 : rtt;
        } else {
            p.dupIndex = index;
            p.dupRem = rem;
            p.dupCount = p.retries;
            p.dupUntil = gNow + 3000;
        }
        p.active = false;
    }
    return true; // answer, unsolicited entry (e.g. command progress) or new
}

// handset side -> module: rewrite destination address of extended frames
void forwardPacket(Frame f) {
    if (!enabled) return;
    if (f.extended()) {
        if (f.b[idx::dest] == txAddr) {
            f.b[idx::dest] = addr::TX;
        } else if (f.b[idx::dest] == rxAddr) {
            f.b[idx::dest] = addr::RX;
        }
        f.fixCrc();
        const uint8_t dev = f.b[idx::dest];
        if (dev != addr::Broadcast) {
            if (f.type() == type::ParamRead && f.size >= 8) {
                trackRequest(dev, f);
            } else if (f.type() == type::ParamWrite) {
                if (Pending* p = pendingSlot(dev, false)) p->active = false; // writes are never repeated
            }
        }
        enqueueModule(f);
    } else {
        // Link / Channels: activateLinkStats(false), activateChannels(false) -> not forwarded
        if ((f.type() != type::Link) && (f.type() != type::Channels)) {
            enqueueModule(f);
        }
    }
}

void rewriteName(Frame& f) {
    if (!doRewriteName || f.type() != type::Info) return;
    bool end = false;
    for (uint8_t i = 0; i < 16; ++i) {
        const uint8_t p = idx::payload + i;
        if (p >= f.size - 1) break;
        if (f.b[p] == '\0') break;
        if (!end && (i < sizeof(settings.txname))) {
            if (settings.txname[i] == '\0') {
                f.b[p] = '.';
                end = true;
            } else {
                f.b[p] = uint8_t(settings.txname[i]);
            }
        } else {
            f.b[p] = '.';
        }
    }
}

// 1.0.9: device info of an mLRS TX module: report the parameters of the mLRS proxy
void patchMlrsInfo(Frame& f) {
    if (f.type() != type::Info) return;
    uint8_t i = idx::payload;
    while (i < f.size - 1 && f.b[i] != 0) ++i;
    ++i; // serial number follows the name
    if (i + 14 > f.size - 1) return;
    const bool isMlrs = f.b[i] == 'm' && f.b[i + 1] == 'L' && f.b[i + 2] == 'R' && f.b[i + 3] == 'S';
    mlrs::onDeviceInfo(isMlrs, gNow);
    if (!isMlrs) return;
    f.b[i + 12] = mlrs::fieldCount();
    f.fixCrc();
}

void rewriteIncoming(Frame f) {
    if (f.b[idx::src] == addr::TX) {
        patchMlrsInfo(f);
        f.b[idx::src] = txAddr;
        rewriteName(f);
    } else if (f.b[idx::src] == addr::RX) {
        f.b[idx::src] = rxAddr;
    }
    if (f.b[idx::dest] == addr::TX) {
        f.b[idx::dest] = addr::Handset;
    }
    f.b[0] = SYNC;
    f.fixCrc();
    sendToHandset(f);
}

void tunnel(const Frame& f, uint8_t subType) {
    Frame t;
    Builder w(t, type::PassThru);
    w.u8(addr::Handset);
    w.u8(settings.address);
    w.u16(passthru::AppIdTelem);
    w.u8(subType);
    w.bytes(&f.b[idx::type], f.size - 3); // type .. last payload byte (without CRC)
    if (w.finish()) {                      // RP2040 port: drop instead of overflowing
        sendToHandset(t);
    }
}

// 1.0.8: "uplink fps" of LinkStatsTx is in units of 10 Hz (EdgeTX: TFPS). The rf profile of
// LinkStats is no rate: CRSF enum 0..3 = 4/50/150/250 Hz, mLRS uses 19/31/111 for its 19/31/111 Hz
// modes. Unknown values (e.g. ELRS rate indices) give 0 instead of a wrong rate.
uint8_t fpsTenth(uint8_t rfProfile) {
    switch (rfProfile) {
    case 0: return 0;
    case 1: return 5;
    case 2: return 15;
    case 3: return 25;
    case 19: return 2;
    case 31: return 3;
    case 111: return 11;
    default: return 0;
    }
}

void transformLinkStats(const Frame& f) {
    if (f.size < 3 + 10 + 1) return;
    const uint8_t* p = &f.b[idx::type + 1];
    const uint8_t up_rssi1    = p[0];
    const uint8_t up_rssi2    = p[1];
    const uint8_t up_link_qly = p[2];
    const uint8_t up_snr      = p[3];
    const uint8_t rf_profile  = p[5];
    const uint8_t up_rf_power = p[6];
    const uint8_t dn_rssi     = p[7];
    const uint8_t dn_link_qly = p[8];
    const uint8_t dn_snr      = p[9];
    Frame rx;
    {
        Builder w(rx, type::LinkStatsRx);
        w.u8(dn_rssi);
        w.u8(dn_link_qly);
        w.u8(dn_link_qly);
        w.u8(dn_snr);
        w.u8(up_rf_power);
        w.finish();
    }
    sendToHandset(rx);
    Frame tx;
    {
        Builder w(tx, type::LinkStatsTx);
        w.u8(std::min(up_rssi1, up_rssi2));
        w.u8(up_link_qly);
        w.u8(up_link_qly);
        w.u8(up_snr);
        w.u8(0);
        w.u8(fpsTenth(rf_profile));
        w.finish();
    }
    sendToHandset(tx);
}

// module side -> handset
// RP2040 port: ArduPilot passthrough (0x80) is handled as telemetry: paused during
// parameter access, forward rate, and its own menu entry. The original treated it as an
// extended frame (forwarded always, bytes 3/4 rewritten as if they were addresses).
// Subtypes: F0 single packet (AppId 0x50xx), F1 status text, F2 multi packet.
bool shouldSendArduPilot(const Frame& f) {
    if (f.size < 5) return false;
    const uint8_t sub = f.b[3];
    uint8_t* c = nullptr;
    if (sub == 0xF0 && f.size >= 7) {
        c = &apCounter[f.b[4] & 0x0F];
    } else if (sub == 0xF1) {
        return true; // status texts are rare and important: never thinned out
    } else if (sub == 0xF2) {
        c = &apCounter[16];
    } else {
        c = &apCounter[17];
    }
    if (++*c >= forwardRate) {
        *c = 0;
        return true;
    }
    return false;
}

void handleArduPilot(const Frame& f) {
    if (telemOff || apMode == 0) return;
    if (!shouldSendArduPilot(f)) return;
    if (apMode == 2) {
        tunnel(f, passthru::SubTelemetry);
    } else {
        sendToHandset(f);
    }
}

void handleIncoming(const Frame& f) {
    if (f.type() == type::MBridgeToRadio) { // 1.0.9: mLRS TX module behind the relay
        mlrs::onModuleFrame(f, gNow);
        return;
    }
    if (f.type() == type::ArduPilot) {
        handleArduPilot(f);
        return;
    }
    if (f.extended()) {
        if (f.type() != type::RadioID) {
            traceReply(f);
            if (answerFromModule(f)) rewriteIncoming(f);
        }
        return;
    }
    const uint8_t t = f.type();
    if (t >= type::Ping) return;
    if (telemOff) return;
    if (t == type::Link) {
        if (linkStatMode > 0 && shouldSendTelemetry(t)) {
            if (linkStatMode == 1) {
                sendToHandset(f);
            } else if (linkStatMode == 2) {
                transformLinkStats(f);
            } else if (linkStatMode == 3) {
                tunnel(f, passthru::SubLinkStat);
            }
        }
    } else {
        const uint8_t mode = fwdMode[t];
        if (mode > 0 && shouldSendTelemetry(t)) {
            if (mode == 2) {
                tunnel(f, passthru::SubTelemetry);
            } else if (mode == 1) {
                sendToHandset(f);
            }
        }
    }
}

void tick(uint32_t now) {
    gNow = now;
    retryTick(now);
    if (telemOff && (now - telemOffSince >= autoOnMs)) {
        telemOff = false;
        hal::log("# telemetry enable\n");
    }
    if (now - lastUpdate >= updateMs) {
        lastUpdate = now;
        // RP2040 port: channels are only sent once real channel data was received
        // (the original sent centered values before the first frame)
        if (source && enabled && haveChannels) {
            chanPending = true;
        }
    }
}

void buildChannels(Frame& f) {
    Builder w(f, type::Channels);
    w.bytes(channels, sizeof(channels));
    w.finish();
}
} // namespace relay

// ---------------------------------------------------------------- device / menu (CrsfCallback)
uint8_t ownAddress = 0xCD;
char devName[32] = {};
char wdgString[16] = {};
char retryString[12] = {};
char voltString[12] = {};
char tempString[12] = {};

void updateName() {
    std::snprintf(devName, sizeof(devName), "%s", DEVICE_TITLE);
}

enum PType : uint8_t { U8 = 0, Sel = 9, Str = 10, Folder = 11, Info = 12, Command = 13 };

struct Param {
    uint8_t parent = 0;
    uint8_t type = U8;
    const char* name = nullptr;
    const char* options = nullptr; // Sel: options, Info: text
    uint8_t* value = nullptr;
    uint8_t min = 0;
    uint8_t max = 0;
    bool (*cb)(uint8_t) = nullptr;
    const char* unit = nullptr;
    char* str = nullptr;           // Str: value
    uint8_t strMax = 0;            // Str: max. length
};

constexpr uint8_t maxParams = 48;
Param params[maxParams];
uint8_t nParams = 0;

uint8_t add(const Param& p) {
    if (nParams < maxParams) params[nParams++] = p;
    return uint8_t(nParams - 1);
}

// 1.0.9: one switch for all telemetry types instead of one entry per type (menu variant B).
// Link statistics frames of the module (0x1C/0x1D, e.g. sent by mLRS) belong to "LinkStat":
// they are passed on only with LinkStat = Forward, otherwise they would duplicate the
// transformed values.
void applyTelemetryModes() {
    // the telemetry types of the former per-type menu (original list)
    static constexpr uint8_t types[] = {0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
                                        0x10, 0x1e, 0x1f, 0x21, 0x22};
    for (auto& m : relay::fwdMode) m = 0;
    for (auto t : types) relay::fwdMode[t] = settings.telemetry_mode;
    const uint8_t link = (settings.link_stat_mode == 1) ? settings.telemetry_mode : 0;
    relay::fwdMode[type::LinkStatsRx] = link;
    relay::fwdMode[type::LinkStatsTx] = link;
}

void buildMenu() {
    nParams = 0;
    Param p;
    p = {}; p.type = Folder; p.name = "root"; add(p);
    // RP2040 port, 1.0.8: info block first; entries not needed for the own use case were removed
    // (version HW/SW, addresses, name rewrite, failsafe, telemetry pause) - see Settings::fixHidden()
    p = {}; p.type = Info; p.name = "Firmware"; p.options = FW_VERSION; add(p);
    p = {}; p.type = Info; p.name = "Watchdog"; p.options = wdgString; add(p);
    // parameter requests repeated by the relay since power-up (idea from the original, SW 20)
    p = {}; p.type = Info; p.name = "Total Retries"; p.options = retryString; add(p);
#if HAS_VSENSE
    p = {}; p.type = Info; p.name = "Supply Volt."; p.options = voltString; add(p);
#endif
    p = {}; p.type = Info; p.name = "Chip Temp."; p.options = tempString; add(p);
    p = {}; p.type = Sel; p.name = "LinkStat"; p.options = "Off;Forward;Transform;Tunnel"; p.value = &settings.link_stat_mode; p.max = 3;
    p.cb = [](uint8_t v) { relay::linkStatMode = v; applyTelemetryModes(); return true; }; add(p);
    p = {}; p.type = Sel; p.name = "Telemetry"; p.options = "Off;Forward"; p.value = &settings.telemetry_mode; p.max = 1;
    p.cb = [](uint8_t) { applyTelemetryModes(); return true; }; add(p);
    p = {}; p.type = U8; p.name = "Forward Rate"; p.value = &settings.telemetry_rate; p.min = 1; p.max = 10;
    p.cb = [](uint8_t a) { relay::forwardRate = a; return true; }; add(p);
    p = {}; p.type = Sel; p.name = "ArduPilot"; p.options = "Off;Forward;Tunnel";
    p.value = &settings.ardupilot_mode; p.max = 2;
    p.cb = [](uint8_t v) { relay::apMode = v; return true; }; add(p);

    p = {}; p.type = Sel; p.name = "Half-Duplex"; p.options = "Off;On"; p.value = &settings.half_duplex; p.max = 1;
    p.cb = [](uint8_t v) { relay::half = (v == 1); hal::relaySetHalfDuplex(relay::half); return true; }; add(p);
    p = {}; p.type = Sel; p.name = "Baudrate"; p.options = "400k;420k;460K;921K;1,8M"; p.value = &settings.baudrate; p.max = 4;
    p.cb = [](uint8_t v) { hal::relaySetBaud(relayBaudrates[v]); return true; }; add(p);
    // 1.0.8: signal polarity of the relay side; mLRS expects inverted CRSF at 400k/921k/1.87M
    p = {}; p.type = Sel; p.name = "Inverted"; p.options = "Off;On"; p.value = &settings.relay_inverted; p.max = 1;
    p.cb = [](uint8_t v) { hal::relaySetInverted(v == 1); return true; }; add(p);
    p = {}; p.type = Sel; p.name = "Own Telemetry"; p.options = "Off;On"; p.value = &settings.own_telemetry; p.max = 1;
    p.cb = [](uint8_t) { return true; }; add(p);
}

uint8_t numberOfParameters() { return uint8_t(nParams - 1); }

void refreshInfo() {
    std::snprintf(wdgString, sizeof(wdgString), "%lu", (unsigned long)hal::watchdogResets());
    std::snprintf(retryString, sizeof(retryString), "%lu", (unsigned long)relay::cntRelayRetries);
    const float v = hal::supplyVoltage();
    if (v < 0.0f) {
        std::snprintf(voltString, sizeof(voltString), "-");
    } else {
        std::snprintf(voltString, sizeof(voltString), "%.2fV", double(v));
    }
    std::snprintf(tempString, sizeof(tempString), "%.1fC", double(hal::chipTemperature()));
}

struct Buf {
    uint8_t d[256];
    uint16_t n = 0;
    void u8(uint8_t v) { if (n < sizeof(d)) d[n++] = v; }
    void str(const char* s) { if (s) while (*s) u8(uint8_t(*s++)); u8(0); }
};

void serialize(uint8_t index, Buf& c) {
    const Param& p = params[index];
    c.u8(p.parent);
    c.u8(p.type);
    c.str(p.name);
    switch (p.type & 0x7f) {
    case U8:
        c.u8(p.value ? *p.value : 0);
        c.u8(p.min);
        c.u8(p.max);
        c.u8(p.min); // default (= min in the original)
        c.str(p.unit);
        break;
    case Sel:
        c.str(p.options);
        c.u8(p.value ? *p.value : 0);
        c.u8(p.min);
        c.u8(p.max);
        c.u8(p.min);
        c.str(p.unit);
        break;
    case Str:
        c.str(p.str);
        c.u8(p.strMax);
        break;
    case Folder:
        for (uint8_t i = 0; i < nParams; ++i) {
            if (params[i].parent == index && !(index == 0 && i == 0)) c.u8(i);
        }
        c.u8(0xff);
        break;
    case Info:
        c.str(p.options);
        break;
    default:
        break;
    }
}

void sendParameterInfo(uint8_t dest, uint8_t index, uint8_t chunk) {
    if (index > numberOfParameters()) {
        hal::log("# wrong param number %u\n", index);
        return;
    }
    refreshInfo();
    Buf b;
    serialize(index, b);
    constexpr uint16_t perChunk = 58 - 4;
    uint8_t chunks = uint8_t((b.n + perChunk - 1) / perChunk);
    if (chunks == 0) chunks = 1;
    if (chunk >= chunks) return;
    const uint16_t start = uint16_t(chunk * perChunk);
    const uint16_t len = std::min<uint16_t>(perChunk, uint16_t(b.n - start));
    Frame f;
    Builder w(f, type::ParamEntry);
    w.u8(dest);
    w.u8(ownAddress);
    w.u8(index);
    w.u8(uint8_t(chunks - 1 - chunk));
    w.bytes(&b.d[start], len);
    w.finish();
    sendToHandset(f);
}

void setParameterValue(uint8_t index, const uint8_t* data, uint8_t n) {
    hal::log("# SetPV i: %u\n", index);
    if (index == 0 || index >= nParams) return;
    Param& p = params[index];
    bool save = true;
    if (p.type == Str) {
        if (p.str && p.strMax) {
            uint8_t i = 0;
            for (; i < n && i < p.strMax - 1 && data[i] != '\0'; ++i) p.str[i] = char(data[i]);
            for (; i < p.strMax; ++i) p.str[i] = '\0';
        }
    } else if (p.type == U8 || p.type == Sel) {
        if (n < 1 || !p.value) return;
        // like the original (since 2026-11-08): out of range values are rejected, not saved
        const uint8_t v = data[0];
        if (v < p.min || v > p.max) {
            hal::log("# SetPV not saved: %u\n", index);
            return;
        }
        *p.value = v;
        if (p.cb) save = p.cb(v);
    } else {
        return;
    }
    if (save) hal::requestSave();
    updateName();
}

void callbacks() {
    for (uint8_t i = 0; i < nParams; ++i) {
        const Param& p = params[i];
        if (p.type != Command && p.cb && p.value) p.cb(*p.value);
    }
}

void sendDeviceInfo(uint8_t dest) {
    refreshInfo();
    Frame f;
    Builder w(f, type::Info);
    w.u8(dest);
    w.u8(ownAddress);
    w.str(devName);
    w.u32(SERIAL_NUMBER);
    w.u32(HW_VERSION);
    w.u32(SW_VERSION);
    w.u8(numberOfParameters());
    w.u8(0); // protocol version
    w.finish();
    sendToHandset(f);
}

// ---------------------------------------------------------------- input side (Crsf Master::analyze)
uint16_t channelPackets = 0;
uint16_t linkPackets = 0;

void analyzeInput(const Frame& f, uint32_t now) {
    switch (f.type()) {
    case type::Link:
        ++linkPackets;
        break;
    case type::Channels:
        if (f.length() >= ChannelsPayload + 2) {
            ++channelPackets;
            ++cntChannelsIn;
            std::memcpy(relay::channels, &f.b[3], ChannelsPayload);
            relay::haveChannels = true;
        }
        break;
    case type::Ping:
        relay::disableTelemetryWithAutoOn(now);
        if (f.size >= 6) {
            const uint8_t dest = f.b[idx::dest];
            if (dest == addr::Broadcast || dest == relay::txAddr) mlrs::onPing(now);
            if (dest == addr::Broadcast || dest == ownAddress) {
                sendDeviceInfo(f.b[idx::src]);
                traceRequest(f, false);
                relay::forwardPacket(f);
            }
        }
        break;
    case type::ParamRead:
        relay::disableTelemetryWithAutoOn(now);
        if (f.size >= 8 && f.b[idx::dest] == ownAddress) {
            traceRequest(f, true);
            sendParameterInfo(f.b[idx::src], f.b[5], f.b[6]);
        } else if (f.size >= 8 && f.b[idx::dest] == relay::txAddr && mlrs::active()) {
            traceRequest(f, true); // 1.0.9: answered by the mLRS proxy
            mlrs::paramRead(f.b[idx::src], f.b[idx::dest], f.b[5], f.b[6]);
        } else {
            traceRequest(f, false);
            relay::forwardPacket(f);
        }
        break;
    case type::ParamWrite:
        relay::disableTelemetryWithAutoOn(now);
        if (f.size >= 8 && f.b[idx::dest] == ownAddress) {
            // no command entries in this menu
            const uint8_t n = uint8_t(f.length() - 5); // value bytes
            traceRequest(f, true);
            setParameterValue(f.b[5], &f.b[6], n);
        } else if (f.size >= 8 && f.b[idx::dest] == relay::txAddr && mlrs::active()) {
            traceRequest(f, true); // 1.0.9: mLRS proxy
            mlrs::paramWrite(f.b[idx::src], f.b[idx::dest], f.b[5], &f.b[6], uint8_t(f.length() - 5), now);
        } else {
            traceRequest(f, false);
            relay::forwardPacket(f);
        }
        break;
    case type::Command:
        relay::forwardPacket(f);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- state machine (GFSM)
enum class State : uint8_t { Undefined, Init, Run, UnConnected };
enum class Event : uint8_t { None, ConnectionLost, DirectConnected, ReceiverConnected };
State state = State::Undefined;
Event event = Event::None;
uint32_t stateSince = 0;
uint32_t lastCheck = 0;
uint32_t lastOwnTelem = 0;
constexpr uint32_t initMs = 500;
constexpr uint32_t checkMs = 300;

void sendSetBaudrate() {
    // CRSF general command "speed proposal" to the relay side
    // RP2040 port: proposes the configured rate (original: always 400k) and adds the
    // command CRC (poly 0xBA) required by the CRSF spec for 0x32 frames
    Frame f;
    Builder w(f, type::Command);
    w.u8(addr::Broadcast);
    w.u8(addr::RX);
    w.u8(0x0A); // general
    w.u8(0x70); // speed
    w.u8(0);    // port
    w.u32(relayBaudrates[settings.baudrate]);
    const uint8_t cmdCrc = crc8(&f.b[idx::type], w.size() - 2, 0xBA);
    w.u8(cmdCrc);
    w.finish();
    relay::enqueueModule(f);
}

void checkPackages(uint32_t now) {
    if (now - lastCheck < checkMs) return;
    lastCheck = now;
    const uint16_t ch = channelPackets;
    const uint16_t lp = linkPackets;
    channelPackets = linkPackets = 0;
    if (ch > 0) {
        event = (lp == 0) ? Event::DirectConnected : Event::ReceiverConnected;
    } else {
        event = Event::ConnectionLost;
    }
    if (hal::relayFrameErrors() > 0) {
        hal::log("# send baudrate\n");
        sendSetBaudrate();
    }
}

// 1.1.1: failsafe with an mLRS TX module. mLRS keeps transmitting the last channel values when its
// CRSF input stops, so "stop sending" does not reach the model. Instead the relay does what EdgeTX
// does when the receiver ID is changed: it selects another mLRS config (other bind phrase), the
// link to the model breaks and the receiver goes into its failsafe. When receiver 1 is back, the
// original config is selected again. mLRS applies a change only after 0.75 s, so short dropouts
// cause nothing. The channels keep being sent, so the mLRS TX keeps its baud rate after restarting.
namespace mlrsfs {
bool on = false;          // failsafe config selected
uint8_t original = 0;     // config id to restore
uint8_t target = 0;       // config id to send
uint8_t sendLeft = 0;     // repetitions left
uint32_t tLast = 0;

void select(uint8_t id) {
    target = id;
    sendLeft = 3;
    tLast = 0;
    hal::log("# mLRS: select config %u\n", unsigned(id));
}

void sendModelSelect(uint8_t id) {
    Frame f;
    Builder w(f, type::Command);
    w.u8(addr::TX);
    w.u8(addr::Handset);
    w.u8(0x10); // CRSF command
    w.u8(0x05); // set model selection
    w.u8(id);
    const uint8_t cmdCrc = crc8(&f.b[idx::type], w.size() - 2, 0xBA);
    w.u8(cmdCrc);
    w.finish();
    relay::enqueueModule(f);
}

void tick(uint32_t now) {
    if (!sendLeft || (tLast && now - tLast < 100)) return;
    tLast = now ? now : 1;
    --sendLeft;
    sendModelSelect(target);
}

uint8_t failsafeId() { return uint8_t((mlrs::configId() + 1) % mlrs::ConfigNum); }
} // namespace mlrsfs

void enter(State s, uint32_t now) {
    state = s;
    stateSince = now;
    switch (s) {
    case State::Undefined:
        break;
    case State::Init:
        hal::log("# Init\n");
        hal::led(hal::Led::Init);
        break;
    case State::UnConnected:
        hal::log("# UnCon\n");
        if (mlrs::active() && mlrs::configKnown() && relay::enabled) {
            mlrsfs::on = true; // relay stays enabled, see mlrsfs
            mlrsfs::original = mlrs::configId();
            mlrsfs::select(mlrsfs::failsafeId());
        } else if (settings.failsafe_mode == 0) {
            relay::enabled = false;
            relay::chanPending = false;
        }
        hal::led(hal::Led::UnConnected);
        break;
    case State::Run:
        hal::log("# Run\n");
        relay::enabled = true;
        if (mlrsfs::on) {
            mlrsfs::on = false;
            mlrsfs::select(mlrsfs::original);
        }
        hal::led(hal::Led::Run);
        break;
    }
}

void fsm(uint32_t now) {
    checkPackages(now);
    switch (state) {
    case State::Undefined:
        if (now - stateSince >= initMs) enter(State::Init, now);
        break;
    case State::Init:
        if (event == Event::ReceiverConnected || event == Event::DirectConnected) { event = Event::None; enter(State::Run, now); }
        else if (event == Event::ConnectionLost) { event = Event::None; enter(State::UnConnected, now); }
        break;
    case State::UnConnected:
        if (event == Event::ReceiverConnected || event == Event::DirectConnected) { event = Event::None; enter(State::Run, now); }
        break;
    case State::Run:
        if (event == Event::ConnectionLost) { event = Event::None; enter(State::UnConnected, now); }
        break;
    }
}

void ownTelemetry(uint32_t now) {
    if (now - lastOwnTelem < 1000) return;
    lastOwnTelem = now;
    if (!settings.own_telemetry || relay::telemOff) return;
    // source id = own CRSF address (>= 128: EdgeTX shows 0x0E as voltage, not as cells)
    const float v = HAS_VSENSE ? hal::supplyVoltage() : -1.0f;
    if (v >= 0.0f) {
        Frame f;
        Builder w(f, type::Cells);
        w.u8(settings.address);
        w.u16(uint16_t(v * 1000.0f + 0.5f)); // mV
        w.finish();
        sendToHandset(f);
    }
    Frame t;
    Builder w(t, type::Temp);
    w.u8(settings.address);
    w.u16(uint16_t(int16_t(hal::chipTemperature() * 10.0f))); // 0.1 degree C
    w.finish();
    sendToHandset(t);
}

} // namespace

// ---------------------------------------------------------------- public API
void init(uint32_t now) {
    relay::enabled = false; // like the original (since 2026-11-08): relay active only in state Run
    mlrs::init({&sendToHandset, &relay::enqueueModule, &hal::log});
    settings.fixHidden();   // 1.0.8: values without menu entry are fixed, stored values are ignored
    relay::txAddr = settings.tx_rewrite_address;
    relay::rxAddr = settings.rx_rewrite_address;
    relay::doRewriteName = false;
    relay::autoOnMs = uint32_t(settings.tempOffTelemetry) * 1000u;
    ownAddress = settings.address;
    updateName();
    buildMenu();
    callbacks(); // updateFromEeprom(): apply all stored settings
    state = State::Undefined;
    mlrsfs::on = false;
    mlrsfs::sendLeft = 0;
    stateSince = now;
    lastCheck = now;
    relay::lastUpdate = now;
    lastOwnTelem = now;
    hal::led(hal::Led::Off);
}

void onInputFrame(const Frame& f, uint32_t now) {
    traceNow = now;
    relay::gNow = now;
    ++cntInFrames;
    analyzeInput(f, now);
}

void onRelayFrame(const Frame& f, uint32_t now) {
    traceNow = now;
    relay::gNow = now;
    ++cntRelayFrames;
    relay::handleIncoming(f);
}

void tick(uint32_t now) {
    relay::tick(now);
    mlrsfs::tick(now);
    mlrs::tick(now, relay::enabled && !mlrsfs::on, mlrsfs::on);
    fsm(now);
    ownTelemetry(now);
}

bool nextToHandset(Frame& f) {
    if (toHandset.pop(f)) {
        ++cntToHandset;
        return true;
    }
    return false;
}

size_t nextRelayBurst(uint8_t* buf, size_t cap) {
    size_t n = 0;
    auto append = [&](const Frame& f) {
        if (n + f.size <= cap) {
            std::memcpy(buf + n, f.b, f.size);
            n += f.size;
        }
    };
    if (relay::chanPending) {
        relay::chanPending = false;
        // 1.0.12: an mLRS TX module starts its reply 250 us after the first frame of a burst, so a
        // frame appended to the channels would collide with it. Like EdgeTX with mLRS: a queued
        // frame is sent instead of the channels of this period (at most every second period).
        static bool skipped = false;
        if (relay::half && mlrs::active() && !skipped) {
            Frame o;
            if (toModule.pop(o)) {
                append(o);
                skipped = true;
                return n;
            }
        }
        skipped = false;
        Frame c;
        relay::buildChannels(c);
        append(c);
        if (relay::half && mlrs::active()) return n; // nothing appended for mLRS
        // channels + at most one queued frame per period (same in both modes)
        Frame o;
        if (toModule.pop(o)) append(o);
        return n;
    }
    // RP2040 port: in half-duplex mode queued frames are only sent directly behind
    // a channels frame (like EdgeTX does), so they cannot collide with the module's reply
    if (!relay::half) {
        Frame o;
        if (toModule.pop(o)) append(o);
    }
    return n;
}

bool halfDuplex() { return relay::half; }

void setTrace(bool on) { trace = on; }
bool tracing() { return trace; }

Stats stats() {
    Stats s{};
    s.inFrames = cntInFrames;
    s.relayFrames = cntRelayFrames;
    s.channelsIn = cntChannelsIn;
    s.toModule = cntToModule;
    s.toHandset = cntToHandset;
    s.droppedToModule = toModule.dropped;
    s.droppedToHandset = toHandset.dropped;
    s.state = uint8_t(state);
    s.relayEnabled = relay::enabled;
    s.telemetryOff = relay::telemOff;
    s.haveChannels = relay::haveChannels;
    s.relayRetries = relay::cntRelayRetries;
    s.duplicatesDropped = relay::cntDupDropped;
    return s;
}

const char* stateName() {
    switch (state) {
    case State::Undefined: return "Start";
    case State::Init: return "Init";
    case State::Run: return "Run";
    case State::UnConnected: return "UnConnected";
    }
    return "?";
}

const char* deviceName() { return devName; }

} // namespace core
