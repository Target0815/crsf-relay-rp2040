**English** | [Deutsch](README.de.md)

# CRSF relay for RP2040

Port of the **RC-Relay32** by Wilhelm Meier
([wmucpp: `boards/rcRelay32`](https://github.com/wimalopaan/wmucpp/tree/master/boards/rcRelay32))
from the WeAct STM32G031 to the RP2040, target board **Waveshare RP2040-Zero**.

The relay connects two CRSF radio links: the receiver of the first link feeds the relay, and the
relay drives the TX module of the second link. All devices remain reachable through `elrsV3.lua`
(or TBS Agent), and telemetry is passed back without collisions.

![Flying from the car with the CRSF relay](docs/relay-setup.en.svg)

A typical use case: the radio stays in the car, but the car body shields the radio signal.
A receiver with the relay and a second TX module therefore stands outside on a tripod.
The short first link covers the distance from the car to the tripod, the second link from the
tripod to the aircraft has a clear line of sight and full range.

## Flashing

1. Download the current UF2 file from **Releases** (`crsf-relay-<version>-rp2040-zero.uf2`).
2. Plug the RP2040-Zero into USB while holding the **BOOT** button. A drive `RPI-RP2` appears.
3. Copy the UF2 file to that drive. Done.

If the firmware is already installed, no button is needed: send `b` in a USB terminal,
or use `picotool load -f -x crsf-relay-…uf2`.

## Wiring (RP2040-Zero)

| RP2040-Zero | Signal                                   | Original (WeAct) |
|-------------|------------------------------------------|------------------|
| GP0         | → RX of receiver 1 (CRSF input)          | PA2              |
| GP1         | ← TX of receiver 1                       | PA3              |
| GP4         | → RX of TX module 2 (full duplex) **or the signal line in half duplex** | PA9 |
| GP5         | ← TX of TX module 2 (full duplex only)   | PA10             |
| GP29        | Supply voltage via divider (optional)    | –                |
| GP16        | RGB LED on the board                     | PA4 (LED)        |
| GND, 5V     | Common ground, supply                    |                  |

* All signals are 3.3 V level, not inverted (CRSF standard). Internal pull-ups are enabled.
* **Full duplex** (ELRS receiver used as transmitter, "RX-as-TX"): cross GP4/GP5 as usual.
* **Half duplex** (JR bay or external ELRS module): connect only **GP4** to the CRSF/S.Port pin of
  the module and set "Half-Duplex" to "On" in the menu. External modules need their own supply
  (usually 5–8.4 V and, depending on output power, more than 1 A), not the 3.3 V pin of the RP2040-Zero.
* Voltage measurement: Vin – 33 kΩ – GP29 – 10 kΩ – GND (up to approx. 14 V). Other values via
  `VSENSE_R1`/`VSENSE_R2` at build time. Without a divider the menu entry shows nonsense, otherwise
  it does no harm.

## LED

| Colour / pattern                  | State                                          |
|-----------------------------------|------------------------------------------------|
| blue, steady                      | Init: waiting for receiver 1                   |
| green, 2× slow                    | Run: channel data from receiver 1 (half duplex: cyan) |
| red, 1× fast                      | no channel data from receiver 1                |

## Menu (elrsV3.lua)

The relay reports itself as `RC-Relay32@205` (205 = 0xCD, its own CRSF address).
TX module 2 appears at address 206 (0xCE), its receiver at 207 (0xCF).

The first entry, **RP2040 Firmware**, shows the firmware version of this port (e.g. `1.0.6`),
so the Lua script immediately shows that the RP2040 port is running and which version.

All menu entries and default values of the original are available:
CRSF Address, TX/RX Rewrite Address, TX Rewrite Name, Rewrite Name, LinkStat
(Off/Forward/Transform/Tunnel), Failsafe (Forward/Hold), Telem. Off Dur.,
folder "Telemetry forwarding" (Forward Rate + Off/Forward/Tunnel per sensor type),
Half-Duplex, Baudrate (400k/420k/460k/921k/1.87M).

New in the folder "Telemetry forwarding":

* **ArduPilot** (Off/Forward/Tunnel, default Forward): ArduPilot passthrough telemetry
  (CRSF type 0x80, e.g. for Yaapu). Like all telemetry it is paused while the Lua script accesses
  parameters, and the Forward Rate applies per telemetry value (AppId) so that no value is thinned
  out completely. Status texts are never thinned out. INAV does not send type 0x80, so the entry
  has no effect there. In Tunnel mode, multi packets that would exceed 64 bytes together with the
  tunnel header are dropped.

New (from the ToDo list of the original):

* **Watchdog**: number of watchdog resets since power-up (watchdog 500 ms)
* **Supply Volt.** and **Chip Temp.**: shown in the menu
* **Own Telemetry**: sends the voltage (CRSF 0x0E) and chip temperature (0x0D) once per second,
  with the relay address as sensor ID. In EdgeTX they appear as separate sensors and do not
  collide with the aircraft's sensors. Default: off.

## USB console

> **Caution:** On the RP2040-Zero the 5V pin is connected directly to the USB socket (no diode).
> Before plugging in USB, disconnect the external 5 V supply from the RP2040-Zero and **keep GND
> connected**. Otherwise the external supply feeds back into the computer's USB port (reboot or
> damage to the port possible). Alternatively use a USB cable without the 5 V wire ("USB power blocker").

The relay appears as a USB serial device (any terminal, baud rate does not matter).
It prints debug messages like the original (`# Run`, `# UnCon`, …) and accepts these commands:

| Key | Function |
|-----|----------|
| `s` | Status: state, packet counters, CRC/framing errors, voltage, temperature |
| `p` | Parameter trace on/off: timestamp of every request to module 2 / receiver 2 and every answer |
| `b` | Reboot into the UF2 bootloader |
| `r` | Reboot |
| `d` | Factory defaults and reboot |

## Differences from the original

The logic follows `package_relay_rewrite.h`, `crsf_2*.h`, `crsf_cb.h` and `gfsm.h`
as of 8 Nov 2026. Taken over from that revision: the relay starts disabled and becomes active only
in state "Run". "Run" is also reached without link statistics if only channel packets arrive
(direct connection). Invalid parameter values are rejected and not stored.

Intentional differences:

1. **Half duplex timing**: messages to the module (ping, parameters, commands) are sent directly
   behind a channels frame in the same burst, one per 10 ms period, the way EdgeTX does it.
   This way they cannot collide with the module's answer. Full duplex works as in the original.
2. **No centre values before the first channels frame**: channels are passed on only after the
   first real channels frame. Since 8 Nov 2026 the original prevents this as well, by starting
   disabled. Here the check for real channel data applies in addition.
3. **"Telem. Off Dur." in real seconds**: in the original the timer runs at half the duration
   (documented in its README by now).
4. **Speed command** on framing errors proposes the configured baud rate (original: always 400k)
   and contains the command CRC required by the CRSF standard. ELRS ignores this command anyway;
   ELRS modules detect baud rate and inversion by themselves.
5. **Parser**: the input parser resynchronises byte by byte after CRC errors instead of
   discarding the rest of the burst. TX name and tunnel packets are limited safely, as in the
   current original.
6. **Settings** are stored in the last flash sector (with checksum). They are written 0.5 s after
   the last change, so several changes from the Lua script cause only one write. Settings of older
   versions are kept on update, new menu entries get their default value.
7. **Retry of parameter requests**: the ELRS Lua script waits 5 s before repeating an unanswered
   request to any device except its own TX module. Behind the relay an answer occasionally gets
   lost on the side of module 2, which cost 5 s each time. The relay now repeats read requests to
   all devices behind it (module 2, receiver 2 and e.g. the ArduPilot flight controller at 0xC8)
   by itself if the answer does not arrive: after three times the measured round trip time, at
   least 0.1 s (module 2) or 0.5 s (over the radio link), at most 1.5 s, up to three times.
   Duplicate answers caused by these retries are dropped. Write requests are never repeated.
   Counters: `s` in the USB console.
8. **ArduPilot telemetry (0x80)**: the original treated this type like a packet with destination
   and source address. It therefore always forwarded it, even while telemetry was paused for Lua
   access, and rewrote bytes 3 and 4 if they looked like addresses. Here 0x80 is regular telemetry
   with its own menu entry (see above).
9. **IrDA** is omitted (not active in the WeAct build anyway; the RP2040 has no IrDA).

Items 7 and 8 have since been taken over into the original (V17); item 1 has been proposed there.

The protocol logic is checked with host tests (`test/test_core.cpp`), the half-duplex PIO programs
with a cycle simulator (`test/pio_sim.py`). Both run automatically on every push.

## Building

GitHub Actions builds the firmware on every push and runs the tests
(download under "Actions" → run → "Artifacts"). A tag `v<version>` or a release published on
GitHub gets the UF2 file attached.

Locally: Pico SDK 2.1.1, CMake, Ninja, arm-none-eabi-gcc:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
./build_all.sh                      # -> dist/crsf-relay-<version>-rp2040-zero.uf2
```

Other pins: e.g. `cmake -B build -DPIN_RELAY_TX=8 -DPIN_RELAY_RX=9 .`
(GP8/9 are UART1 as well). For a Raspberry Pi Pico: `-DPICO_BOARD=pico`.
It has no RGB LED, otherwise everything is the same.

Tests:

```sh
cd test && g++ -std=c++17 -I../src test_core.cpp ../src/core.cpp -o test_core && ./test_core
python3 test/pio_sim.py build/rp2040-zero/hd_uart.pio.h
```

## Structure

| File | Content |
|------|---------|
| `src/crsf.h`      | CRSF constants, CRC, frame builder, stream parser, queue |
| `src/core.cpp`    | Relay logic, menu, device info, state machine (hardware independent) |
| `src/ports.cpp`   | UART0/UART1 with IRQ receive and DMA transmit, PIO half duplex |
| `src/hd_uart.pio` | Half-duplex UART programs (transmit with line release, receive with framing check) |
| `src/main.cpp`    | Main loop, flash storage, LED, ADC, watchdog, USB console |
| `src/config.h`    | Pins, voltage divider, version numbers |

## Disclaimer

Use at your own risk. The relay is part of the control path to the aircraft: before every flight,
check range, failsafe behaviour and telemetry on the ground, and follow the model flying rules
that apply to you. No warranty is given for function or fitness for a particular purpose
(see also GPL-3.0, sections 15 and 16).

## License

GPL-3.0-or-later, like the original. Original: Copyright (C) 2019–2026 Wilhelm Meier.
