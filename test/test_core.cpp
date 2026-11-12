// Host tests for the protocol core: g++ -std=c++17 -I../src test_core.cpp ../src/core.cpp
#include "core.h"
#include "hal.h"
#include "config.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <cstddef>

using namespace crsf;

// ------------------------------------------------------------------ HAL stub
namespace stub {
uint32_t baud = 0;
bool half = false;
bool inverted = false;
uint32_t frameErrors = 0;
hal::Led led = hal::Led::Off;
int saves = 0;
bool verbose = false;
}
namespace hal {
void relaySetBaud(uint32_t b) { stub::baud = b; }
void relaySetHalfDuplex(bool on) { stub::half = on; }
void relaySetInverted(bool on) { stub::inverted = on; }
uint32_t relayFrameErrors() { auto e = stub::frameErrors; stub::frameErrors = 0; return e; }
void led(Led p) { stub::led = p; }
void requestSave() { ++stub::saves; }
float supplyVoltage() { return 12.34f; }
float chipTemperature() { return 31.5f; }
uint32_t watchdogResets() { return 2; }
void log(const char* fmt, ...) {
    if (!stub::verbose) return;
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
}
}

// ------------------------------------------------------------------ helpers
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Frame make(uint8_t t, std::vector<uint8_t> payload, uint8_t sync = SYNC) {
    Frame f;
    Builder w(f, t);
    for (auto b : payload) w.u8(b);
    w.finish();
    f.b[0] = sync;
    return f;
}
static std::vector<Frame> drainHandset() {
    std::vector<Frame> v; Frame f;
    while (core::nextToHandset(f)) v.push_back(f);
    return v;
}
static std::vector<Frame> splitBurst(const uint8_t* b, size_t n) {
    std::vector<Frame> v; Parser p;
    for (size_t i = 0; i < n; ++i) p.push(b[i]);
    Frame f;
    while (p.next(f)) v.push_back(f);
    return v;
}
static std::vector<Frame> drainRelay(uint32_t& now, int periods = 1) {
    std::vector<Frame> all;
    uint8_t buf[256];
    for (int i = 0; i < periods; ++i) {
        now += 10;
        core::tick(now);
        size_t n;
        while ((n = core::nextRelayBurst(buf, sizeof buf)) > 0) {
            auto v = splitBurst(buf, n);
            all.insert(all.end(), v.begin(), v.end());
        }
    }
    return all;
}
static Frame channelsFrame(uint16_t v) {
    // pack 16 x 11 bit
    uint8_t p[22] = {};
    uint32_t bits = 0; int nb = 0, k = 0;
    for (int ch = 0; ch < 16; ++ch) {
        bits |= uint32_t(v & 0x7ff) << nb; nb += 11;
        while (nb >= 8) { p[k++] = uint8_t(bits); bits >>= 8; nb -= 8; }
    }
    return make(type::Channels, std::vector<uint8_t>(p, p + 22));
}
static Frame linkFrame() {
    return make(type::Link, {50, 60, 100, 9, 0, 4, 2, 70, 99, 8});
}
static void reconnect(uint32_t& now);
static std::string cstr(const uint8_t* p, size_t max, size_t* used = nullptr) {
    std::string s; size_t i = 0;
    for (; i < max && p[i]; ++i) s += char(p[i]);
    if (used) *used = i + 1;
    return s;
}

// read one parameter like elrsV3.lua does (all chunks)
struct ParamInfo { uint8_t parent, type; std::string name; std::vector<uint8_t> rest; };
static bool readParam(uint32_t now, uint8_t index, ParamInfo& out) {
    std::vector<uint8_t> data;
    uint8_t chunk = 0;
    for (;;) {
        core::onInputFrame(make(type::ParamRead, {0xCD, addr::Handset, index, chunk}), now);
        auto v = drainHandset();
        if (v.size() != 1) return false;
        const Frame& f = v[0];
        if (!f.crcOk() || f.type() != type::ParamEntry) return false;
        if (f.b[3] != addr::Handset || f.b[4] != 0xCD || f.b[5] != index) return false;
        const uint8_t follow = f.b[6];
        data.insert(data.end(), &f.b[7], &f.b[f.size - 1]);
        if (follow == 0) break;
        ++chunk;
        if (chunk > 10) return false;
    }
    out.parent = data[0];
    out.type = data[1];
    size_t used;
    out.name = cstr(&data[2], data.size() - 2, &used);
    out.rest.assign(data.begin() + 2 + used, data.end());
    return true;
}

static void reconnect(uint32_t& now) {
    for (int i = 0; i < 40; ++i) {
        core::onInputFrame(channelsFrame(1000), now);
        if (i % 4 == 0) core::onInputFrame(linkFrame(), now);
        drainRelay(now);
    }
    drainHandset();
}

// ------------------------------------------------------------------ tests
int main(int argc, char**) {
    stub::verbose = argc > 1;
    uint32_t now = 1000;
    core::settings.defaults();
    core::init(now);

    // --- settings applied at start
    CHECK(stub::baud == 921'600);
    CHECK(!stub::half);
    CHECK(std::string(core::deviceName()) == "RC-Relay32@205");

    // --- parser: garbage, split frames, CRC error, resync
    {
        Parser p; Frame f;
        auto a = channelsFrame(992), b = linkFrame();
        std::vector<uint8_t> s = {0x00, 0xC8, 0x99, 0x12};
        s.insert(s.end(), a.b, a.b + a.size);
        auto bad = b; bad.b[5] ^= 0xff;
        s.insert(s.end(), bad.b, bad.b + bad.size);
        s.insert(s.end(), b.b, b.b + b.size);
        int got = 0;
        for (auto c : s) { p.push(c); while (p.next(f)) { ++got; CHECK(f.crcOk()); } }
        CHECK(got == 2);
        CHECK(p.crcErrors >= 1);
    }

    // --- no channels before the first channels frame (safety)
    {
        auto v = drainRelay(now, 5);
        CHECK(v.empty());
    }

    // --- ping broadcast -> device info + forwarded
    {
        core::onInputFrame(make(type::Ping, {addr::Broadcast, addr::Handset}), now);
        auto h = drainHandset();
        CHECK(h.size() == 1);
        const Frame& f = h[0];
        CHECK(f.type() == type::Info && f.crcOk());
        CHECK(f.b[3] == addr::Handset && f.b[4] == 0xCD);
        size_t used;
        CHECK(cstr(&f.b[5], 40, &used) == "RC-Relay32@205");
        const uint8_t* q = &f.b[5 + used];
        const uint32_t serial = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
        const uint32_t hw = (q[4] << 24) | (q[5] << 16) | (q[6] << 8) | q[7];
        CHECK(serial == 1234 && hw == 4);
        CHECK(q[12] == 31); // number of parameters
        CHECK(drainRelay(now).empty()); // relay disabled before state Run
    }

    // --- read the complete menu like the Lua script
    std::map<std::string, uint8_t> index;
    {
        int folders = 0, sels = 0;
        for (uint8_t i = 1; i <= 31; ++i) {
            ParamInfo pi;
            const bool ok = readParam(now, i, pi);
            CHECK(ok);
            if (!ok) break;
            index[pi.name] = i;
            if (pi.type == 11) ++folders;
            if (pi.type == 9) ++sels;
            if (stub::verbose) printf("  %2u parent %2u type %2u %s\n", i, pi.parent, pi.type, pi.name.c_str());
        }
        CHECK(folders == 1);
        CHECK(sels == 19 + 5); // LinkStat, Half-Duplex, Baudrate, Inverted, Own Telemetry
        ParamInfo pi;
        CHECK(!readParam(now, 32, pi)); // out of range -> no answer
        // root folder lists its children, without itself
        ParamInfo root;
        CHECK(readParam(now, 0, root));
        CHECK(root.type == 11 && root.rest.back() == 0xff);
        CHECK(root.rest[0] == 1);
        // telemetry folder has 20 children (incl. ArduPilot)
        ParamInfo tf;
        CHECK(readParam(now, index["Telemetry forwarding"], tf));
        CHECK(tf.rest.size() == 20 + 1);
        // selection layout: options, value, min, max, default, unit
        ParamInfo ls;
        CHECK(readParam(now, index["LinkStat"], ls));
        size_t used;
        CHECK(cstr(ls.rest.data(), ls.rest.size(), &used) == "Off;Forward;Transform;Tunnel");
        CHECK(ls.rest[used] == 2 && ls.rest[used + 2] == 3);
        // info entries
        ParamInfo vi;
        CHECK(readParam(now, index["Supply Volt."], vi));
        CHECK(cstr(vi.rest.data(), vi.rest.size()) == "12.34V");
        // RP2040 hint with firmware version is the first entry
        ParamInfo fw;
        CHECK(readParam(now, 1, fw));
        CHECK(fw.type == 12 && fw.name == "RP2040 Firmware" && cstr(fw.rest.data(), fw.rest.size()) == FW_VERSION);
        // 1.0.8: removed entries
        for (const char* n : {"Version(HW/SW)", "CRSF Address", "TX Rewrite Address", "RX Rewrite Address",
                              "TX Rewrite Name", "Rewrite Name", "Failsafe", "Telem. Off Dur."}) {
            CHECK(index.count(n) == 0);
        }
        // info block directly after the firmware entry
        CHECK(index["Watchdog"] == 2 && index["Total Retries"] == 3 && index["Supply Volt."] == 4 && index["Chip Temp."] == 5);
    }

    // --- parameter write: baudrate, half duplex, string, address
    {
        const int s0 = stub::saves;
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Baudrate"], 4}), now);
        CHECK(stub::baud == 1'870'000);
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Half-Duplex"], 1}), now);
        CHECK(stub::half && core::halfDuplex());
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Forward Rate"], 99}), now);
        CHECK(core::settings.telemetry_rate == 2); // out of range: rejected
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Forward Rate"], 2}), now);
        CHECK(stub::saves == s0 + 3);
        // 1.0.8: relay side polarity
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Inverted"], 1}), now);
        CHECK(stub::inverted && core::settings.relay_inverted == 1);
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Inverted"], 2}), now);
        CHECK(stub::inverted); // out of range: rejected
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Inverted"], 0}), now);
        CHECK(!stub::inverted);
        CHECK(std::string(core::deviceName()) == "RC-Relay32@205");
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Half-Duplex"], 0}), now);
        drainHandset();
        drainRelay(now, 2);
    }

    // --- direct connection (channels, no link stats) -> Run
    {
        for (int i = 0; i < 100; ++i) { core::onInputFrame(channelsFrame(992), now); drainRelay(now); }
        CHECK(std::string(core::stateName()) == "Run");
    }

    // --- ping is forwarded in state Run
    {
        core::onInputFrame(make(type::Ping, {addr::Broadcast, addr::Handset}), now);
        drainHandset();
        bool ping = false;
        for (auto& f : drainRelay(now, 2)) if (f.type() == type::Ping && f.crcOk()) ping = true;
        CHECK(ping);
    }

    // --- param read for another device is rewritten and forwarded (with trace output)
    {
        core::setTrace(true);
        const bool v = stub::verbose; stub::verbose = true;
        core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, 3, 0}), now);
        core::onInputFrame(make(type::ParamWrite, {0xCF, addr::Handset, 3, 1}), now);
        std::vector<Frame> r;
        for (auto& f : drainRelay(now, 2)) if (f.type() != type::Channels) r.push_back(f);
        CHECK(r.size() == 2);
        if (r.size() != 2) r.resize(2);
        CHECK(r[0].b[3] == addr::TX && r[0].crcOk());
        CHECK(r[1].b[3] == addr::RX && r[1].crcOk());
        core::onInputFrame(make(type::ParamRead, {0xCF, addr::Handset, 3, 1}), now);
        core::onInputFrame(make(type::ParamRead, {0xCF, addr::Handset, 3, 1}), now + 5000);
        core::onRelayFrame(make(type::ParamEntry, {addr::Handset, addr::RX, 3, 0, 1, 2}), now + 5100);
        drainHandset();
        drainRelay(now, 3);
        stub::verbose = v;
        core::setTrace(false);
    }

    // --- relay repeats lost parameter reads, drops the resulting duplicates
    {
        drainHandset();
        drainRelay(now, 3);
        // keep receiver #1 "connected" while time passes
        auto drainRelay = [&](uint32_t& t, int periods) {
            std::vector<Frame> all;
            for (int i = 0; i < periods; ++i) {
                core::onInputFrame(channelsFrame(992), t);
                auto v = ::drainRelay(t, 1);
                for (auto& f : v) if (f.type() != type::Channels) all.push_back(f);
            }
            return all;
        };
        auto countReads = [](const std::vector<Frame>& v, uint8_t dest, uint8_t i) {
            int n = 0;
            for (auto& f : v) if (f.type() == type::ParamRead && f.b[3] == dest && f.b[5] == i) ++n;
            return n;
        };
        auto entry = [](uint8_t src, uint8_t i) {
            return make(type::ParamEntry, {addr::Handset, src, i, 0, 0, 9, 'X', 0});
        };
        // TX: answered in time -> no retry
        core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, 5, 0}), now);
        auto r = drainRelay(now, 1);
        CHECK(countReads(r, addr::TX, 5) == 1);
        core::onRelayFrame(entry(addr::TX, 5), now);
        CHECK(drainHandset().size() == 1);
        r = drainRelay(now, 30);
        CHECK(countReads(r, addr::TX, 5) == 0);

        // TX: answer lost -> relay repeats after >= 100 ms
        core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, 6, 0}), now);
        r = drainRelay(now, 5);
        CHECK(countReads(r, addr::TX, 6) == 1);
        r = drainRelay(now, 6); // 110 ms after the request
        CHECK(countReads(r, addr::TX, 6) == 1); // one retry
        // late original answer + answer to the retry: only one reaches the handset
        core::onRelayFrame(entry(addr::TX, 6), now);
        core::onRelayFrame(entry(addr::TX, 6), now);
        CHECK(drainHandset().size() == 1);
        // unsolicited entries (command progress) still pass
        core::onRelayFrame(entry(addr::TX, 9), now);
        core::onRelayFrame(entry(addr::TX, 9), now);
        CHECK(drainHandset().size() == 2);

        // chunked entry: a late duplicate of chunk 0 must not swallow chunk 1
        {
            auto chunk = [](uint8_t i, uint8_t rem) {
                return make(type::ParamEntry, {addr::Handset, addr::TX, i, rem, 0, 9, 'Y', 0});
            };
            core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, 12, 0}), now);
            drainRelay(now, 11);                         // one retry
            core::onRelayFrame(chunk(12, 1), now);       // answer to chunk 0
            CHECK(drainHandset().size() == 1);
            core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, 12, 1}), now);
            core::onRelayFrame(chunk(12, 1), now);       // late duplicate of chunk 0: dropped
            CHECK(drainHandset().empty());
            core::onRelayFrame(chunk(12, 0), now);       // chunk 1 passes
            auto h = drainHandset();
            CHECK(h.size() == 1 && h[0].b[6] == 0);
            drainRelay(now, 30);
            CHECK(core::stats().relayRetries == 2);      // no retry for chunk 1
        }

        // RX: slow link, gives up after 3 retries
        core::onInputFrame(make(type::ParamRead, {0xCF, addr::Handset, 7, 0}), now);
        r = drainRelay(now, 400); // 4 s
        CHECK(countReads(r, addr::RX, 7) == 1 + 3);
        // flight controller behind receiver #2 (ArduPilot, 0xC8): retried as well
        core::onInputFrame(make(type::ParamRead, {0xC8, addr::Handset, 5, 0}), now);
        r = drainRelay(now, 100); // 1 s: retry after 3 x 300 ms (no RTT measured yet)
        CHECK(countReads(r, 0xC8, 5) == 2);
        core::onRelayFrame(make(type::ParamEntry, {addr::Handset, 0xC8, 5, 0, 0, 9, 'F', 0}), now);
        core::onRelayFrame(make(type::ParamEntry, {addr::Handset, 0xC8, 5, 0, 0, 9, 'F', 0}), now);
        auto fc = drainHandset();
        CHECK(fc.size() == 1 && fc[0].b[4] == 0xC8);
        r = drainRelay(now, 100);
        CHECK(countReads(r, 0xC8, 5) == 0);
        // writes are never repeated
        core::onInputFrame(make(type::ParamWrite, {0xCF, addr::Handset, 7, 1}), now);
        r = drainRelay(now, 300);
        int writes = 0;
        for (auto& f : r) if (f.type() == type::ParamWrite) ++writes;
        CHECK(writes == 1);
        CHECK(core::stats().relayRetries == 6);
        ParamInfo tr;
        CHECK(readParam(now, index["Total Retries"], tr));
        CHECK(tr.type == 12 && cstr(tr.rest.data(), tr.rest.size()) == "6");
        CHECK(core::stats().duplicatesDropped == 3);
        drainHandset();
    }

    // --- channels are relayed every 10 ms with identical payload
    {
        auto c = channelsFrame(1500);
        core::onInputFrame(c, now);
        auto r = drainRelay(now, 3);
        CHECK(r.size() == 3);
        for (auto& f : r) CHECK(f.type() == type::Channels && std::memcmp(f.b, c.b, c.size) == 0);
    }

    // --- state machine: receiver connected -> Run
    {
        for (int i = 0; i < 40; ++i) {
            core::onInputFrame(channelsFrame(1000), now);
            if (i % 4 == 0) core::onInputFrame(linkFrame(), now);
            drainRelay(now);
        }
        CHECK(std::string(core::stateName()) == "Run");
        CHECK(stub::led == hal::Led::Run);
    }

    // --- incoming from module: source rewrite + name rewrite
    {
        drainHandset();
        uint32_t later = now + 40'000; // telemetry auto on again
        core::tick(later); now = later;
        auto info = make(type::Info, {addr::Handset, addr::TX, 'E', 'L', 'R', 'S', ' ', 'T', 'X', ' ', 'M', 'o', 'd', 0, 0, 0, 0, 1});
        core::onRelayFrame(info, now);
        auto h = drainHandset();
        CHECK(h.size() == 1 && h[0].b[4] == 0xCE && h[0].crcOk());
        CHECK(cstr(&h[0].b[5], 20) == "ELRS TX Mod"); // rewrite name is off
        // RX source, dest TX -> handset
        core::onRelayFrame(make(type::ParamEntry, {addr::TX, addr::RX, 1, 0, 0}), now);
        h = drainHandset();
        CHECK(h.size() == 1 && h[0].b[3] == addr::Handset && h[0].b[4] == 0xCF);
        // RadioID is dropped
        core::onRelayFrame(make(type::RadioID, {addr::Handset, addr::TX, 0x10, 0, 0, 0, 0, 0, 0, 0, 0}), now);
        CHECK(drainHandset().empty());
    }

    // --- telemetry: link stats transform, forward rate 2, tunnel
    {
        core::onRelayFrame(linkFrame(), now);
        core::onRelayFrame(linkFrame(), now);
        auto h = drainHandset();
        CHECK(h.size() == 2);
        CHECK(h[0].type() == type::LinkStatsRx && h[1].type() == type::LinkStatsTx);
        CHECK(h[0].b[3] == 70 && h[0].b[4] == 99 && h[0].b[7] == 2);
        CHECK(h[1].b[3] == 50 && h[1].b[4] == 100 && h[1].b[8] == 0); // rf profile 4: unknown rate -> 0
        // 1.0.8: mLRS 31 Hz mode (rf profile 31) -> TFPS 3 (x10 Hz)
        core::tick(now += 1000);
        drainHandset();
        core::onRelayFrame(make(type::Link, {50, 60, 100, 9, 0, 31, 2, 70, 99, 8}), now);
        core::onRelayFrame(make(type::Link, {50, 60, 100, 9, 0, 31, 2, 70, 99, 8}), now);
        h = drainHandset();
        CHECK(h.size() == 2 && h[1].type() == type::LinkStatsTx && h[1].b[8] == 3);

        auto bat = make(0x08, {0, 120, 0, 10, 0, 0, 100, 80});
        for (int i = 0; i < 4; ++i) core::onRelayFrame(bat, now);
        h = drainHandset();
        CHECK(h.size() == 2 && h[0].type() == 0x08);

        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Battery"], 2}), now);
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["LinkStat"], 3}), now);
        core::tick(now += 40'000);
        drainHandset();
        for (int i = 0; i < 2; ++i) core::onRelayFrame(bat, now);
        for (int i = 0; i < 2; ++i) core::onRelayFrame(linkFrame(), now);
        h = drainHandset();
        CHECK(h.size() == 2);
        CHECK(h[0].type() == type::PassThru && h[0].b[3] == addr::Handset && h[0].b[4] == 0xCD);
        CHECK(h[0].b[5] == (6200 >> 8) && h[0].b[6] == (6200 & 0xff) && h[0].b[7] == 0x02 && h[0].b[8] == 0x08);
        CHECK(h[1].b[7] == 0x01 && h[1].b[8] == type::Link && h[1].crcOk());
        // unlisted telemetry type (0x01) is not forwarded
        core::onRelayFrame(make(0x01, {1, 2, 3}), now);
        core::onRelayFrame(make(0x01, {1, 2, 3}), now);
        CHECK(drainHandset().empty());
    }

    // --- temporary telemetry off after parameter access
    {
        core::onInputFrame(make(type::ParamRead, {0xCD, addr::Handset, 1, 0}), now);
        drainHandset();
        auto bat = make(0x08, {0, 120, 0, 10, 0, 0, 100, 80});
        for (int i = 0; i < 4; ++i) core::onRelayFrame(bat, now);
        CHECK(drainHandset().empty());
        core::tick(now + 29'000);
        for (int i = 0; i < 4; ++i) core::onRelayFrame(bat, now);
        CHECK(drainHandset().empty());
        now += 30'000;
        core::tick(now);
        for (int i = 0; i < 4; ++i) core::onRelayFrame(bat, now);
        CHECK(!drainHandset().empty());
    }

    // --- half duplex: queued frames only behind a channels frame, one per period
    {
        reconnect(now);
        CHECK(std::string(core::stateName()) == "Run");
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Half-Duplex"], 1}), now);
        for (int i = 0; i < 3; ++i) core::onInputFrame(make(type::ParamRead, {0xCE, addr::Handset, uint8_t(i), 0}), now);
        uint8_t buf[256];
        CHECK(core::nextRelayBurst(buf, sizeof buf) == 0); // nothing without channel tick
        now += 10; core::tick(now);
        auto v = splitBurst(buf, core::nextRelayBurst(buf, sizeof buf));
        if (stub::verbose) for (auto& f : v) printf("  burst type %02x size %u\n", f.type(), f.size);
        CHECK(v.size() == 2 && v[0].type() == type::Channels && v[1].type() == type::ParamRead);
        CHECK(core::nextRelayBurst(buf, sizeof buf) == 0);
        auto r = drainRelay(now, 3);
        CHECK(r.size() == 3 + 2);
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Half-Duplex"], 0}), now);
        drainHandset();
    }

    // --- own telemetry
    {
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Own Telemetry"], 1}), now);
        core::tick(now += 31'000);
        drainHandset();
        core::tick(now += 1000);
        auto h = drainHandset();
        CHECK(h.size() == 2);
        CHECK(h[0].type() == type::Cells && h[0].b[3] == 0xCD && ((h[0].b[4] << 8) | h[0].b[5]) == 12340);
        CHECK(h[1].type() == type::Temp && ((h[1].b[4] << 8) | h[1].b[5]) == 315);
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["Own Telemetry"], 0}), now);
    }

    // --- frame errors -> speed command with command CRC
    {
        stub::frameErrors = 3;
        auto r = drainRelay(now, 40);
        bool found = false;
        for (auto& f : r) {
            if (f.type() == type::Command) {
                found = true;
                CHECK(f.b[5] == 0x0A && f.b[6] == 0x70);
                const uint32_t b = (f.b[8] << 24) | (f.b[9] << 16) | (f.b[10] << 8) | f.b[11];
                CHECK(b == 1'870'000);
                CHECK(crc8(&f.b[2], 10, 0xBA) == f.b[12]);
            }
        }
        CHECK(found);
    }

    // --- connection lost -> UnConnected, failsafe "Forward" stops the relay
    {
        drainRelay(now, 50);
        CHECK(std::string(core::stateName()) == "UnConnected");
        auto r = drainRelay(now, 5);
        CHECK(r.empty());
        // pings are no longer forwarded either
        core::onInputFrame(make(type::Ping, {addr::Broadcast, addr::Handset}), now);
        drainHandset();
        CHECK(drainRelay(now, 2).empty());
        // back again
        for (int i = 0; i < 40; ++i) {
            core::onInputFrame(channelsFrame(1200), now);
            if (i % 4 == 0) core::onInputFrame(linkFrame(), now);
            drainRelay(now);
        }
        CHECK(std::string(core::stateName()) == "Run");
        // failsafe is fixed to Forward (1.0.8): the relay stops when receiver 1 is lost
        drainRelay(now, 50);
        CHECK(std::string(core::stateName()) == "UnConnected");
        auto h = drainRelay(now, 5);
        bool chan = false;
        for (auto& f : h) if (f.type() == type::Channels) chan = true;
        CHECK(!chan);
    }

    // --- UnConnected -> Run with channels only (DirectConnected)
    {
        for (int i = 0; i < 40; ++i) { core::onInputFrame(channelsFrame(1100), now); drainRelay(now); }
        CHECK(std::string(core::stateName()) == "Run");
    }

    // --- ArduPilot passthrough (0x80) is telemetry, not an extended frame
    {
        reconnect(now);
        core::tick(now += 40'000);
        drainHandset();
        // F0 single packets with AppIds 0x5006 / 0x5002 alternating: rate 2 per AppId
        auto ap = [](uint8_t app) { return make(0x80, {0xF0, app, 0x50, 1, 2, 3, 4}); };
        for (int i = 0; i < 4; ++i) { core::onRelayFrame(ap(0x06), now); core::onRelayFrame(ap(0x02), now); }
        auto h = drainHandset();
        CHECK(h.size() == 4);
        int a6 = 0, a2 = 0;
        for (auto& f : h) { if (f.b[4] == 0x06) ++a6; if (f.b[4] == 0x02) ++a2; CHECK(f.crcOk()); }
        CHECK(a6 == 2 && a2 == 2);          // no AppId starves
        CHECK(h[0].b[3] == 0xF0);           // payload untouched
        // status text always passes
        auto st = make(0x80, {0xF1, 6, 'H', 'i', 0});
        core::onRelayFrame(st, now); core::onRelayFrame(st, now);
        CHECK(drainHandset().size() == 2);
        // paused during parameter access
        core::onInputFrame(make(type::ParamRead, {0xCD, addr::Handset, 1, 0}), now);
        drainHandset();
        for (int i = 0; i < 4; ++i) core::onRelayFrame(ap(0x06), now);
        core::onRelayFrame(st, now);
        CHECK(drainHandset().empty());
        core::tick(now += 31'000);
        // menu: Off / Tunnel
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["ArduPilot"], 0}), now);
        core::tick(now += 31'000);
        drainHandset();
        for (int i = 0; i < 4; ++i) core::onRelayFrame(ap(0x06), now);
        CHECK(drainHandset().empty());
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["ArduPilot"], 2}), now);
        core::tick(now += 31'000);
        drainHandset();
        for (int i = 0; i < 2; ++i) core::onRelayFrame(ap(0x06), now);
        h = drainHandset();
        CHECK(h.size() == 1 && h[0].type() == type::PassThru && h[0].b[8] == 0x80 && h[0].crcOk());
        core::onInputFrame(make(type::ParamWrite, {0xCD, addr::Handset, index["ArduPilot"], 1}), now);
        CHECK(core::settings.ardupilot_mode == 1);
        drainHandset();
    }

    // --- flash image: round trip and migration from 1.0.0-1.0.3 (without ardupilot_mode)
    {
        uint8_t img[256];
        Settings a; a.defaults(); a.address = 0xC5; a.ardupilot_mode = 2; a.baudrate = 1;
        const size_t n = store::encode(a, img);
        Settings b;
        CHECK(store::decode(img, n, b) && b.address == 0xC5 && b.ardupilot_mode == 2 && b.baudrate == 1);
        img[20] ^= 1;
        CHECK(!store::decode(img, n, b));
        // old layout: 59 bytes of settings, check at offset 68, padding byte zero
        Settings o; o.defaults(); o.address = 0xC7; o.half_duplex = 1;
        const uint16_t oldSize = 59;
        uint8_t old[72] = {};
        const uint32_t magic = store::Magic; const uint16_t ver = 1;
        std::memcpy(old, &magic, 4); std::memcpy(old + 4, &ver, 2); std::memcpy(old + 6, &oldSize, 2);
        std::memcpy(old + 8, &o, oldSize);
        const uint32_t c = store::fnv(old, 68);
        std::memcpy(old + 68, &c, 4);
        Settings m;
        CHECK(store::decode(old, sizeof(old), m));
        CHECK(m.address == 0xC7 && m.half_duplex == 1 && m.ardupilot_mode == 1);
        CHECK(offsetof(Settings, ardupilot_mode) == 59);
        CHECK(m.relay_inverted == 0); // 1.0.8 field keeps its default
        CHECK(offsetof(Settings, relay_inverted) == 60);
    }

    // --- 1.0.8: values without menu entry are fixed, stored values are ignored
    {
        Settings s; s.defaults();
        s.failsafe_mode = 1; s.address = 200; s.tx_rewrite_address = 0xC2; s.rewrite_name = 1; s.tempOffTelemetry = 5;
        s.relay_inverted = 1;
        s.fixHidden();
        CHECK(s.failsafe_mode == 0 && s.address == 0xCD && s.tx_rewrite_address == 0xCE && s.rewrite_name == 0);
        CHECK(s.tempOffTelemetry == 30 && s.relay_inverted == 1);
    }

    // --- settings plausibility
    {
        Settings s; s.defaults();
        CHECK(s.plausible());
        s.baudrate = 9;
        CHECK(!s.plausible());
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
