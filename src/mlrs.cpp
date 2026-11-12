/*
 * CRSF-Relay for RP2040 - mLRS parameter proxy (1.0.9), see mlrs.h
 * GPL-3.0-or-later, see crsf.h
 */
#include "mlrs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mlrs {

using namespace crsf;

namespace mb {
uint8_t payloadLen(uint8_t cmd) {
    switch (cmd) {
    case 2: return 22;                 // TX_LINK_STATS
    case DeviceItemTx: return 24;
    case DeviceItemRx: return 24;
    case ParamItem: return 24;
    case ParamItem2: return 24;
    case ParamItem3_4: return 24;
    case RequestCmd: return 18;
    case Info: return 24;
    case ParamSet: return 7;
    case 16: return 3;                 // MODELID_SET
    default: return 0;                 // REQUEST_INFO, PARAM_STORE, BIND_START/STOP, ...
    }
}
} // namespace mb

namespace {

constexpr uint8_t FrameMBridgeToModule = 0x81;
constexpr uint8_t FrameMBridgeToRadio  = 0x82;

// mBridge parameter types
constexpr uint8_t MbUint8 = 0, MbInt8 = 1, MbUint16 = 2, MbInt16 = 3, MbList = 4, MbStr6 = 5;
// CRSF parameter types (ELRS Lua)
constexpr uint8_t TU8 = 0, TI8 = 1, TSel = 9, TFolder = 11, TInfo = 12, TCommand = 13, Hidden = 0x80;

constexpr uint8_t MaxParams = 72;
constexpr uint8_t MaxFields = MaxParams + 8;
constexpr uint32_t RequestTimeoutMs = 500;
constexpr uint32_t InfoTimeoutMs = 3000;
constexpr uint32_t RetryLaterMs = 5000;
constexpr uint32_t SaveReloadMs = 3000;   // like paramLoadDeadTime in mLRS.lua
constexpr uint32_t SaveMaxMs = 15000;
constexpr uint8_t MaxRequestTries = 8;

struct MParam {
    bool valid = false;
    uint8_t type = 0;
    char name[17] = {};
    uint8_t val[6] = {};
    int8_t min = 0, max = 0;
    char unit[7] = {};
    uint16_t allowed = 0xffff;
    char opts[72] = {};
};

struct DevItem {
    bool valid = false;
    uint16_t version = 0;
    char name[21] = {};
};

enum class St : uint8_t { Off, ReqInfo, WaitInfo, Params, Done, Failed };
enum Kind : uint8_t { KStatus, KInfoTx, KInfoRx, KFolderTx, KFolderRx, KFolderFs, KParam, KSave };

struct Field {
    uint8_t kind = KStatus;
    uint8_t parent = 0;
    uint8_t mi = 0;
};

Io io{};
bool detected = false;
St st = St::Off;
uint32_t tState = 0;       // state entered / last request
uint8_t tries = 0;
uint8_t idx = 0;           // parameter being loaded
uint8_t expect = 0;        // 0 = ITEM, 1 = ITEM2, 2 = ITEM3, 3 = ITEM4
uint8_t paramNumInfo = 0;  // from INFO (0 = unknown)
uint8_t nParams = 0;       // parameters of the last complete list
uint32_t doneAt = 0;
uint32_t reloadAt = 0;
bool saving = false;
uint32_t saveSince = 0;
bool rxAvailable = false;
DevItem devTx, devRx;
MParam P[MaxParams];
Field F[MaxFields];        // field id n is F[n - 1]
uint8_t nF = 0;
bool structured = false;
uint32_t cntRequests = 0, cntRetries = 0;

void log(const char* s, unsigned a = 0, unsigned b = 0) {
    if (io.log) io.log(s, a, b);
}

// ---------------------------------------------------------------- mBridge to the module
void sendCmd(uint8_t cmd, const uint8_t* payload, uint8_t n) {
    if (!io.toModule) return;
    Frame f;
    Builder w(f, FrameMBridgeToModule);
    w.u8('O');
    w.u8('W');
    w.u8(uint8_t(mb::CmdStx + cmd));
    const uint8_t len = mb::payloadLen(cmd);
    for (uint8_t i = 0; i < len; ++i) w.u8(i < n ? payload[i] : 0);
    w.finish();
    f.b[idx::sync] = addr::TX; // mLRS accepts 0x81 only with address 0xEE
    io.toModule(f);
    ++cntRequests;
}

void requestInfo(uint32_t now) {
    sendCmd(mb::RequestInfo, nullptr, 0);
    tState = now;
}

void requestParam(uint32_t now) {
    const uint8_t p[2] = {mb::ParamItem, idx};
    sendCmd(mb::RequestCmd, p, 2);
    expect = 0;
    tState = now;
}

void startLoad(uint32_t now) {
    st = St::ReqInfo;
    tries = 0;
    devTx.valid = devRx.valid = false;
    requestInfo(now);
    log("# mLRS: load\n");
}

// ---------------------------------------------------------------- helpers
bool hasPrefix(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }
bool isTx(const MParam& p) { return hasPrefix(p.name, "Tx"); }
bool isRx(const MParam& p) { return hasPrefix(p.name, "Rx"); }
bool isFs(const MParam& p) { return hasPrefix(p.name, "Rx FS Ch"); }

void copyStr(char* dst, size_t cap, const uint8_t* src, size_t max) {
    size_t i = 0;
    for (; i < max && i + 1 < cap && src[i]; ++i) dst[i] = char(src[i]);
    dst[i] = '\0';
}
size_t strnlen8(const uint8_t* s, size_t max) {
    size_t i = 0;
    while (i < max && s[i]) ++i;
    return i;
}

uint8_t optionCount(const MParam& p) {
    if (!p.opts[0]) return 0;
    uint8_t n = 1;
    for (const char* c = p.opts; *c; ++c) if (*c == ',') ++n;
    return n;
}
// option text number i (copied to out)
void optionText(const MParam& p, uint8_t i, char* out, size_t cap) {
    const char* c = p.opts;
    for (uint8_t k = 0; k < i && *c; ++c) if (*c == ',') ++k;
    size_t n = 0;
    while (*c && *c != ',' && n + 1 < cap) out[n++] = *c++;
    out[n] = '\0';
}
bool optionAllowed(const MParam& p, uint8_t v) {
    if (v >= optionCount(p)) return false;
    char t[24];
    optionText(p, v, t, sizeof t);
    if (std::strcmp(t, "-") == 0) return false;
    if (p.allowed == 0xffff) return true;
    return (p.allowed & (1u << v)) != 0;
}
// like mb_allowed_mask_editable() in mLRS.lua: none or a single option -> not editable
bool editable(const MParam& p) {
    if (p.allowed == 0xffff) return optionCount(p) > 1;
    return (p.allowed & (p.allowed - 1)) != 0;
}

void versionStr(uint16_t v, char* out, size_t cap) {
    std::snprintf(out, cap, "v%u.%u.%02u", unsigned(v >> 12), unsigned((v >> 6) & 0x3f), unsigned(v & 0x3f));
}

uint8_t addField(uint8_t kind, uint8_t parent, uint8_t mi = 0) {
    if (nF >= MaxFields) return 0;
    F[nF].kind = kind;
    F[nF].parent = parent;
    F[nF].mi = mi;
    return ++nF; // field id
}

void buildFields() {
    nF = 0;
    addField(KInfoTx, 0);
    addField(KInfoRx, 0);
    for (uint8_t i = 0; i < nParams; ++i) {
        if (!isTx(P[i]) && !isRx(P[i])) addField(KParam, 0, i);
    }
    const uint8_t ftx = addField(KFolderTx, 0);
    for (uint8_t i = 0; i < nParams; ++i) {
        if (isTx(P[i])) addField(KParam, ftx, i);
    }
    const uint8_t frx = addField(KFolderRx, 0);
    uint8_t ffs = 0;
    for (uint8_t i = 0; i < nParams; ++i) {
        if (!isRx(P[i])) continue;
        if (isFs(P[i])) {
            if (!ffs) ffs = addField(KFolderFs, frx);
            addField(KParam, ffs, i);
        } else {
            addField(KParam, frx, i);
        }
    }
    addField(KSave, 0);
    structured = true;
}

void finish(uint32_t now) {
    nParams = idx;
    st = St::Done;
    doneAt = now;
    buildFields();
    if (saving) saving = false;
    log("# mLRS: %u params loaded\n", nParams);
}

void paramComplete(uint32_t now) {
    ++idx;
    if (idx >= MaxParams || (paramNumInfo && idx >= paramNumInfo)) {
        finish(now);
    } else {
        tries = 0;
        requestParam(now);
    }
}

// ---------------------------------------------------------------- CRSF parameter entries
struct Buf {
    uint8_t d[200];
    uint16_t n = 0;
    void u8(uint8_t v) { if (n < sizeof(d)) d[n++] = v; }
    void str(const char* s) { if (s) while (*s) u8(uint8_t(*s++)); u8(0); }
};

const char* shortName(const MParam& p) {
    if (isFs(p)) return p.name + 6;           // "Rx FS Ch1" -> "Ch1"
    if ((isTx(p) || isRx(p)) && p.name[2] == ' ') return p.name + 3;
    return p.name;
}

void serializeParam(const Field& fl, Buf& c) {
    const MParam& p = P[fl.mi];
    const bool hide = !p.valid || (isRx(p) && !rxAvailable) || (p.type == MbList && p.allowed == 0);
    const uint8_t h = hide ? Hidden : 0;
    char t[32];
    switch (p.type) {
    case MbList:
        if (editable(p)) {
            c.u8(uint8_t(TSel | h));
            c.str(shortName(p));
            for (const char* o = p.opts; *o; ++o) c.u8(*o == ',' ? ';' : uint8_t(*o));
            c.u8(0);
            c.u8(p.val[0]);
            c.u8(0);
            c.u8(uint8_t(optionCount(p) - 1));
            c.u8(0);
            c.str("");
        } else {
            optionText(p, p.val[0], t, sizeof t);
            c.u8(uint8_t(TInfo | h));
            c.str(shortName(p));
            c.str(t);
        }
        break;
    case MbInt8:
    case MbUint8:
        c.u8(uint8_t((p.type == MbInt8 ? TI8 : TU8) | h));
        c.str(shortName(p));
        c.u8(p.val[0]);
        c.u8(uint8_t(p.min));
        c.u8(uint8_t(p.max));
        c.u8(uint8_t(p.min));
        c.str(p.unit);
        break;
    case MbStr6:
        copyStr(t, 7, p.val, 6);
        c.u8(uint8_t(TInfo | h)); // string editing is not implemented in the ELRS Lua script
        c.str(shortName(p));
        c.str(t);
        break;
    default:
        c.u8(uint8_t(TInfo | h));
        c.str(shortName(p));
        c.str("?");
        break;
    }
}

void serializeFolder(uint8_t id, const char* name, Buf& c) {
    c.u8(TFolder);
    c.str(name);
    for (uint8_t i = 0; i < nF; ++i) {
        if (F[i].parent == id) c.u8(uint8_t(i + 1));
    }
    c.u8(0xff);
}

void serialize(uint8_t id, Buf& c) {
    char t[40], v[12];
    if (id == 0) {
        c.u8(0);
        if (structured) {
            serializeFolder(0, "root", c);
        } else {
            c.u8(TFolder);
            c.str("root");
            c.u8(1);
            c.u8(0xff);
        }
        return;
    }
    if (!structured) { // only a status line until the list is loaded
        c.u8(0);
        c.u8(TInfo);
        c.str("mLRS");
        if (st == St::Failed) {
            std::snprintf(t, sizeof t, "no answer");
        } else {
            std::snprintf(t, sizeof t, "loading %u/%u", unsigned(idx), unsigned(paramNumInfo));
        }
        c.str(t);
        return;
    }
    const Field& fl = F[id - 1];
    c.u8(fl.parent);
    switch (fl.kind) {
    case KInfoTx:
        versionStr(devTx.version, v, sizeof v);
        std::snprintf(t, sizeof t, "%s %s", devTx.name, v);
        c.u8(TInfo); c.str("TX"); c.str(t);
        break;
    case KInfoRx:
        if (rxAvailable && devRx.valid && devRx.name[0]) {
            versionStr(devRx.version, v, sizeof v);
            std::snprintf(t, sizeof t, "%s %s", devRx.name, v);
        } else {
            std::snprintf(t, sizeof t, "not connected");
        }
        c.u8(TInfo); c.str("RX"); c.str(t);
        break;
    case KFolderTx: serializeFolder(id, "Tx", c); break;
    case KFolderRx: serializeFolder(id, rxAvailable ? "Rx" : "Rx (no receiver)", c); break;
    case KFolderFs: serializeFolder(id, "Failsafe", c); break;
    case KParam: serializeParam(fl, c); break;
    case KSave:
        c.u8(TCommand);
        c.str("Save");
        c.u8(saving ? 2 : 0);   // executing / ready
        c.u8(50);               // poll every 0.5 s
        c.str(saving ? "saving..." : "");
        break;
    default:
        c.u8(TInfo); c.str("?"); c.str("");
        break;
    }
}

} // namespace

// ---------------------------------------------------------------- API
void init(const Io& i) {
    io = i;
    reset();
}

void reset() {
    detected = false;
    st = St::Off;
    structured = false;
    nF = 0;
    nParams = 0;
    paramNumInfo = 0;
    idx = 0;
    saving = false;
    reloadAt = 0;
    rxAvailable = false;
    devTx = DevItem{};
    devRx = DevItem{};
    for (auto& p : P) p = MParam{};
    cntRequests = cntRetries = 0;
}

void onDeviceInfo(bool isMlrs, uint32_t now) {
    if (!isMlrs) {
        if (detected) { reset(); log("# mLRS: other TX module, proxy off\n"); } // module swapped
        return;
    }
    if (detected) return;
    detected = true;
    log("# mLRS: TX module detected\n");
    (void)now;
}

void onModuleFrame(const Frame& f, uint32_t now) {
    if (f.type() != FrameMBridgeToRadio || f.size < 5) return;
    if (!detected) {
        detected = true;
        log("# mLRS: TX module detected\n");
    }
    const uint8_t c = f.b[3];
    if ((c & 0xe0) != mb::CmdStx) return; // mLRS statistics (0x65), envelopes (0x66)
    const uint8_t cmd = c & 0x1f;
    const uint8_t* d = &f.b[4];
    const size_t n = size_t(f.size - 5); // without CRC
    if (n < mb::payloadLen(cmd)) return;
    switch (cmd) {
    case mb::DeviceItemTx:
    case mb::DeviceItemRx: {
        DevItem& it = (cmd == mb::DeviceItemTx) ? devTx : devRx;
        it.valid = true;
        it.version = uint16_t(d[0] | (d[1] << 8));
        copyStr(it.name, sizeof it.name, &d[4], 20);
        if (cmd == mb::DeviceItemTx && st == St::ReqInfo) {
            st = St::WaitInfo;
            tState = now;
        }
        break;
    }
    case mb::Info:
        rxAvailable = (d[5] & 0x01) != 0;
        paramNumInfo = d[8];
        if (st == St::ReqInfo || st == St::WaitInfo) {
            st = St::Params;
            idx = 0;
            tries = 0;
            requestParam(now);
        }
        break;
    case mb::ParamItem: {
        if (st != St::Params) break;
        const uint8_t i = d[0];
        if (i == 0xff) { finish(now); break; }
        if (i != idx || i >= MaxParams) break;
        MParam& p = P[i];
        p.type = d[1];
        copyStr(p.name, sizeof p.name, &d[2], 16);
        std::memcpy(p.val, &d[18], 6);
        p.opts[0] = '\0';
        p.unit[0] = '\0';
        p.allowed = 0xffff;
        p.valid = true;
        expect = 1;
        tState = now;
        break;
    }
    case mb::ParamItem2: {
        if (st != St::Params || expect != 1 || d[0] != idx) break;
        MParam& p = P[idx];
        tState = now;
        if (p.type == MbList) {
            p.allowed = uint16_t(d[1] | (d[2] << 8));
            copyStr(p.opts, sizeof p.opts, &d[3], 21);
            if (strnlen8(&d[3], 21) == 21) { expect = 2; break; }
        } else if (p.type == MbInt8 || p.type == MbUint8 || p.type == MbUint16 || p.type == MbInt16) {
            p.min = int8_t(d[1]);
            p.max = int8_t(d[3]);
            copyStr(p.unit, sizeof p.unit, &d[7], 6);
        }
        paramComplete(now);
        break;
    }
    case mb::ParamItem3_4: {
        if (st != St::Params || (expect != 2 && expect != 3)) break;
        const bool item4 = d[0] >= 128;
        if (uint8_t(d[0] & 0x7f) != idx || item4 != (expect == 3)) break;
        MParam& p = P[idx];
        tState = now;
        const size_t have = std::strlen(p.opts);
        copyStr(p.opts + have, sizeof p.opts - have, &d[1], 23);
        if (!item4 && strnlen8(&d[1], 23) == 23) { expect = 3; break; }
        paramComplete(now);
        break;
    }
    default:
        break;
    }
}

bool active() { return detected; }
bool loaded() { return structured; }
uint8_t fieldCount() { return structured ? nF : 1; }

void onPing(uint32_t now) {
    if (!detected || saving) return;
    if (st == St::Off || st == St::Failed || (st == St::Done && now - doneAt > SaveReloadMs)) {
        reloadAt = now; // started in tick()
    }
}

void paramRead(uint8_t handset, uint8_t dev, uint8_t index, uint8_t chunk) {
    if (!io.toHandset || index > fieldCount()) return;
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
    w.u8(handset);
    w.u8(dev);
    w.u8(index);
    w.u8(uint8_t(chunks - 1 - chunk));
    w.bytes(&b.d[start], len);
    w.finish();
    io.toHandset(f);
}

void paramWrite(uint8_t index, const uint8_t* data, uint8_t n, uint32_t now) {
    if (!structured || index == 0 || index > nF || n < 1) return;
    const Field& fl = F[index - 1];
    if (fl.kind == KSave) {
        if (data[0] == 1 && !saving) {        // lcsClick
            sendCmd(mb::ParamStore, nullptr, 0);
            saving = true;
            saveSince = now;
            reloadAt = now + SaveReloadMs;
            log("# mLRS: store\n");
        } else if (data[0] == 5) {            // lcsCancel
            saving = false;
        }
        return;
    }
    if (fl.kind != KParam) return;
    MParam& p = P[fl.mi];
    if (!p.valid || (isRx(p) && !rxAvailable)) return;
    uint8_t v = data[0];
    if (p.type == MbList) {
        if (!editable(p) || !optionAllowed(p, v)) return;
    } else if (p.type == MbInt8) {
        if (int8_t(v) < p.min || int8_t(v) > p.max) return;
    } else if (p.type == MbUint8) {
        if (v < uint8_t(p.min) || v > uint8_t(p.max)) return;
    } else {
        return;
    }
    p.val[0] = v;
    const uint8_t set[7] = {fl.mi, v, 0, 0, 0, 0, 0};
    sendCmd(mb::ParamSet, set, sizeof set);
    log("# mLRS: set %u = %u\n", fl.mi, v);
}

void tick(uint32_t now, bool linkUp) {
    if (!detected) return;
    if (saving && now - saveSince > SaveMaxMs) saving = false;
    if (!linkUp) return;
    if (reloadAt && int32_t(now - reloadAt) >= 0 && st != St::ReqInfo && st != St::WaitInfo && st != St::Params) {
        reloadAt = 0;
        startLoad(now);
        return;
    }
    if (st == St::Off) {
        startLoad(now);
        return;
    }
    switch (st) {
    case St::ReqInfo:
        if (now - tState >= RequestTimeoutMs) {
            if (++tries > 2 * MaxRequestTries) { st = St::Failed; tState = now; log("# mLRS: no answer\n"); break; }
            ++cntRetries;
            requestInfo(now);
        }
        break;
    case St::WaitInfo:
        // with a receiver connected the module first fetches the receiver setup over the air
        if (now - tState >= InfoTimeoutMs) { st = St::ReqInfo; requestInfo(now); }
        break;
    case St::Params:
        if (now - tState >= RequestTimeoutMs) {
            if (++tries > MaxRequestTries) { st = St::Failed; tState = now; log("# mLRS: param %u failed\n", idx); break; }
            ++cntRetries;
            requestParam(now);
        }
        break;
    case St::Failed:
        if (now - tState >= RetryLaterMs) startLoad(now);
        break;
    default:
        break;
    }
}

const char* stateName() {
    switch (st) {
    case St::Off: return detected ? "detected" : "off";
    case St::ReqInfo: return "request info";
    case St::WaitInfo: return "wait info";
    case St::Params: return "loading";
    case St::Done: return "loaded";
    case St::Failed: return "failed";
    }
    return "?";
}
uint8_t paramsLoaded() { return st == St::Params ? idx : nParams; }
uint8_t paramNum() { return paramNumInfo; }
uint32_t requests() { return cntRequests; }
uint32_t retries() { return cntRetries; }

} // namespace mlrs
