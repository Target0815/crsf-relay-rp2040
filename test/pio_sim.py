#!/usr/bin/env python3
"""Minimal cycle simulator for the hd_uart PIO programs (instruction words are taken
from the pioasm generated header, so the assembled code itself is tested).

usage: pio_sim.py build/hd_uart.pio.h
"""
import random
import re
import sys


def load(header, name):
    txt = open(header).read()
    block = re.search(name + r"_program_instructions\[\] = \{(.*?)\};", txt, re.S).group(1)
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{4})", block)]
    wrap_t = int(re.search(r"#define " + name + r"_wrap_target (\d+)", txt).group(1))
    wrap = int(re.search(r"#define " + name + r"_wrap (\d+)", txt).group(1))
    return words, wrap_t, wrap


class SM:
    def __init__(self, prog, sideset_bits=0, sideset_opt=False, pin_dir_init=0):
        self.code, self.wrap_target, self.wrap = prog
        self.ss_bits = sideset_bits + (1 if sideset_opt else 0)
        self.ss_opt = sideset_opt
        self.pc = 0
        self.x = self.y = 0
        self.osr = 0
        self.osr_cnt = 32
        self.isr = 0
        self.isr_cnt = 0
        self.txf = []
        self.rxf = []
        self.delay = 0
        self.pin_val = 1
        self.pin_dir = pin_dir_init
        self.irq = set()
        self.fifo_depth = 8

    def step(self, line):
        if self.delay:
            self.delay -= 1
            return
        w = self.code[self.pc]
        op = w >> 13
        ds = (w >> 8) & 0x1F
        delay_bits = 5 - self.ss_bits
        delay = ds & ((1 << delay_bits) - 1)
        if self.ss_bits:
            ss = ds >> delay_bits
            if self.ss_opt:
                if ss >> (self.ss_bits - 1):
                    self.pin_val = ss & 1
            else:
                self.pin_val = ss & 1
        stall = False
        nxt = self.pc + 1 if self.pc != self.wrap else self.wrap_target
        arg = w & 0xFF
        if op == 0:  # JMP
            cond = (arg >> 5) & 7
            addr = arg & 0x1F
            take = {0: True, 1: self.x == 0, 2: self.x != 0, 6: line == 1}[cond]
            if cond == 2:
                self.x = (self.x - 1) & 0xFFFFFFFF
            if take:
                nxt = addr
        elif op == 1:  # WAIT
            pol = (arg >> 7) & 1
            src = (arg >> 5) & 3
            assert src == 1
            if line != pol:
                stall = True
        elif op == 2:  # IN pins
            n = arg & 0x1F or 32
            assert (arg >> 5) & 7 == 0 and n == 1
            self.isr = (self.isr >> 1) | (line << 31)
            self.isr_cnt += 1
        elif op == 3:  # OUT pins
            n = arg & 0x1F or 32
            assert (arg >> 5) & 7 == 0 and n == 1
            self.pin_val = self.osr & 1
            self.osr >>= 1
            self.osr_cnt += 1
        elif op == 4:  # PUSH / PULL
            if arg & 0x80:  # pull
                block = arg & 0x20
                if self.txf:
                    self.osr = self.txf.pop(0)
                    self.osr_cnt = 0
                elif block:
                    stall = True
                else:
                    self.osr = self.x
            else:
                if len(self.rxf) < self.fifo_depth:
                    self.rxf.append(self.isr)
                self.isr = 0
                self.isr_cnt = 0
        elif op == 5:  # MOV
            dst = (arg >> 5) & 7
            src = arg & 7
            if src == 5:
                val = 0xFFFFFFFF if len(self.txf) < 1 else 0
            elif src == 2:
                val = self.y
            else:
                raise NotImplementedError(hex(w))
            if dst == 1:
                self.x = val
            elif dst == 2:
                self.y = val
        elif op == 6:  # IRQ
            self.irq.add(arg & 7)
        elif op == 7:  # SET
            dst = (arg >> 5) & 7
            data = arg & 0x1F
            if dst == 1:
                self.x = data
            elif dst == 0:
                self.pin_val = data & 1
            elif dst == 4:
                self.pin_dir = data & 1
        if stall:
            return
        self.pc = nxt
        self.delay = delay


def uart_wave(data, bit_cycles, stop_bits=1.0, gap=0.0, bad_stop=None):
    """list of (level) per cycle for an external UART sender"""
    out = []
    t = 0.0
    edges = []
    for i, b in enumerate(data):
        bits = [0] + [(b >> k) & 1 for k in range(8)]
        stop = 0 if bad_stop == i else 1
        for v in bits:
            edges.append((t, v)); t += bit_cycles
        edges.append((t, stop)); t += bit_cycles * stop_bits
        edges.append((t, 1)); t += gap * bit_cycles
    n = int(t) + 1
    level, k = 1, 0
    for c in range(n):
        while k < len(edges) and edges[k][0] <= c:
            level = edges[k][1]; k += 1
        out.append(level)
    return out


def decode_uart(wave, bit_cycles):
    res = []
    i = 0
    while i < len(wave):
        if wave[i] == 0:
            start = i
            byte = 0
            for k in range(8):
                byte |= wave[int(start + bit_cycles * (1.5 + k))] << k
            stop = wave[int(start + bit_cycles * 9.5)]
            res.append((byte, stop, start))
            i = int(start + bit_cycles * 9.5)
            while i < len(wave) and wave[i] == 0:
                i += 1
        else:
            i += 1
    return res


def test_tx(header, rnd):
    sm = SM(load(header, "hd_tx"), sideset_bits=1, sideset_opt=True)
    data = [rnd.randrange(256) for _ in range(rnd.randrange(1, 70))]
    pending = list(data)
    wave, dirs = [], []
    idle_pc_cycles = 0
    for cyc in range(30 + len(data) * 12 * 8 + 200):
        # DMA keeps the 8 deep FIFO full
        while pending and len(sm.txf) < 8 and cyc > 20:
            sm.txf.append(pending.pop(0))
        sm.step(1)
        line = sm.pin_val if sm.pin_dir else 1  # pull-up when released
        wave.append(line)
        dirs.append(sm.pin_dir)
        if not pending and not sm.txf and sm.pc == 1:
            idle_pc_cycles += 1
    dec = decode_uart(wave, 8)
    assert [d[0] for d in dec] == data, (data, dec)
    assert all(d[1] == 1 for d in dec), "stop bit"
    assert sm.pin_dir == 0, "line not released"
    assert sm.pc == 1, "not waiting at pull"
    # driven continuously during the burst (no release between bytes)
    first = dirs.index(1)
    last = len(dirs) - 1 - dirs[::-1].index(1)
    assert all(dirs[first:last + 1]), "released inside the burst"
    # the line is driven only shortly after the last stop bit
    end_last_stop = dec[-1][2] + 10 * 8
    assert last - end_last_stop < 20, last - end_last_stop
    return len(data)


def test_rx(header, rnd, tol):
    sm = SM(load(header, "hd_rx"))
    data = [rnd.randrange(256) for _ in range(rnd.randrange(1, 70))]
    bad = rnd.randrange(len(data)) if rnd.random() < 0.3 else None
    wave = [1] * 37 + uart_wave(data, 8 * (1 + tol), gap=rnd.choice([0, 0, 0.5, 3]), bad_stop=bad) + [1] * 100
    sm.fifo_depth = 10_000
    for lvl in wave:
        sm.step(lvl)
    got = [(w >> 24) & 0xFF for w in sm.rxf]
    expect = [b for i, b in enumerate(data) if i != bad]
    if bad is None:
        assert got == expect, (tol, data, got)
        assert 4 not in sm.irq
    else:
        # the bad byte is dropped and flagged; resynchronisation may cost the next byte
        assert 4 in sm.irq
        assert got[:bad] == expect[:bad]
    return len(data)


def main():
    header = sys.argv[1]
    rnd = random.Random(1)
    n = 0
    for _ in range(300):
        n += test_tx(header, rnd)
    for tol in (-0.025, -0.015, -0.005, 0.0, 0.005, 0.015, 0.025):
        for _ in range(100):
            n += test_rx(header, rnd, tol)
    print(f"PIO OK: {n} bytes through hd_tx/hd_rx")


main()
