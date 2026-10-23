# Pinout & Wiring

Board: **NodeMCU 1.0 (ESP-12E)** or **Wemos D1 mini**. Pin numbers in the firmware
are GPIO numbers; the silkscreen label (D1, D2 …) is given alongside.

---

## 1. Master pin table

| Peripheral | Module pin | NodeMCU label | GPIO | Notes |
|---|---|---|---|---|
| I²C 16×2 LCD | SDA | **D2** | 4 | shared bus |
| I²C 16×2 LCD | SCL | **D1** | 5 | shared bus |
| I²C 16×2 LCD | VCC | **VIN (5 V)** | — | LCD backpack needs 5 V |
| I²C 16×2 LCD | GND | GND | — | |
| ADS1115 ADC | SDA / SCL | **D2 / D1** | 4 / 5 | same bus as LCD, addr `0x48` |
| ADS1115 ADC | VDD | **3V3** | — | |
| ADS1115 ADC | ADDR | GND | — | sets address `0x48` |
| Voltage sensor (0–25 V) | S (out) | **ADS A0** | — | *not* the ESP's A0 |
| Current sensor ACS712 | OUT | **ADS A1** | — | |
| Current sensor ACS712 | VCC | **VIN (5 V)** | — | |
| DHT11 | DATA | **D4** | 2 | 10 kΩ pull-up to 3V3 |
| DHT11 | VCC | 3V3 | — | |
| SW-420 vibration | DO | **D6** | 12 | digital pulse, interrupt |
| SW-420 vibration | VCC | 3V3 | — | |
| Relay module | IN | **D5** | 14 | active LOW on most boards |
| Relay module | VCC | **VIN (5 V)** | — | |
| Buzzer (optional) | + | **D7** | 13 | active buzzer |
| Latch reset button | — | **D3 (FLASH)** | 0 | **optional**, `RESET_BTN_FITTED 0` by default; anything holding D3 low looks like a permanent press |

**Do not use D0 (GPIO16), D3 (GPIO0) or D8 (GPIO15) for sensors.** They are boot-strapping
pins — a sensor pulling them at power-up stops the board from booting.

---

## 1a. INA219 - the recommended power front-end

One I2C module measures **bus voltage and current together**, replacing both
the 0-25 V divider and the ACS712, and using no analog pin at all. It is the
default in `config.h` (`USE_INA219 1`, address `0x40`).

| INA219 pin | Goes to |
|---|---|
| VCC | NodeMCU **3V3** |
| GND | GND (common) |
| SDA | **D2** (GPIO4) |
| SCL | **D1** (GPIO5) |
| A0, A1 | GND -> address `0x40` |
| **VIN+** | supply **+**, from the relay NO contact |
| **VIN-** | motor **+** terminal |

VIN+ and VIN- are the screw terminals: the motor current flows **through** the
module between them, across its 0.1 ohm shunt. Bus voltage is measured at VIN-,
so the reading is the rail the motor actually sees, after the relay.

Leave A0/A1 tied to GND unless you need a second module on the same bus
(`0x41`, `0x44`, `0x45`).

Range is set by `INA219_RANGE` in `config.h`. The default 32 V / 2 A is the
safe choice: a stalled motor draws several times its running current, and a
narrower range simply clips.

Why this is better than the two-sensor arrangement it replaces: one device
instead of two, no analog channel contention, no ACS712 zero-point drift, no
5 V signal feeding a 3.3 V ADC input, and current and voltage sampled from the
same point in the circuit.

## 1b. The LCD is optional - and it can take the whole bus down

`firmware/config.h` ships with **`LCD_FITTED 0`**. With the flag at 0 the
display is compiled out completely: no library, no object, no I2C traffic for
it. The web dashboard is then the only readout, which loses nothing - it shows
strictly more than 16x2 characters ever did.

Set it to `1` only when a known-good display is wired **and powered from 5 V**.

The reason this matters: a PCF8574 backpack with no VCC clamps SDA and SCL to
ground through its internal protection diodes. That is not a weak pull-down you
can design around - it stops the ESP8266 from driving the bus at all, so *every*
I2C address returns error 4 and the ADS1115 disappears with it. The symptoms are
a blank screen, `BUS FAULT on 126 of 126` from `tools/i2c_scanner`, 0.00 V on
the dashboard, and an ML verdict of "Undervoltage / Brownout" that is really a
missing wire three steps upstream.

If the display misbehaves, `LCD_FITTED 0` is a legitimate permanent answer, not
a workaround.

## 2. Why an ADS1115 is in the list

The ESP8266 has **exactly one ADC input (A0)**, and this project needs **two**
analog channels: the voltage sensor and the ACS712 current sensor. There is no
way around that with the ESP8266 alone.

Two options, both supported by the firmware:

| | `USE_ADS1115 1` *(recommended)* | `USE_ADS1115 0` *(fallback)* |
|---|---|---|
| Extra part | ADS1115 breakout (~₹150) | none |
| Voltage | ADS channel A0 | ESP **A0** |
| Current | ADS channel A1 | **not measured** — dashboard shows "No sensor", model uses an imputed value |
| Resolution | 16-bit | 10-bit |
| Overload protection | yes | no |

Set the mode at the top of `firmware/config.h`. Everything else is identical.

---

## 3. Power

```
  12 V / 6 V bench supply ──┬── Relay COM
                            │
                            └── Buck converter (to 5 V) ── NodeMCU VIN
                                                        ── LCD VCC
                                                        ── Relay VCC
                                                        ── ACS712 VCC
  Relay NO ──────────────────── DC motor + terminal
  Motor  − terminal ─────────── supply GND
```

Rules that matter:

1. **Common ground.** Supply GND, NodeMCU GND, relay GND and all sensor GNDs must be
   tied together, or the ADC readings are meaningless.
2. **Never power the motor from the NodeMCU 5 V pin.** The relay switches the
   supply rail directly; the ESP only drives the relay coil input.
3. **Flyback diode.** Put a 1N4007 across the DC motor terminals (band to +) and a
   100 nF ceramic capacitor across the brushes. A brushed DC motor without this
   will reset the ESP and inject false vibration pulses.
4. The ACS712 goes **in series** with the motor's positive lead, after the relay.
5. The voltage sensor goes **across** the motor supply (VCC/GND of the module to the
   rail being measured).

---

## 4. Sensor placement for vibration analysis

The SW-420 must be **mechanically coupled** to the motor, not dangling on wires:

- Mount the motor on a small plate or on the chassis, not hand-held.
- Fix the SW-420 PCB flat on the motor body or on the plate next to it, with
  double-sided foam tape or a cable tie, with the spring sensor oriented
  **perpendicular to the shaft**.
- Tune the sensitivity trimmer so that a healthy running motor gives roughly
  **2–10 events/second** on the dashboard. That is your baseline; the thresholds
  of 18 (watch) and 35 (trip) are calibrated against it.
- Re-tune the trimmer any time you re-mount the sensor.

To create a repeatable "faulty" condition for your demo, tape a small washer or
blob of putty to the motor's fan/flywheel — that is a genuine imbalance and will
push the vibration rate into the fault band.

---

## 5. I²C address check

If the LCD stays blank or shows only blocks, run an I²C scanner sketch. Typical
results:

- `0x27` or `0x3F` — LCD backpack → set `LCD_I2C_ADDR`
- `0x48` — ADS1115 → set `ADS1115_ADDR`

If the ADS1115 does not appear, the firmware logs `[WARN] ADS1115 not found` and
keeps running with current measurement disabled.

---

## 6. Calibration

| Reading wrong | Fix |
|---|---|
| Voltage reads ~0 | Sensor output on the wrong ADC channel, or module GND not common |
| Voltage off by a constant factor | Measure the real rail with a multimeter, then set `VOLT_TRIM = real / displayed` in `config.h` |
| Current non-zero with the motor stopped | Let the board idle after power-up — `calibrateCurrentZero()` samples the ACS712 offset at boot with the relay open |
| Current reads half / double | Wrong `ACS712_MV_PER_AMP` — 185 for the 5 A board, 100 for 20 A, 66 for 30 A |
| Temperature stuck at the same value | DHT11 needs ≥ 2 s between reads; check the 10 kΩ pull-up |
| Vibration pegged at a huge number | Sensitivity trimmer too high, or no flyback diode on the motor |
