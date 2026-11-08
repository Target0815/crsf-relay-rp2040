/*
 * CRSF-Relay for RP2040
 * Port of RC-Relay32 (WMuCpp, Copyright (C) 2019 - 2026 Wilhelm Meier)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CRSF basics: constants, CRC, frame container, builder, stream parser, queue.
 * Hardware independent (also compiled by the host tests).
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace crsf {

constexpr uint8_t SYNC = 0xC8;

namespace addr {
constexpr uint8_t Broadcast  = 0x00;
constexpr uint8_t Controller = 0xC8;
constexpr uint8_t Handset    = 0xEA;
constexpr uint8_t RX         = 0xEC;
constexpr uint8_t TX         = 0xEE;
}

namespace type {
constexpr uint8_t Temp        = 0x0D;
constexpr uint8_t Cells       = 0x0E;
constexpr uint8_t Link        = 0x14;
constexpr uint8_t Channels    = 0x16;
constexpr uint8_t LinkStatsRx = 0x1C;
constexpr uint8_t LinkStatsTx = 0x1D;
constexpr uint8_t Ping        = 0x28; // first extended type
constexpr uint8_t Info        = 0x29;
constexpr uint8_t ParamEntry  = 0x2B;
constexpr uint8_t ParamRead   = 0x2C;
constexpr uint8_t ParamWrite  = 0x2D;
constexpr uint8_t Command     = 0x32;
constexpr uint8_t RadioID     = 0x3A;
constexpr uint8_t PassThru    = 0x7F;
constexpr uint8_t Last        = 0x80; // ArduPilot
constexpr uint8_t ArduPilot   = 0x80; // passthrough telemetry, no extended header
}

namespace idx {
constexpr uint8_t sync = 0, length = 1, type = 2, dest = 3, src = 4, payload = 5;
}

namespace passthru {
constexpr uint16_t AppIdTelem   = 6200;
constexpr uint8_t  SubLinkStat  = 0x01;
constexpr uint8_t  SubTelemetry = 0x02;
}

constexpr uint8_t MaxFrame  = 64; // sync + length + 62
constexpr uint8_t MaxLength = 62; // value of the length byte
constexpr uint8_t MinLength = 2;

constexpr uint8_t ChannelsPayload = 22;

inline uint8_t crc8(const uint8_t* p, size_t n, uint8_t poly = 0xD5) {
    uint8_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80) ? uint8_t((crc << 1) ^ poly) : uint8_t(crc << 1);
        }
    }
    return crc;
}

struct Frame {
    uint8_t size = 0;           // total bytes incl. sync and CRC
    uint8_t b[MaxFrame] = {};

    uint8_t type() const { return b[idx::type]; }
    uint8_t length() const { return b[idx::length]; }
    // extended frames carry dest/src addresses (as in the original: size >= 6)
    // RP2040 port: 0x80 (ArduPilot) has no dest/src addresses and is telemetry
    bool extended() const {
        return (size >= 6) && (b[idx::type] >= type::Ping) && (b[idx::type] != type::ArduPilot);
    }
    // recompute CRC after the frame was modified in place
    void fixCrc() {
        const uint8_t len = b[idx::length];
        b[len + 1] = crc8(&b[idx::type], len - 1);
    }
    bool crcOk() const {
        const uint8_t len = b[idx::length];
        return crc8(&b[idx::type], len - 1) == b[len + 1];
    }
};

// Builds [SYNC][len][type][payload...][crc]
class Builder {
public:
    Builder(Frame& f, uint8_t t) : f_(f) {
        f_.b[0] = SYNC;
        f_.b[2] = t;
        f_.size = 3;
        ok_ = true;
    }
    void u8(uint8_t v) {
        if (f_.size < MaxFrame - 1) {
            f_.b[f_.size++] = v;
        } else {
            ok_ = false;
        }
    }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void str(const char* s) {
        if (s) {
            while (*s) u8(uint8_t(*s++));
        }
        u8(0);
    }
    void bytes(const uint8_t* p, size_t n) { while (n--) u8(*p++); }
    uint8_t size() const { return f_.size; }
    // finalizes the frame, returns false if it overflowed
    bool finish() {
        f_.b[1] = uint8_t(f_.size - 1);
        f_.b[f_.size] = crc8(&f_.b[2], f_.size - 2);
        ++f_.size;
        return ok_;
    }
private:
    Frame& f_;
    bool ok_ = true;
};

// Byte stream to frames, with resynchronisation.
// Accepted start bytes as in the original: 0xC8, 0xEA, 0xEC (RadioMaster ERS sensors)
class Parser {
public:
    void push(uint8_t c) {
        if (n_ < sizeof(buf_)) buf_[n_++] = c;
    }
    // extract the next complete frame (call repeatedly until false)
    bool next(Frame& out) {
        for (;;) {
            if (n_ == 0) return false;
            if (!validStart(buf_[0])) { drop(1); continue; }
            if (n_ < 2) return false;
            const uint8_t len = buf_[1];
            if ((len < MinLength) || (len > MaxLength)) { drop(1); continue; }
            if (n_ < 3) return false;
            if (buf_[2] > type::Last) { drop(1); continue; }
            const uint8_t total = uint8_t(len + 2);
            if (n_ < total) return false;
            if (crc8(&buf_[2], len - 1) == buf_[total - 1]) {
                std::memcpy(out.b, buf_, total);
                out.size = total;
                drop(total);
                ++frames;
                return true;
            }
            ++crcErrors;
            drop(1);
        }
    }
    bool partial() const { return n_ > 0; }
    void reset() { n_ = 0; }

    uint32_t frames = 0;
    uint32_t crcErrors = 0;
private:
    static bool validStart(uint8_t s) {
        return (s == SYNC) || (s == addr::Handset) || (s == addr::RX);
    }
    void drop(uint8_t k) {
        if (k >= n_) { n_ = 0; return; }
        std::memmove(buf_, buf_ + k, n_ - k);
        n_ = uint8_t(n_ - k);
    }
    uint8_t buf_[MaxFrame + 2] = {};
    uint8_t n_ = 0;
};

template<size_t N>
class FrameQueue {
public:
    bool push(const Frame& f) {
        if (count_ >= N) { ++dropped; return false; }
        q_[(head_ + count_) % N] = f;
        ++count_;
        return true;
    }
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
    const Frame& front() const { return q_[head_]; }
    void pop() {
        if (count_) { head_ = (head_ + 1) % N; --count_; }
    }
    bool pop(Frame& f) {
        if (!count_) return false;
        f = q_[head_];
        pop();
        return true;
    }
    void clear() { head_ = count_ = 0; }
    uint32_t dropped = 0;
private:
    Frame q_[N];
    size_t head_ = 0;
    size_t count_ = 0;
};

} // namespace crsf
