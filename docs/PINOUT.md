# Pinout & Wiring

Board: **NodeMCU 1.0 (ESP-12E)** or **Wemos D1 mini**. Pin numbers in the firmware
are GPIO numbers; the silkscreen label (D1, D2 …) is given alongside.

---

## 1. Master pin table

| Peripheral | Module pin | NodeMCU label | GPIO | Notes |
|---|---|---|---|---|
| INA219 current | SDA | **D2** | 4 | I²C, addr `0x40` |
| INA219 current | SCL | **D1** | 5 | I²C |
| INA219 current | VCC | **3V3** | — | logic side only |
| INA219 current | GND | GND | — | |
| INA219 current | A0 / A1 pads | GND | — | sets address `0x40` |
| INA219 current | VIN+ / VIN− | — | — | screw terminals, **in series** with the motor — see §3 |
| Voltage sensor (0–25 V) | S (out) | **A0** | — | the ESP's own ADC pin |
| Voltage sensor (0–25 V) | VCC / GND | across the supply rail | — | measures the rail ahead of the relay |
| DHT11 | DATA | **D4** | 2 | 10 kΩ pull-up to 3V3 |
| DHT11 | VCC | 3V3 | — | |
| SW-420 vibration | DO | **D6** | 12 | digital pulse, interrupt |
| SW-420 vibration | VCC | 3V3 | — | |
| Relay module | IN | **D5** | 14 | active LOW on most boards |
| Relay module | VCC | **VIN (5 V)** | — | |
| Buzzer (optional) | + | **D7** | 13 | active buzzer |
| Latch reset button | — | **D3 (FLASH)** | 0 | already on-board, to GND |

**Do not use D0 (GPIO16), D3 (GPIO0) or D8 (GPIO15) for sensors.** They are boot-strapping
pins — a sensor pulling them at power-up stops the board from booting.

---

## 1b. Nothing unpowered may sit on the I2C bus

The INA219 is the **only** I2C device in this design. Keep it that way, and in
particular never leave an unpowered module hanging on SDA/SCL.

The reason this matters, learned the hard way: a module with no VCC — a PCF8574
LCD backpack is the classic case — clamps SDA and SCL to ground through its
internal protection diodes. That is not a weak pull-down you can design around;
it stops the ESP8266 from driving the bus at all, so *every* I2C address returns
error 4 and the INA219 disappears with it. The symptoms are
`BUS FAULT on 126 of 126` from `tools/i2c_scanner`, a current reading of
"No sensor", and a long, confusing boot.

If an I2C device misbehaves, disconnect it entirely rather than leaving it wired
but dead. `CURRENT_SENSOR_FITTED 0` is a legitimate permanent answer for the
INA219 — with the flag at 0 the bus is never started at all.

## 2. Why the current sensor is an INA219

The ESP8266 has **exactly one ADC input (A0)**. An ACS712 is an analog sensor,
so with the voltage module on A0 there is no channel left for it — which is why
this project used to need an ADS1115 to split the one pin into two.

The INA219 sidesteps that entirely: shunt and 12-bit ADC on board, amps
delivered over I2C as a number. Voltage keeps the ESP's own A0, and the ADS1115
and ACS712 both leave the bill of materials.

| | `CURRENT_SENSOR_FITTED 1` *(default)* | `CURRENT_SENSOR_FITTED 0` |
|---|---|---|
| Extra part | INA219 breakout (~₹200) | none |
| Voltage | ESP **A0**, 10-bit (~5 mV on a 6 V rail) | ESP **A0** |
| Current | INA219 over I2C, 0.1 mA resolution | **not measured** — dashboard shows "No sensor", model uses an imputed value |
| I2C bus | one device at `0x40` | not started at all |
| Overload protection | yes | no |

Set the mode at the top of `firmware/config.h`. Everything else is identical.

---

## 3. Power

There are two separate circuits that never mix. Motor current flows along the
power path only; the NodeMCU just watches it and switches it.

**The power path — four connections, in order:**

```
  Supply +  ──────────────── Relay COM
  Relay NO  ──────────────── INA219 VIN+   (screw terminal)
  INA219 VIN− ────────────── Motor +       (screw terminal)
  Motor −   ──────────────── Supply −
```

The motor's current physically passes between the INA219's two screw terminals,
across the 0.1 Ω shunt inside it. Nothing else connects to VIN+ or VIN−.

**The logic path:**

```
  Supply + ── Buck converter (to 5 V) ── NodeMCU VIN
                                      ── Relay VCC
  NodeMCU 3V3 ───────────────────────── INA219 VCC
  NodeMCU D2 / D1 ───────────────────── INA219 SDA / SCL
```

Rules that matter:

1. **Common ground.** Supply −, NodeMCU GND, relay GND and INA219 GND must all be
   tied together. This is the single most common cause of nonsense readings.
2. **Never power the motor from the NodeMCU 5 V pin.** The relay switches the
   supply rail directly; the ESP only drives the relay coil input.
3. **Flyback diode.** Put a 1N4007 across the DC motor terminals (band to +) and a
   100 nF ceramic capacitor across the brushes. A brushed DC motor without this
   will reset the ESP and inject false vibration pulses.
4. The INA219 goes **in series** with the motor's positive lead, after the relay.
   Its logic side (VCC) runs from 3V3 — that is separate from the motor rail
   passing through VIN+/VIN−.
5. The voltage sensor goes **across** the motor supply (VCC/GND of the module to the
   rail being measured), ahead of the relay, so a trip does not blind it.

### Check this before you apply power

With the supply **off**, put a multimeter in resistance mode across the INA219's
VIN+ and VIN−. You should read near **0 Ω** — that is the shunt. Then measure
from VIN− to supply ground: you should read the motor's winding resistance,
typically a few ohms.

If VIN+ to VIN− reads the motor's winding resistance instead, the module is
wired **in parallel** with the motor rather than in series. Fix that before
applying power — a 0.1 Ω shunt straight across a supply will destroy the
module.

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

The firmware scans the bus at boot and prints what it finds. You can also run
`tools/i2c_scanner` on its own. Expected result:

- `0x40` — INA219 with both address pads to GND → matches `INA219_ADDR`
- `0x41`–`0x4F` — an address pad is tied to VCC instead; set `INA219_ADDR` to match

If the INA219 does not appear, the firmware logs `[WARN] INA219 not found` and
keeps running with current measurement disabled — voltage, temperature and
vibration protection are unaffected. If *every* address errors instead, a line
is clamped: run `tools/i2c_linetest`.

---

## 6. Calibration

| Reading wrong | Fix |
|---|---|
| Voltage reads ~0 | Sensor output on the wrong ADC channel, or module GND not common |
| Voltage off by a constant factor | Measure the real rail with a multimeter, then set `VOLT_TRIM = real / displayed` in `config.h` |
| Current reads 0 with the motor running | INA219 wired in parallel instead of in series, or VIN+/VIN− not actually carrying motor current — re-do the resistance check in §3 |
| Current pegged at ~1 A or ~0.4 A | Reading is saturating the range — raise `INA219_CAL` to `2` (32 V / 2 A) in `config.h` |
| Current slightly non-zero at rest | Normal shunt noise; raise `CURRENT_DEADBAND_A` a little |
| Temperature stuck at the same value | DHT11 needs ≥ 2 s between reads; check the 10 kΩ pull-up |
| Vibration pegged at a huge number | Sensitivity trimmer too high, or no flyback diode on the motor |
