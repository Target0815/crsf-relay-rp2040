/*
 * CRSF-Relay for RP2040 - build configuration
 * GPL-3.0-or-later, see crsf.h
 */
#pragma once

#define FW_NAME      "crsf-relay"
#define FW_VERSION   "1.1.1"

// 1.0.13: device name in the ELRS Lua script / TBS Agent: "RC-Relay <chip>" (max. 15 characters
// in EdgeTX). Up to 1.0.12 it was "RC-Relay32@<address>" like the original.
#if defined(PICO_RP2350) && PICO_RP2350
#define CHIP_NAME "RP2350"
#else
#define CHIP_NAME "RP2040"
#endif
#ifndef DEVICE_TITLE
#define DEVICE_TITLE "RC-Relay " CHIP_NAME
#endif
#define HW_VERSION    4u     // 1 WeAct G031, 2 WMG0B1, 3 Nucleo G431, 4 RP2040
#define SW_VERSION    15u    // functional level of the original firmware
#define SERIAL_NUMBER 1234u

// ---- pins (defaults for Waveshare RP2040-Zero, override with -D...)
#ifndef PIN_IN_TX
#define PIN_IN_TX     0   // UART0 TX  -> RX of receiver #1
#endif
#ifndef PIN_IN_RX
#define PIN_IN_RX     1   // UART0 RX  <- TX of receiver #1
#endif
#ifndef PIN_RELAY_TX
#define PIN_RELAY_TX  4   // UART1 TX  -> RX of module #2; half duplex: the single signal line
#endif
#ifndef PIN_RELAY_RX
#define PIN_RELAY_RX  5   // UART1 RX  <- TX of module #2 (full duplex only)
#endif
#ifndef PIN_WS2812
#define PIN_WS2812    16  // on-board RGB LED of the RP2040-Zero
#endif
#ifndef PIN_VSENSE
#define PIN_VSENSE    29  // ADC3, supply voltage via divider
#endif

// 1.0.11: supply voltage measurement only if a divider is fitted (otherwise the open pin gives
// random values). 0 = no menu entry "Supply Volt." and no voltage in "Own Telemetry".
#ifndef HAS_VSENSE
#define HAS_VSENSE    0
#endif

// voltage divider for PIN_VSENSE: Vin -- R1 -- PIN -- R2 -- GND
#ifndef VSENSE_R1
#define VSENSE_R1     33.0f // kOhm
#endif
#ifndef VSENSE_R2
#define VSENSE_R2     10.0f // kOhm
#endif

#define LED_BRIGHTNESS 40   // 0..255
#define WATCHDOG_MS    500
