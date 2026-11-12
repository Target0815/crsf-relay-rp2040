**English** | [Deutsch](README.de.md)

# RC-Relay RP2040

A relay for CRSF radio control: it connects two radio links in a row, extending the range or
getting around obstacles – the **radio link** (radio → relay) and the **model link**
(relay → model).

![Using the relay](docs/relay-setup.en.svg)

The relay is placed where it has a clear view of the model – on a tripod, on the car roof or on
an observation tower. The radio stays with the pilot, for example in the car.
Suitable for aircraft, RC cars and boats.

> **Use at your own risk.** Test the failsafe behaviour on the ground before every use
> (see [Failsafe](#failsafe)).

## Features

* **Model link with ELRS or mLRS** – the relay detects the module type by itself.
* **Everything is set up in the ELRS Lua script:** the relay itself, the relay's TX module and
  the receiver in the model. With mLRS the relay also shows the mLRS parameters in the ELRS Lua.
* **Telemetry** from the model (GPS, battery, flight mode …) arrives at the radio as usual,
  the values of both links stay separate.
* **Failsafe:** if the radio link is lost (e.g. radio switched off), the model goes into its failsafe.
* **Fast Lua menu:** the relay repeats lost requests itself.
* Half duplex (module bay/JR pin) and full duplex, watchdog, USB console for diagnostics.

## What you need

* a **radio with an ELRS TX module** (internal or external) for the radio link, e.g. RadioMaster TX15
* **Waveshare RP2040-Zero** (or another RP2040 board, pins see below)
* a **CRSF receiver** for the radio link, e.g. RadioMaster ER3C-i (ELRS)
* a **TX module** for the model link: ELRS module (e.g. RadioMaster Bandit Nano) or mLRS TX
  (e.g. MatekSys mR900-30, mLRS v1.4.04 or newer)

## Installing the firmware

1. Download the UF2 file from **Releases** (`crsf-relay-<version>-rp2040-zero.uf2`).
2. Plug the RP2040-Zero into USB while holding the **BOOT** button; the drive `RPI-RP2` appears.
3. Copy the UF2 file onto it. Settings are kept when updating.

## Wiring

| RP2040-Zero | connects to                                                   |
|-------------|---------------------------------------------------------------|
| GP0         | RX of the receiver (radio link)                                 |
| GP1         | TX of the receiver (radio link)                                 |
| GP4         | TX module: **half duplex** – the single signal line (JR pin 5 / "In"); **full duplex** – RX of the module |
| GP5         | TX module: full duplex only – TX of the module                |
| 5V, GND     | 5 V supply, common ground of all devices                      |

All signals are 3.3 V level. The polarity of the module side is set in the menu
("Inverted", see table).

> **Note:** so far only **half duplex** has been tested with real hardware
> (ELRS: RadioMaster Bandit Nano, mLRS: MatekSys mR900-30). Full duplex is implemented but has
> only been checked without hardware.

> **USB warning:** on the RP2040-Zero, 5V is connected directly to the USB socket. Before plugging
> into a PC, disconnect the external 5 V line (keep GND connected) or use a USB power blocker.

## Settings at a glance

In the ELRS Lua script, select the device **RC-Relay RP2040**.

| Menu entry    | with ELRS module                                 | with mLRS module           | Meaning |
|---------------|--------------------------------------------------|----------------------------|---------|
| Half-Duplex   | **On** (module bay) / Off (receiver as TX)       | **On**                     | one signal line (GP4) or two (GP4/GP5) |
| Baudrate      | 921k (default)                                   | **400k**                   | speed towards the TX module |
| Inverted      | **Off**                                          | **On**                     | polarity of the module side |
| LinkStat      | Transform                                        | Transform                  | how the values of the model link reach the radio |
| Telemetry     | Forward                                          | Forward                    | telemetry from the model to the radio |
| Forward Rate  | 2                                                | 2                          | pass on every n-th telemetry message (relieves the radio link) |
| ArduPilot     | Forward (with ArduPilot FC)                      | any                        | ArduPilot telemetry (Yaapu widget) |
| Own Telemetry | as you like                                      | as you like                | chip temperature of the relay as a sensor |

**Bold** = differs from the default or important.

On the **mLRS module** and **mLRS receiver** (through the relay in the ELRS Lua or on the module display):

| mLRS parameter      | Value                      |
|---------------------|----------------------------|
| Tx Ch Source        | crsf (default)             |
| Rx Out Mode         | crsf (to the flight controller) |
| Rx Failsafe Mode    | as you like, e.g. "no signal" |
| Config ID + 1       | do **not** bind it to this model with the same bind phrase (see Failsafe) |

## Getting started

1. **Switch on the radio first, then the relay.** (An mLRS module looks for its settings only
   during the first seconds after power-up.)
2. The LED on the RP2040-Zero shows the state:

   | LED                     | State |
   |-------------------------|-------|
   | blue, steady            | starting, waiting for the receiver |
   | green, 2 flashes        | running (half duplex: cyan) |
   | red, 1 short flash      | no data from the receiver (radio link lost) |

3. The ELRS Lua shows: the radio's TX module, the relay's receiver, **RC-Relay RP2040**,
   the relay's TX module and the receiver in the model (with ELRS).

## The relay's menu

| Entry          | Meaning |
|----------------|---------|
| Firmware       | version of the relay firmware |
| Watchdog       | number of watchdog resets (should stay 0) |
| Total Retries  | repeated menu requests since power-up (rises with a weak link) |
| Chip Temp.     | temperature of the RP2040 |
| other entries  | see [Settings at a glance](#settings-at-a-glance) |

**LinkStat** in detail: *Transform* shows the values of the model link as separate sensors (RRSP, TRSP,
RPWR, TFPS) without disturbing the values of the radio link. *Forward* passes them on unchanged (then the
sensors jump between both links), *Tunnel* wraps them for Lua scripts/widgets, *Off* drops them.

## mLRS: parameters in the ELRS Lua

The mLRS module appears in the device list (e.g. **Matek mR900-30**). After power-up the relay
loads the mLRS parameters in the background and shows them there:

* at the top: TX and RX with name and version, below that **Save**
* folders **General**, **Tx**, **Rx** and **Rx Failsafe**

As in the mLRS Lua, changes take effect only after **Save**. Folders with changes not saved yet
show "(n unsaved)" in their name. After saving, close and reopen the Lua once to clear the marks.

Notes:
* If it shows "loading …, reopen", the relay is still loading: close the Lua and reopen it after
  a few seconds.
* Rx parameters appear only while the mLRS receiver in the model is connected.
* The bind phrase is read-only (the ELRS Lua cannot edit text).

## Failsafe

* **ELRS:** if the radio link is lost, the relay stops sending. The ELRS module stops the link to the
  model and the receiver in the model goes into its failsafe.
* **mLRS:** an mLRS module would simply keep sending the last values without an input signal.
  So when the radio link is lost, the relay switches to another mLRS config (config ID + 1, like changing
  the "Receiver ID" in EdgeTX). The link to the model breaks and the mLRS receiver goes into its
  failsafe. When the radio link is back, the relay switches back and the link is re-established after a
  few seconds. Dropouts shorter than 0.75 s cause nothing.
  Important: config ID + 1 must not be bound with the same bind phrase (the defaults "mlrs.0",
  "mlrs.1" … are fine).
* **Relay without power:** if the relay's TX module is powered from the same source, it is off
  as well and the model goes into its failsafe.

**Test:** with the model safe on the ground, switch off the radio → the flight controller or model
must report failsafe. Switch the radio back on → the link must come back.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| mLRS: no control, no telemetry | check Baudrate 400k and Inverted On; switch on the radio first, then the relay |
| mLRS does not work at 921k | stay at 400k – sufficient for all mLRS modes |
| relay menu loads slowly | normal on first opening; "Total Retries" shows the repetitions |
| sensors jump between two values | set LinkStat to Transform |
| LED red | the relay's receiver has no link to the radio or gets no channels |

## USB console (diagnostics)

Connect the RP2040-Zero to a PC via USB (see warning above), any terminal program:
`s` status · `p` log of menu requests on/off · `b` bootloader (update) · `r` reboot ·
`d` factory defaults.

## Origin and licence

Based on the **RC-Relay32** by Wilhelm Meier
([wmucpp](https://github.com/wimalopaan/wmucpp/tree/master/boards/rcRelay32)), ported to the
RP2040 and extended (mLRS among others). Licence: GPL-3.0-or-later.

Questions, problems or ideas: please open an [issue](../../issues) on GitHub.
