/*
 * CRSF-Relay for RP2040
 *
 * Port of RC-Relay32 by Wilhelm Meier (https://github.com/wimalopaan/wmucpp,
 * boards/rcRelay32) from STM32G031 to RP2040 (Waveshare RP2040-Zero).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "config.h"
#include "core.h"
#include "mlrs.h"
#include "hal.h"
#include "ports.h"
#include "ws2812.pio.h"

using crsf::Frame;

namespace {

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

// ------------------------------------------------------------------ debug log (USB CDC)
char logBuf[4096];
volatile uint32_t logHead = 0, logTail = 0;

void logPut(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint32_t next = (logHead + 1) % sizeof(logBuf);
        if (next == logTail) return; // full: drop
        logBuf[logHead] = s[i];
        logHead = next;
    }
}

void logFlush() {
    if (!stdio_usb_connected()) {
        logTail = logHead;
        return;
    }
    uint32_t n = 0;
    char tmp[64];
    while (logTail != logHead && n < sizeof(tmp)) {
        tmp[n++] = logBuf[logTail];
        logTail = (logTail + 1) % sizeof(logBuf);
    }
    if (n) stdio_put_string(tmp, int(n), false, true);
}

// ------------------------------------------------------------------ watchdog reset counter
constexpr uint32_t WDG_MAGIC = 0x57444721; // "WDG!"
uint32_t wdgResets = 0;

void countWatchdogResets() {
    if (watchdog_hw->scratch[0] != WDG_MAGIC) {
        watchdog_hw->scratch[0] = WDG_MAGIC;
        watchdog_hw->scratch[1] = 0;
    }
    if (watchdog_enable_caused_reboot()) {
        watchdog_hw->scratch[1] = watchdog_hw->scratch[1] + 1;
    }
    wdgResets = watchdog_hw->scratch[1];
}

// ------------------------------------------------------------------ settings in flash
constexpr uint32_t FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
uint8_t pageBuf[FLASH_PAGE_SIZE];
static_assert(store::Header + sizeof(Settings) + 8 <= FLASH_PAGE_SIZE, "settings must fit into one flash page");
static_assert(offsetof(Settings, ardupilot_mode) == 59, "layout of 1.0.0-1.0.3 must stay unchanged (migration)");

bool loadSettings(Settings& out) {
    const uint8_t* img = reinterpret_cast<const uint8_t*>(XIP_BASE + FLASH_OFFSET);
    return store::decode(img, FLASH_PAGE_SIZE, out);
}

void flashWrite(void*) {
    flash_range_erase(FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_OFFSET, pageBuf, FLASH_PAGE_SIZE);
}

bool saveSettings(const Settings& st) {
    std::memset(pageBuf, 0xff, sizeof(pageBuf));
    store::encode(st, pageBuf);
    watchdog_update();
    const int r = flash_safe_execute(flashWrite, nullptr, 100);
    watchdog_update();
    return r == PICO_OK;
}

bool savePending = false;
uint32_t saveAt = 0;

// ------------------------------------------------------------------ RGB LED (blink patterns of the original)
PIO const ledPio = pio1;
uint ledSm = 0;
hal::Led ledPattern = hal::Led::Off;
uint32_t ledSince = 0;
uint32_t ledLast = 0xffffffff;

void ledPut(uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t grb = (uint32_t(g) << 16) | (uint32_t(r) << 8) | b;
    if (grb == ledLast) return;
    ledLast = grb;
    pio_sm_put_blocking(ledPio, ledSm, grb << 8u);
}

void ledInit() {
    ledSm = pio_claim_unused_sm(ledPio, true);
    const uint off = pio_add_program(ledPio, &ws2812_program);
    ws2812_program_init(ledPio, ledSm, off, PIN_WS2812, 800000);
    ledPut(0, 0, 0);
}

// n flashes, then a pause
bool flashes(uint32_t t, uint32_t n, uint32_t on, uint32_t off, uint32_t pause) {
    const uint32_t period = n * (on + off) + pause;
    const uint32_t x = t % period;
    if (x >= n * (on + off)) return false;
    return (x % (on + off)) < on;
}

void ledUpdate(uint32_t now) {
    constexpr uint8_t B = LED_BRIGHTNESS;
    const uint32_t t = now - ledSince;
    switch (ledPattern) {
    case hal::Led::Off:
        ledPut(0, 0, 0);
        break;
    case hal::Led::Init: // steady
        ledPut(0, 0, B);
        break;
    case hal::Led::UnConnected: // one fast flash
        if (flashes(t, 1, 60, 60, 380)) ledPut(B, 0, 0); else ledPut(0, 0, 0);
        break;
    case hal::Led::Run: // two slow flashes; half duplex: cyan instead of green
        if (flashes(t, 2, 150, 150, 700)) {
            if (ports::relayHalfDuplex()) ledPut(0, B, B); else ledPut(0, B, 0);
        } else {
            ledPut(0, 0, 0);
        }
        break;
    }
}

// ------------------------------------------------------------------ ADC
float vSupply = -1.0f;
float tChip = 0.0f;
uint32_t lastAdc = 0;

void adcInit() {
    adc_init();
    adc_gpio_init(PIN_VSENSE);
    adc_set_temp_sensor_enabled(true);
}

void adcUpdate(uint32_t now) {
    if (now - lastAdc < 200) return;
    lastAdc = now;
    constexpr float conv = 3.3f / 4096.0f;
    adc_select_input(PIN_VSENSE - 26);
    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) sum += adc_read();
    const float v = (float(sum) / 8.0f) * conv * (VSENSE_R1 + VSENSE_R2) / VSENSE_R2;
    vSupply = (vSupply < 0.0f) ? v : (vSupply * 0.7f + v * 0.3f);
    adc_select_input(4);
    sum = 0;
    for (int i = 0; i < 8; ++i) sum += adc_read();
    const float vt = (float(sum) / 8.0f) * conv;
    const float t = 27.0f - (vt - 0.706f) / 0.001721f;
    tChip = (now < 1000) ? t : (tChip * 0.8f + t * 0.2f);
}

// ------------------------------------------------------------------ USB console
crsf::Parser inParser;
crsf::Parser relayParser;

void printStatus() {
    const auto s = core::stats();
    hal::log("\n%s %s  (%s)\n", FW_NAME, FW_VERSION, core::deviceName());
    hal::log("state       : %s, relay %s%s\n", core::stateName(), s.relayEnabled ? "enabled" : "disabled",
             s.telemetryOff ? ", telemetry temporarily off" : "");
    hal::log("relay port  : %s, %lu baud\n", ports::relayHalfDuplex() ? "half duplex (GP" "4" ")" : "full duplex",
             (unsigned long)ports::relayBaud());
    hal::log("input       : frames %lu, channels %lu, crc err %lu, frame err %lu, overflow %lu\n",
             (unsigned long)s.inFrames, (unsigned long)s.channelsIn, (unsigned long)inParser.crcErrors,
             (unsigned long)ports::inputFrameErrors(), (unsigned long)ports::inputOverflows());
    hal::log("relay       : frames %lu, crc err %lu, frame err %lu, overflow %lu\n",
             (unsigned long)s.relayFrames, (unsigned long)relayParser.crcErrors,
             (unsigned long)ports::relayFrameErrorsTotal(), (unsigned long)ports::relayOverflows());
    hal::log("sent        : to module %lu (dropped %lu), to handset %lu (dropped %lu)\n",
             (unsigned long)s.toModule, (unsigned long)s.droppedToModule,
             (unsigned long)s.toHandset, (unsigned long)s.droppedToHandset);
    hal::log("param retry : %lu by the relay, %lu duplicate answers dropped\n",
             (unsigned long)s.relayRetries, (unsigned long)s.duplicatesDropped);
    if (mlrs::active()) {
        hal::log("mLRS        : %s, params %u/%u, requests %lu, retries %lu\n", mlrs::stateName(),
                 unsigned(mlrs::paramsLoaded()), unsigned(mlrs::paramNum()),
                 (unsigned long)mlrs::requests(), (unsigned long)mlrs::retries());
    }
    hal::log("supply      : %.2f V, chip %.1f C, watchdog resets %lu\n", double(vSupply), double(tChip),
             (unsigned long)wdgResets);
}

void console() {
    const int c = getchar_timeout_us(0);
    if (c < 0) return;
    switch (c) {
    case 's': printStatus(); break;
    case 'p':
        core::setTrace(!core::tracing());
        hal::log("# parameter trace %s\n", core::tracing() ? "on" : "off");
        break;
    case 'b':
        hal::log("# reboot to BOOTSEL (UF2) ...\n");
        logFlush();
        sleep_ms(100);
        reset_usb_boot(0, 0);
        break;
    case 'r':
        hal::log("# reboot ...\n");
        logFlush();
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
        break;
    case 'd':
        hal::log("# factory defaults, reboot ...\n");
        core::settings.defaults();
        saveSettings(core::settings);
        logFlush();
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
        break;
    case 'h':
    case '?':
        hal::log("\ncommands: s=status  p=parameter trace on/off  b=bootloader (UF2)  r=reboot  d=factory defaults\n");
        break;
    default:
        break;
    }
}

} // namespace

// ------------------------------------------------------------------ HAL for the core
namespace hal {
void relaySetBaud(uint32_t baud) {
    hal::log("# Relay baud: %lu\n", (unsigned long)baud);
    ports::relaySetBaud(baud);
}
void relaySetHalfDuplex(bool on) {
    hal::log("# Relay half duplex: %u\n", on ? 1u : 0u);
    ports::relaySetHalfDuplex(on);
    relayParser.reset();
}
void relaySetInverted(bool on) {
    hal::log("# Relay inverted: %u\n", on ? 1u : 0u);
    ports::relaySetInverted(on);
    relayParser.reset();
}
uint32_t relayFrameErrors() { return ports::relayFrameErrorsTakeNew(); }
void led(Led p) {
    if (p != ledPattern) {
        ledPattern = p;
        ledSince = nowMs();
    }
}
void requestSave() {
    savePending = true;
    saveAt = nowMs() + 500;
}
float supplyVoltage() { return vSupply; }
float chipTemperature() { return tChip; }
uint32_t watchdogResets() { return wdgResets; }
void log(const char* fmt, ...) {
    char tmp[160];
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, a);
    va_end(a);
    if (n > 0) logPut(tmp, size_t(n) < sizeof(tmp) ? size_t(n) : sizeof(tmp) - 1);
}
}

// ------------------------------------------------------------------ main
int main() {
    stdio_init_all();
    countWatchdogResets();

    if (!loadSettings(core::settings)) {
        core::settings.defaults();
    }

    ledInit();
    adcInit();
    adcUpdate(0);
    ports::inputInit(inputBaudrate);
    ports::relayInit(relayBaudrates[0]);

    core::init(nowMs());
    hal::log("# %s %s start, watchdog resets: %lu\n", FW_NAME, FW_VERSION, (unsigned long)wdgResets);

    watchdog_enable(WATCHDOG_MS, true);

    Frame f;
    uint8_t burst[160];
    constexpr uint32_t idleResetUs = 500;

    for (;;) {
        watchdog_update();
        const uint32_t now = nowMs();

        // receiver #1 -> core
        uint8_t c;
        while (ports::inputRead(c)) {
            inParser.push(c);
            while (inParser.next(f)) core::onInputFrame(f, now);
        }
        if (inParser.partial() && ports::inputIdleUs() > idleResetUs) inParser.reset();

        // module #2 -> core
        ports::relayPoll();
        while (ports::relayRead(c)) {
            relayParser.push(c);
            while (relayParser.next(f)) core::onRelayFrame(f, now);
        }
        if (relayParser.partial() && ports::relayIdleUs() > idleResetUs) relayParser.reset();

        core::tick(now);

        // core -> ports
        if (!ports::inputBusy() && core::nextToHandset(f)) {
            ports::inputSend(f.b, f.size);
        }
        if (!ports::relayBusy()) {
            const size_t n = core::nextRelayBurst(burst, sizeof(burst));
            if (n) ports::relaySend(burst, n);
        }

        // housekeeping
        if (savePending && int32_t(now - saveAt) >= 0) {
            savePending = false;
            const bool ok = saveSettings(core::settings);
            hal::log("# EEPROM %s\n", ok ? "OK" : "NOK");
        }
        ledUpdate(now);
        adcUpdate(now);
        console();
        logFlush();
    }
}
