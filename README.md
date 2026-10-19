# Machine Learning-Based Predictive Maintenance of Electrical Motors Using Vibration Analysis

**Synaptix Robotics**

An ESP8266 node watches a DC motor's voltage, current, winding temperature and
vibration signature. A **pre-trained** decision-tree classifier runs on the board
itself, names the fault, estimates remaining useful life, and tells the operator
what to do about it. Anything dangerous opens a relay and cuts the motor supply.
A light-theme web dashboard shows everything live.

> **Pre-trained means exactly that.** The model is trained offline in Python
> (`ml/train_model.py`) and frozen into a C header (`firmware/model.h`). The
> ESP8266 only runs inference — about 40 µs per sample, zero heap. No training,
> no cloud call, no internet dependency at run time.

---

## 1. What it does

| Condition | Threshold | What happens |
|---|---|---|
| Supply voltage above rating | **> 6.0 V** | Relay opens, LCD + buzzer alert, dashboard turns red, ML returns the overvoltage remedy |
| Winding temperature | **> 55 °C** | Relay opens, cool-down guidance |
| Vibration | **> 35 events/s** | Relay opens, imbalance guidance |
| Vibration elevated | **18 – 35 events/s** | No trip. Classified as bearing wear, maintenance scheduled, RUL drops to ~20 h |
| Motor current | **> 1.2 A** | Relay opens, stall/jam guidance |
| Supply sag | **< 3.5 V** | Brownout warning, no trip |

Two independent layers decide this, deliberately:

1. **Hard rules** in `applyProtectionRules()` — plain comparisons against the
   thresholds. Safety never depends on the model.
2. **The ML classifier** — names *which* fault it is, how confident it is, what
   the operator should do, and how much life is left. A severity-3 class also
   opens the relay, so the model can back the rules up but can never override
   them.

A trip is **latched**: the relay stays open until someone presses *Reset latch*
on the dashboard or holds the FLASH button on the board. That is on purpose — an
auto-restarting motor with a real fault is how equipment gets destroyed.

---

## 2. Repository layout

```
pdm-motor-ml/
├── firmware/
│   ├── firmware.ino      ESP8266 sketch — sensors, relay, LCD, web API
│   ├── config.h          Wi-Fi, pins, calibration, thresholds  ← edit this
│   └── model.h           PRE-TRAINED tree + suggestions (frozen export)
├── ml/
│   ├── train_model.py    Offline training + C export
│   └── requirements.txt
├── dashboard/
│   ├── index.html        Light-theme dashboard
│   ├── styles.css
│   └── app.js            Polls the ESP, draws charts, demo simulator
├── docs/
│   ├── PINOUT.md         Full pin table, power, calibration
│   └── DEPLOY.md         GitHub + Vercel, and the HTTPS caveat
├── vercel.json
└── README.md
```

---

## 3. Bill of materials

| # | Part | Qty | Note |
|---|---|---|---|
| 1 | NodeMCU ESP8266 (ESP-12E) | 1 | or Wemos D1 mini |
| 2 | DC motor, 6 V | 1 | the unit under test |
| 3 | 1-channel relay module 5 V | 1 | active-LOW type |
| 4 | Voltage sensor module 0–25 V | 1 | 30k/7.5k divider |
| 5 | ACS712 current sensor | 1 | 5 A version |
| 6 | SW-420 vibration sensor | 1 | digital output |
| 7 | DHT11 temperature + humidity | 1 | |
| 8 | I²C 16×2 LCD (PCF8574 backpack) | 1 | addr 0x27 or 0x3F |
| 9 | **ADS1115 16-bit I²C ADC** | 1 | **see the note below** |
| 10 | Buzzer (active, 5 V) | 1 | optional |
| 11 | 1N4007 diode + 100 nF cap | 1 | flyback across the motor — not optional in practice |
| 12 | Buck converter / 5 V supply | 1 | |

### The one part you didn't list, and why you need it

The ESP8266 has **one** ADC pin. Your project needs **two** analog channels
(voltage sensor **and** current sensor). That is a hardware limit of the chip,
not a software problem.

- **With an ADS1115** (`USE_ADS1115 1`, the default): both channels read properly
  at 16-bit resolution, and it shares the I²C bus the LCD already uses — two extra
  wires, nothing else changes.
- **Without one** (`USE_ADS1115 0`): voltage goes on the ESP's A0, current is not
  measured. The dashboard shows "No sensor" on the current tile and the model
  substitutes an imputed value. Everything else — overvoltage, overheat, vibration,
  relay cut-off, dashboard — works exactly the same.

Flip the switch in `firmware/config.h`. Both paths are written and supported.

---

## 4. Pinout (summary)

| Signal | NodeMCU | GPIO |
|---|---|---|
| LCD + ADS1115 SDA | **D2** | 4 |
| LCD + ADS1115 SCL | **D1** | 5 |
| DHT11 DATA | **D4** | 2 |
| Relay IN | **D5** | 14 |
| SW-420 DO | **D6** | 12 |
| Buzzer | **D7** | 13 |
| Latch reset | **D3** (FLASH) | 0 |
| Voltage sensor OUT | ADS **A0** | — |
| ACS712 OUT | ADS **A1** | — |

Full table, power wiring, flyback diode, sensor mounting and calibration:
**[docs/PINOUT.md](docs/PINOUT.md)**.

---

## 5. Build and flash

1. **Arduino IDE** → Preferences → Additional Board URLs:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
   then Boards Manager → install **esp8266**.
2. Library Manager → install:
   - *LiquidCrystal I2C* (Frank de Brabander)
   - *DHT sensor library* + *Adafruit Unified Sensor*
   - *Adafruit ADS1X15*
   - *ArduinoJson* (v7)
3. Open `firmware/firmware.ino`. In `config.h` set `WIFI_SSID`, `WIFI_PASSWORD`,
   and `USE_ADS1115`.
4. Board: **NodeMCU 1.0 (ESP-12E Module)**, Upload speed 115200.
5. Upload, then open Serial Monitor at **115200**. It prints the IP address —
   the LCD shows it too.

If Wi-Fi fails, the board starts its own access point `Synaptix-PDM` /
`synaptix123` at `192.168.4.1`, so the demo still works with no router.

---

## 6. Run the dashboard

**Locally (full live telemetry):**

```bash
npx serve dashboard
```

Open `http://localhost:3000`, type the ESP8266's IP into the box, press
**Connect**. The IP is remembered in the browser.

**Live on Vercel:** see [docs/DEPLOY.md](docs/DEPLOY.md). Short version — push to
GitHub, import into Vercel, done. One caveat you need to know about: a page served
over HTTPS cannot call the ESP8266's HTTP API (browsers block mixed content). The
dashboard says so plainly and **Demo mode works everywhere**, so the public link is
still fully interactive.

**Demo mode** runs a synthetic motor with fault-injection buttons — overvoltage,
overheat, bearing, overload. No hardware needed. Use it to rehearse the
demonstration and to show the fault path on a projector.

### What's on the dashboard

- ML verdict banner — fault name, confidence, severity, relay state, RUL, trip count
- Recommended action — the model's maintenance guidance, step by step
- Four live tiles with sparklines: voltage, current, winding temp, vibration
- Main time-series chart with the trip limit drawn in, crosshair + tooltip,
  and a "View as table" fallback
- Fault classification timeline — one colour band per second
- Threshold editor that writes straight to the board
- Event log with timestamps
- Motor controls: Start, Stop, Reset latch
- "SYNAPTIX ROBOTICS" watermark pinned to the corner

---

## 7. HTTP API

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Minimal built-in status page |
| `/api/status` | GET | Full telemetry + ML verdict + 120-sample history |
| `/api/relay?state=on\|off` | GET | Manual motor control (refuses `on` while latched) |
| `/api/reset` | GET | Clear the fault latch, re-energise |
| `/api/config` | GET / POST | Read or write thresholds |

All responses send `Access-Control-Allow-Origin: *` so the dashboard can be served
from anywhere.

<details>
<summary><code>/api/status</code> sample</summary>

```json
{
  "device": "Synaptix PDM Motor Node",
  "ip": "192.168.1.50", "rssi": -58, "uptime_s": 412, "run_seconds": 390,
  "sensors": { "voltage": 5.12, "current": 0.38, "power": 1.95,
               "temperature": 33.0, "humidity": 48.0,
               "vibration": 12, "vib_peak": 30, "current_sensor_ok": true },
  "ml": { "model": "dt-v1.3.0", "trained_on": "2026-10-12", "class_id": 0,
          "fault": "Healthy", "confidence": 0.981, "severity": 0,
          "suggestion": "No action. Keep logging; next scheduled greasing in 50 running hours.",
          "rul_hours": 420 },
  "protection": { "relay_on": true, "latched": false,
                  "trip_count": 0, "last_trip": "none" },
  "thresholds": { "voltage_max": 6, "temp_max": 55, "vib_max": 35, "current_max": 1.2 },
  "history": [[5.10,0.37,32.9,11,0], [5.12,0.38,33.0,12,0]]
}
```
</details>

---

## 8. The model

**Classifier:** `DecisionTreeClassifier(max_depth=5, min_samples_leaf=25,
class_weight='balanced')` — chosen because a tree is the only common classifier
that survives being compiled into a 17-node array and run on a 32-bit MCU with no
floating-point library tricks, while staying fully explainable. Every decision is
a readable comparison, which is exactly what you want when a relay is on the other
end of it.

**Features (6):** voltage, current, temperature, vibration rate, vibration peak
density, power.

**Classes (7):** Healthy, Overvoltage Stress, Undervoltage/Brownout, Mechanical
Overload, Thermal Overheat, Bearing Wear, Rotor Imbalance.

**Training data:** `ml/train_model.py` generates 17 500 labelled samples from
per-class operating envelopes (the physical ranges a 6 V bench motor actually sits
in for each condition), with Gaussian sensor noise added so the tree learns
tolerant boundaries rather than razor-thin ones. Test accuracy on the held-out
25 % split is printed when you run it.

**Retraining with your own data** — recommended before you submit, because your
motor and your SW-420 trimmer setting are not the ones these envelopes assume:

```bash
cd ml
pip install -r requirements.txt
python train_model.py                  # synthetic
python train_model.py --csv my_log.csv # your captures
```

Capture your own CSV by logging the Serial Monitor output while you run the motor
healthy, then with a washer taped to the flywheel, then stalled, and so on,
labelling each run. The script prints a classification report, confusion matrix
and feature importances, and writes `ml/model_generated.h` — paste its `TREE[]`
array over the one in `firmware/model.h` and bump `MODEL_VERSION`.

**Remaining useful life** is a monotone degradation estimate from temperature and
vibration stress, not a trained regressor — it is an indicator for scheduling, and
the code says so.

---

## 9. Honest limitations

Worth stating in your report; examiners ask.

- **"Vibration analysis" here is event-rate analysis, not FFT.** The SW-420 is a
  threshold switch, so the features are pulse rate and burst density, not a
  frequency spectrum. Real bearing-defect frequency analysis (BPFO/BPFI) needs an
  accelerometer such as the ADXL345 or MPU6050 sampled at a few hundred Hz plus an
  FFT. The architecture here takes that upgrade directly — swap the feature
  extractor, retrain, re-export; the firmware, API and dashboard are unchanged.
- **The training set is synthetic** unless you retrain on your own captures. It is
  built from realistic envelopes, but it is not field data.
- **DHT11 measures ambient air near the motor**, not winding temperature, and has
  ±2 °C accuracy with a 2-second floor on reads. Mount it against the motor body
  for the closest approximation, or move to a DS18B20 clamped to the casing.
- **RUL is an indicator, not a prediction.** A trained RUL model needs run-to-
  failure data this project does not have.
- **A page on HTTPS cannot reach the ESP8266 on HTTP.** Covered in DEPLOY.md.

---

## 10. Safety

- Keep the whole build at low voltage. A 6 V DC bench motor and a 12 V supply are
  the intended scope. **Do not wire this relay to mains.**
- The relay switches the motor's supply rail; the ESP never carries motor current.
- Fit the flyback diode and the snubber capacitor before the first run.
- The relay is opened at boot and only energised after the first sensor frame, so
  a board reset never slams the motor on.

---

<sub>Synaptix Robotics · Machine Learning-Based Predictive Maintenance of Electrical Motors Using Vibration Analysis</sub>
