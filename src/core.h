/*
 * CRSF-Relay for RP2040 - protocol core (hardware independent).
 *
 * Input side  ("handset side"): CRSF receiver #1, full duplex, 420 kBaud.
 * Relay side  ("module side")  : TX module #2 (half duplex) or RX-as-TX (full duplex).
 *
 * GPL-3.0-or-later, see crsf.h
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include "crsf.h"
#include "settings.h"

namespace core {

extern Settings settings;

// call once, after the hardware ports are ready (applies all settings)
void init(uint32_t nowMs);

// a complete, CRC-checked frame from receiver #1
void onInputFrame(const crsf::Frame& f, uint32_t nowMs);
// a complete, CRC-checked frame from the relay side (module #2 / its receiver)
void onRelayFrame(const crsf::Frame& f, uint32_t nowMs);

// call frequently from the main loop
void tick(uint32_t nowMs);

// next frame to send to receiver #1
bool nextToHandset(crsf::Frame& f);
// next burst (one or more frames, back to back) for the relay side; 0 = nothing
size_t nextRelayBurst(uint8_t* buf, size_t cap);

bool halfDuplex();

// parameter trace on the debug console (timestamps of requests and replies)
void setTrace(bool on);
bool tracing();

// diagnostics
struct Stats {
    uint32_t inFrames, relayFrames;
    uint32_t channelsIn, toModule, toHandset;
    uint32_t droppedToModule, droppedToHandset;
    uint8_t state; // 0 Undefined 1 Init 2 Run 3 UnConnected
    bool relayEnabled, telemetryOff, haveChannels;
    uint32_t relayRetries, duplicatesDropped;
};
Stats stats();
const char* stateName();
const char* deviceName();

}
