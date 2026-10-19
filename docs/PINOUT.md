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
| Latch reset button | — | **D3 (FLASH)** | 0 | already on-board, to GND |

**Do not use D0 (GPIO16), D3 (GPIO0) or D8 (GPIO15) for sensors.** They are boot-strapping
pins — a sensor pulling them at power-up stops the board from booting.

---

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
