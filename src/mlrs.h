/*
 * CRSF-Relay for RP2040 - mLRS parameter proxy (1.0.9)
 * GPL-3.0-or-later, see crsf.h
 *
 * An mLRS TX module behind the relay answers the device ping of the ELRS Lua script, but reports
 * 0 parameters: its own Lua script uses mBridge commands in CRSF frames 0x81/0x82 instead of the
 * CRSF parameter protocol, and those frames cannot pass the ELRS link in front of the relay.
 *
 * The relay therefore loads the mLRS parameters itself via mBridge (like mLRS.lua does) and
 * answers the parameter requests of the ELRS Lua script for this device:
 *   radio --ELRS Lua--> relay  (CRSF parameter protocol, device address = TX rewrite address)
 *   relay --mBridge---> mLRS TX (REQUEST_INFO, REQUEST_CMD/PARAM_ITEM, PARAM_SET, PARAM_STORE)
 * mLRS protocol reference: mLRS v1.4.04, Common/protocols/mbridge_protocol.h, lua/mLRS.lua
 */
#pragma once

#include <cstdint>
#include "crsf.h"

namespace mlrs {

struct Io {
    void (*toHandset)(const crsf::Frame&);
    void (*toModule)(const crsf::Frame&);
    void (*log)(const char* fmt, ...);
};

void init(const Io& io);
void reset();

// CRSF frame 0x82 from the module side (mBridge answers, mLRS statistics)
void onModuleFrame(const crsf::Frame& f, uint32_t now);
// device info of the module seen (serial number "mLRS" identifies an mLRS TX module)
void onDeviceInfo(bool isMlrs, uint32_t now);

bool active();          // an mLRS TX module was detected behind the relay
bool loaded();          // parameter list complete
uint8_t fieldCount();   // number of parameters reported to the ELRS Lua script

// device ping from the radio (ELRS Lua started): refresh the parameter list
void onPing(uint32_t now);
// parameter protocol for the proxied device; dev = address the Lua script uses (TX rewrite address)
void paramRead(uint8_t handset, uint8_t dev, uint8_t index, uint8_t chunk);
void paramWrite(uint8_t index, const uint8_t* data, uint8_t n, uint32_t now);

// call periodically; 'linkUp' = relay is sending to the module
void tick(uint32_t now, bool linkUp);

// diagnostics
const char* stateName();
uint8_t paramsLoaded();
uint8_t paramNum();
uint32_t requests();
uint32_t retries();

// mBridge command ids (subset)
namespace mb {
constexpr uint8_t RequestInfo  = 3;
constexpr uint8_t DeviceItemTx = 4;
constexpr uint8_t DeviceItemRx = 5;
constexpr uint8_t ParamItem    = 7;
constexpr uint8_t ParamItem2   = 8;
constexpr uint8_t ParamItem3_4 = 9;
constexpr uint8_t RequestCmd   = 10;
constexpr uint8_t Info         = 11;
constexpr uint8_t ParamSet     = 12;
constexpr uint8_t ParamStore   = 13;
constexpr uint8_t CmdStx       = 0xA0;
uint8_t payloadLen(uint8_t cmd);
}

} // namespace mlrs
