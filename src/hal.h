/*
 * CRSF-Relay for RP2040 - hooks from the protocol core to the hardware.
 * Implemented in main.cpp (firmware) and test/hal_stub.cpp (host tests).
 * GPL-3.0-or-later, see crsf.h
 */
#pragma once

#include <cstdint>

namespace hal {

enum class Led : uint8_t { Off, Init, UnConnected, Run };

void relaySetBaud(uint32_t baud);
void relaySetHalfDuplex(bool on);
void relaySetInverted(bool on); // 1.0.8
uint32_t relayFrameErrors(); // read and reset
void led(Led pattern);
void requestSave();          // settings changed, store to flash soon
float supplyVoltage();       // volts, < 0 if unavailable
float chipTemperature();     // degree C
uint32_t watchdogResets();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}
