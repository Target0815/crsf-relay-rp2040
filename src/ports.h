/*
 * CRSF-Relay for RP2040 - serial ports
 *   InputPort : UART0, full duplex, receiver #1
 *   RelayPort : UART1 (full duplex) or PIO single-wire half duplex, module #2
 * GPL-3.0-or-later, see crsf.h
 */
#pragma once

#include <cstdint>
#include <cstddef>

// single producer (IRQ) / single consumer (main loop) byte ring
class ByteRing {
public:
    void put(uint8_t c) {
        const uint16_t h = head_;
        const uint16_t n = uint16_t((h + 1) & (Size - 1));
        if (n == tail_) { ++overflows; return; }
        buf_[h] = c;
        head_ = n;
    }
    bool get(uint8_t& c) {
        const uint16_t t = tail_;
        if (t == head_) return false;
        c = buf_[t];
        tail_ = uint16_t((t + 1) & (Size - 1));
        return true;
    }
    void clear() { tail_ = head_; }
    volatile uint32_t overflows = 0;
private:
    static constexpr uint16_t Size = 1024;
    uint8_t buf_[Size] = {};
    volatile uint16_t head_ = 0;
    volatile uint16_t tail_ = 0;
};

namespace ports {

// ---- receiver #1
void inputInit(uint32_t baud);
bool inputRead(uint8_t& c);
uint32_t inputIdleUs();          // time since the last received byte
bool inputBusy();
void inputSend(const uint8_t* p, size_t n);
uint32_t inputFrameErrors();     // total
uint32_t inputOverflows();

// ---- module #2
void relayInit(uint32_t baud);
void relaySetBaud(uint32_t baud);
uint32_t relayBaud();
void relaySetHalfDuplex(bool on);
bool relayHalfDuplex();
void relayPoll();                // call from the main loop (half-duplex turnaround)
bool relayRead(uint8_t& c);
uint32_t relayIdleUs();
bool relayBusy();
void relaySend(const uint8_t* p, size_t n);
uint32_t relayFrameErrorsTakeNew(); // since last call
uint32_t relayFrameErrorsTotal();
uint32_t relayOverflows();

}
