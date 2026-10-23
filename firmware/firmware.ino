/* =====================================================================
 *  Synaptix Robotics
 *  Machine Learning-Based Predictive Maintenance of Electrical Motors
 *  Using Vibration Analysis
 *
 *  Board    : NodeMCU 1.0 (ESP-12E) / Wemos D1 mini   [ESP8266]
 *  Sensors  : Voltage module 0-25V, ACS712 current, SW-420 vibration,
 *             DHT11 temperature+humidity
 *  Output   : 1-ch relay (motor supply), buzzer, web dashboard
 *  ML       : PRE-TRAINED decision tree, inference only (firmware/model.h)
 *
 *  Libraries to install (Arduino IDE -> Library Manager):
 *    - DHT sensor library           by Adafruit  (+ Adafruit Unified Sensor)
 *    - Adafruit ADS1X15             by Adafruit   (only if USE_ADS1115 == 1)
 *    - ArduinoJson (v7)             by Benoit Blanchon
 *  Board manager URL:
 *    https://arduino.esp8266.com/stable/package_esp8266com_index.json
 * ===================================================================== */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <DHT.h>
#include <ArduinoJson.h>

#include "config.h"
#include "model.h"

#if USE_INA219
  #include <Adafruit_INA219.h>
  Adafruit_INA219 ina(INA219_ADDR);
  bool inaOk = false;
#elif USE_ADS1115
  #include <Adafruit_ADS1X15.h>
  Adafruit_ADS1115 ads;
  bool adsOk = false;
#endif

/* The I2C bus only needs starting if something lives on it. */
#define I2C_NEEDED  (USE_INA219 || USE_ADS1115)

DHT dht(PIN_DHT, DHT11);
ESP8266WebServer server(80);

/* ------------------------- live state ------------------------------- */
struct Telemetry {
  float voltage = 0, current = 0, power = 0;
  float temperature = NAN, humidity = NAN;
  float vibRate = 0, vibPeak = 0;
  bool  relayOn = false;
  bool  latched = false;          // a trip is latched until reset
  uint8_t cls = CLS_HEALTHY;
  float confidence = 0;
  uint8_t severity = 0;
  uint16_t rulHours = 0;
  uint32_t uptimeS = 0;
  uint32_t tripCount = 0;
  uint32_t runSeconds = 0;        // seconds the relay has been closed
  bool  currentSensorOk = true;
  bool  voltageSensorOk = true;
} T;

/* runtime-editable thresholds, seeded from config.h */
struct Thresholds {
  float vMax = TH_VOLTAGE_MAX, vMin = TH_VOLTAGE_MIN;
  float iMax = TH_CURRENT_MAX;
  float tWarn = TH_TEMP_WARN,  tMax = TH_TEMP_MAX;
  float vibWarn = TH_VIB_WARN, vibMax = TH_VIB_MAX;
} TH;

/* ring buffer for the dashboard chart */
struct Frame { float v, i, t, vib; uint8_t cls; };
Frame  history[HISTORY_LEN];
uint16_t histCount = 0, histHead = 0;

/* vibration pulse counting */
volatile uint32_t vibPulses = 0;
volatile uint32_t vibLastUs = 0;
uint32_t vibSubWindowPeak = 0, vibSubWindowCount = 0;
uint32_t lastSubWindowMs = 0;

float acsZeroVolts = ACS712_ZERO_VOLTS;
char  lastTripReason[64] = "none";

uint32_t lastSampleMs = 0, lastDhtMs = 0;

/* Boot-time I2C census, published on /api/status so the bus can be
   diagnosed over the network instead of through the Serial Monitor. */
uint8_t  i2cFound[8]  = {0};
uint8_t  i2cCount     = 0;
uint16_t i2cBusErrors = 0;

/* ===================================================================
 *  Interrupt: SW-420 digital output
 * =================================================================== */
void IRAM_ATTR onVibrationPulse() {
  /* Reject edges closer together than the debounce window: that is
     comparator chatter, not a mechanical event. */
  uint32_t now = micros();
  if (now - vibLastUs < VIB_DEBOUNCE_US) return;
  vibLastUs = now;
  vibPulses++;
}

/* ===================================================================
 *  Boot-time I2C bus report (prints to Serial, costs ~30 ms)
 *  The ADS1115 lives on this bus, so when current/voltage readings go
 *  missing this is the first thing to read.
 * =================================================================== */
uint8_t scanI2C() {
  uint8_t  found = 0;
  uint16_t busErrors = 0;
  uint32_t t0 = millis();
  i2cCount = 0;
  Serial.println(F("[I2C] scanning bus (SDA=GPIO4/D2, SCL=GPIO5/D1)..."));
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 4) busErrors++;
    if (err == 0) {
      found++;
      if (i2cCount < 8) i2cFound[i2cCount++] = addr;
      Serial.printf("[I2C]   found 0x%02X%s\n", addr,
        (addr == 0x27 || addr == 0x3F) ? "  (LCD backpack)" :
        (addr >= 0x48 && addr <= 0x4B) ? "  (ADS1115)" :
        (addr == 0x40 || addr == 0x41 ||
         addr == 0x44 || addr == 0x45) ? "  (INA219)"  : "");
    }
  }
  i2cBusErrors = busErrors;
  Serial.printf("[I2C]   scan finished in %lu ms\n", (unsigned long)(millis() - t0));
  if (busErrors > 100)
    Serial.println(F("[I2C]   BUS FAULT on every address - a line is stuck "
                     "low or SDA/SCL are shorted. Run tools/i2c_linetest."));
  else if (!found)
    Serial.println(F("[I2C]   NOTHING ON THE BUS - check SDA/SCL wiring, "
                     "ADS1115 on 3V3, and a common GND."));
  return found;
}

/* ===================================================================
 *  Relay helpers
 * =================================================================== */
void relayWrite(bool on) {
  T.relayOn = on;
#if RELAY_ACTIVE_LOW
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
#else
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
#endif
}

/* ---- Non-blocking buzzer -------------------------------------------
 * A repeating (on_ms, off_ms) pattern, stepped from loop(). Nothing here
 * ever calls delay(): a blocking beep in the fault path would stall the
 * web server for as long as it sounded, exactly when the operator most
 * needs the dashboard to respond.
 * ------------------------------------------------------------------ */
uint16_t buzzOnMs = 0, buzzOffMs = 0;
uint32_t buzzEdgeMs = 0;
bool     buzzHigh = false;
bool     buzzMuted = false;

static inline void buzzerWrite(bool on) {
#if BUZZER_FITTED
  #if BUZZER_PASSIVE
    /* A bare piezo makes no sound on steady DC - it needs a square wave. */
    if (on) tone(PIN_BUZZER, BUZZER_TONE_HZ);
    else    noTone(PIN_BUZZER);
  #elif BUZZER_ACTIVE_HIGH
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  #else
    digitalWrite(PIN_BUZZER, on ? LOW : HIGH);
  #endif
#else
  (void)on;
#endif
}

/* (0, 0) silences it. Re-asserting the same pattern does not restart it. */
void buzzerPattern(uint16_t onMs, uint16_t offMs) {
  if (buzzOnMs == onMs && buzzOffMs == offMs) return;
  buzzOnMs = onMs; buzzOffMs = offMs;
  buzzEdgeMs = millis();
  buzzHigh = false;
  buzzerWrite(false);
}

void buzzerUpdate() {
  if (buzzMuted || buzzOnMs == 0) {
    if (buzzHigh) { buzzHigh = false; buzzerWrite(false); }
    return;
  }
  uint32_t now = millis();
  if (now - buzzEdgeMs >= (uint32_t)(buzzHigh ? buzzOnMs : buzzOffMs)) {
    buzzEdgeMs = now;
    buzzHigh = !buzzHigh;
    buzzerWrite(buzzHigh);
  }
}

void tripMotor(const char* reason) {
  if (T.latched) return;
  relayWrite(false);
  T.latched = true;
  T.tripCount++;
  strncpy(lastTripReason, reason, sizeof(lastTripReason) - 1);
  lastTripReason[sizeof(lastTripReason) - 1] = 0;
  Serial.printf("[TRIP] %s\n", reason);
  buzzMuted = false;                 // a new trip always un-mutes
  buzzerPattern(ALARM_ON_MS, ALARM_OFF_MS);
}

void clearLatch() {
  T.latched = false;
  strncpy(lastTripReason, "none", sizeof(lastTripReason));
  relayWrite(true);
  buzzMuted = false;
  buzzerPattern(0, 0);
  Serial.println("[LATCH] cleared, motor re-energised");
}

/* ===================================================================
 *  Sensor reads
 * =================================================================== */
float readVoltage() {
#if USE_INA219
  if (!inaOk) { T.voltageSensorOk = false; return 0.0f; }
  /* Bus voltage = the rail the motor actually sees, measured at VIN-. */
  return ina.getBusVoltage_V() * VOLT_TRIM;
#elif USE_ADS1115
  if (adsOk) {
    int16_t raw = ads.readADC_SingleEnded(0);
    float vAdc = (raw * ADS_GAIN_FS_VOLTS) / 32767.0f;
    return vAdc * VOLT_DIVIDER_RATIO * VOLT_TRIM;
  }
  /* No ADS1115 on the bus. Report "no sensor" rather than a fake 0.00 V,
     which the model would otherwise classify as a brownout. */
  T.voltageSensorOk = false;
  return 0.0f;
#else
  uint32_t acc = 0;
  for (uint8_t k = 0; k < 16; k++) { acc += analogRead(PIN_VOLT_ADC); delayMicroseconds(200); }
  float vAdc = (acc / 16.0f) * ESP_ADC_FS_VOLTS / 1023.0f;
  return vAdc * VOLT_DIVIDER_RATIO * VOLT_TRIM;
#endif
}

float readCurrent() {
#if USE_INA219
  if (!inaOk) { T.currentSensorOk = false; return 0.0f; }
  float a = fabsf(ina.getCurrent_mA()) / 1000.0f;
  return a < CURRENT_DEADBAND_A ? 0.0f : a;
#elif !CURRENT_SENSOR_FITTED
  T.currentSensorOk = false;    // no ACS712 in this build
  return 0.0f;
#elif USE_ADS1115
  if (!adsOk) { T.currentSensorOk = false; return 0.0f; }
  /* RMS-ish: average the absolute deviation from the zero point */
  float acc = 0;
  const uint8_t N = 32;
  for (uint8_t k = 0; k < N; k++) {
    int16_t raw = ads.readADC_SingleEnded(1);
    float vAdc = (raw * ADS_GAIN_FS_VOLTS) / 32767.0f;
    acc += fabsf(vAdc - acsZeroVolts);
    delayMicroseconds(300);
  }
  float amps = (acc / N) * 1000.0f / ACS712_MV_PER_AMP;
  return amps < CURRENT_DEADBAND_A ? 0.0f : amps;
#else
  T.currentSensorOk = false;   // no second ADC channel available
  return 0.0f;
#endif
}

void calibrateCurrentZero() {
#if USE_ADS1115 && CURRENT_SENSOR_FITTED
  if (!adsOk) return;
  float acc = 0;
  for (uint8_t k = 0; k < 64; k++) {
    acc += (ads.readADC_SingleEnded(1) * ADS_GAIN_FS_VOLTS) / 32767.0f;
    delay(2);
  }
  acsZeroVolts = acc / 64.0f;
  Serial.printf("[CAL] ACS712 zero = %.3f V\n", acsZeroVolts);
#endif
}

/* ===================================================================
 *  History
 * =================================================================== */
void pushHistory() {
  history[histHead] = { T.voltage, T.current, T.temperature, T.vibRate, T.cls };
  histHead = (histHead + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

/* ===================================================================
 *  Hard rule layer  (runs BEFORE the model — safety never depends on ML)
 * =================================================================== */
void applyProtectionRules() {
  if (T.voltageSensorOk && T.voltage > TH.vMax) {
    char msg[40]; snprintf(msg, sizeof(msg), "OVERVOLT %.2fV", T.voltage);
    tripMotor(msg); return;
  }
  if (!isnan(T.temperature) && T.temperature > TH.tMax) {
    char msg[40]; snprintf(msg, sizeof(msg), "OVERHEAT %.1fC", T.temperature);
    tripMotor(msg); return;
  }
  if (T.vibRate > TH.vibMax) {
    char msg[40]; snprintf(msg, sizeof(msg), "VIBRATION %.0f/s", T.vibRate);
    tripMotor(msg); return;
  }
  if (T.currentSensorOk && T.current > TH.iMax) {
    char msg[40]; snprintf(msg, sizeof(msg), "OVERLOAD %.2fA", T.current);
    tripMotor(msg); return;
  }
}

/* ===================================================================
 *  One feature frame: sample -> rules -> ML -> log
 * =================================================================== */
void sampleAndClassify() {
  /* vibration: pulses in the last window, converted to events/second */
  noInterrupts();
  uint32_t pulses = vibPulses; vibPulses = 0;
  interrupts();
  T.vibRate = pulses * (1000.0f / SAMPLE_WINDOW_MS) * VIB_SCALE;
  T.vibPeak = vibSubWindowPeak * 10.0f * VIB_SCALE;  // 100 ms window -> per second
  vibSubWindowPeak = 0;

  T.voltage = readVoltage();
  T.current = readCurrent();
  T.power   = T.voltage * T.current;

  if (millis() - lastDhtMs >= DHT_PERIOD_MS) {
    lastDhtMs = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) T.temperature = t;
    if (!isnan(h)) T.humidity = h;
  }

  applyProtectionRules();

  float f[N_FEATURES] = {
    T.voltageSensorOk ? T.voltage : 5.0f,      // imputed nominal when no sensor
    T.currentSensorOk ? T.current : 0.35f,     // imputed mean when no sensor
    isnan(T.temperature) ? 30.0f : T.temperature,
    T.vibRate,
    T.vibPeak,
    T.power
  };
  MlResult r   = mlPredict(f);
  T.cls        = r.cls;
  T.confidence = r.confidence;
  T.severity   = r.severity;
  T.rulHours   = mlEstimateRulHours(f, r.cls);

  /* ML backs up the rules: a severity-3 class also opens the relay */
  if (r.severity >= 3 && !T.latched) {
    char msg[40]; snprintf(msg, sizeof(msg), "ML:%s", r.shortName);
    tripMotor(msg);
  }

  /* Drive the buzzer from the current state, every frame. */
  if (T.latched)            buzzerPattern(ALARM_ON_MS, ALARM_OFF_MS);
  else if (r.severity >= 2) buzzerPattern(WARN_ON_MS, WARN_OFF_MS);
  else                      buzzerPattern(0, 0);

  if (T.relayOn) T.runSeconds++;
  T.uptimeS = millis() / 1000;
  pushHistory();

  Serial.printf("V=%.2f I=%.2f T=%.1f VIB=%.0f -> %s (%.0f%%)\n",
                T.voltage, T.current, T.temperature, T.vibRate,
                CLASS_NAME[T.cls], T.confidence * 100);
}

/* ===================================================================
 *  HTTP API
 * =================================================================== */
void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() { sendCors(); server.send(204); }

void handleStatus() {
  JsonDocument doc;
  doc["device"]      = "Synaptix PDM Motor Node";
  doc["ip"]          = WiFi.localIP().toString();
  doc["rssi"]        = WiFi.RSSI();
  doc["uptime_s"]    = T.uptimeS;
  doc["run_seconds"] = T.runSeconds;

  JsonObject s = doc["sensors"].to<JsonObject>();
  s["voltage"]     = roundf(T.voltage * 100) / 100.0f;
  s["current"]     = roundf(T.current * 100) / 100.0f;
  s["power"]       = roundf(T.power * 100) / 100.0f;
  s["temperature"] = isnan(T.temperature) ? 0 : roundf(T.temperature * 10) / 10.0f;
  s["humidity"]    = isnan(T.humidity) ? 0 : roundf(T.humidity * 10) / 10.0f;
  s["vibration"]   = T.vibRate;
  s["vib_peak"]    = T.vibPeak;
  s["current_sensor_ok"] = T.currentSensorOk;
  s["voltage_sensor_ok"] = T.voltageSensorOk;

  JsonObject m = doc["ml"].to<JsonObject>();
  m["model"]       = MODEL_VERSION;
  m["trained_on"]  = MODEL_TRAINED;
  m["class_id"]    = T.cls;
  m["fault"]       = CLASS_NAME[T.cls];
  m["confidence"]  = roundf(T.confidence * 1000) / 1000.0f;
  m["severity"]    = T.severity;
  m["suggestion"]  = CLASS_ACTION[T.cls];
  m["rul_hours"]   = T.rulHours;

  JsonObject d = doc["diagnostics"].to<JsonObject>();
#if USE_INA219
  d["sensor"] = "INA219";
  d["sensor_ok"] = inaOk;
#elif USE_ADS1115
  d["sensor"] = "ADS1115";
  d["sensor_ok"] = adsOk;
#else
  d["sensor"] = "ESP A0";
  d["sensor_ok"] = true;
#endif
  d["i2c_bus_errors"] = i2cBusErrors;
  d["i2c_bus_fault"]  = (i2cBusErrors > 100);
  JsonArray devs = d["i2c_devices"].to<JsonArray>();
  for (uint8_t k = 0; k < i2cCount; k++) {
    char hex[6];
    snprintf(hex, sizeof(hex), "0x%02X", i2cFound[k]);
    devs.add(hex);
  }
  d["free_heap"] = ESP.getFreeHeap();

  JsonObject p = doc["protection"].to<JsonObject>();
  p["relay_on"]    = T.relayOn;
  p["latched"]     = T.latched;
  p["trip_count"]  = T.tripCount;
  p["last_trip"]   = lastTripReason;

  JsonObject th = doc["thresholds"].to<JsonObject>();
  th["voltage_max"] = TH.vMax;  th["voltage_min"] = TH.vMin;
  th["current_max"] = TH.iMax;
  th["temp_warn"]   = TH.tWarn; th["temp_max"]    = TH.tMax;
  th["vib_warn"]    = TH.vibWarn; th["vib_max"]   = TH.vibMax;

  JsonArray hist = doc["history"].to<JsonArray>();
  for (uint16_t k = 0; k < histCount; k++) {
    uint16_t idx = (histHead + HISTORY_LEN - histCount + k) % HISTORY_LEN;
    JsonArray row = hist.add<JsonArray>();
    row.add(roundf(history[idx].v * 100) / 100.0f);
    row.add(roundf(history[idx].i * 100) / 100.0f);
    row.add(roundf(history[idx].t * 10) / 10.0f);
    row.add(history[idx].vib);
    row.add(history[idx].cls);
  }

  String out; serializeJson(doc, out);
  sendCors();
  server.send(200, "application/json", out);
}

void handleRelay() {
  String state = server.arg("state");
  if (state == "off") { relayWrite(false); }
  else if (state == "on") {
    if (T.latched) { sendCors(); server.send(409, "application/json",
      "{\"error\":\"latched\",\"message\":\"Clear the fault latch first\"}"); return; }
    relayWrite(true);
  } else { sendCors(); server.send(400, "application/json",
      "{\"error\":\"use ?state=on|off\"}"); return; }
  sendCors();
  server.send(200, "application/json", String("{\"relay_on\":") + (T.relayOn ? "true" : "false") + "}");
}

void handleBuzzer() {
  String st = server.arg("state");
  if (st == "test") {
    buzzMuted = false;
    buzzerPattern(ALARM_ON_MS, ALARM_OFF_MS);
  } else if (st == "mute") {
    buzzMuted = true;
  } else if (st == "off") {
    buzzMuted = false;
    buzzerPattern(0, 0);
  } else {
    sendCors();
    server.send(400, "application/json",
                "{\"error\":\"use ?state=test|mute|off\"}");
    return;
  }
  sendCors();
  server.send(200, "application/json",
              String("{\"muted\":") + (buzzMuted ? "true" : "false") + "}");
}

void handleReset() {
  clearLatch();
  sendCors();
  server.send(200, "application/json", "{\"ok\":true,\"latched\":false}");
}

void handleConfig() {
  if (server.method() == HTTP_POST) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendCors(); server.send(400, "application/json", "{\"error\":\"bad json\"}"); return;
    }
    if (doc["voltage_max"].is<float>()) TH.vMax    = doc["voltage_max"];
    if (doc["voltage_min"].is<float>()) TH.vMin    = doc["voltage_min"];
    if (doc["current_max"].is<float>()) TH.iMax    = doc["current_max"];
    if (doc["temp_warn"].is<float>())   TH.tWarn   = doc["temp_warn"];
    if (doc["temp_max"].is<float>())    TH.tMax    = doc["temp_max"];
    if (doc["vib_warn"].is<float>())    TH.vibWarn = doc["vib_warn"];
    if (doc["vib_max"].is<float>())     TH.vibMax  = doc["vib_max"];
  }
  JsonDocument r;
  r["voltage_max"] = TH.vMax; r["voltage_min"] = TH.vMin;
  r["current_max"] = TH.iMax; r["temp_warn"]   = TH.tWarn;
  r["temp_max"]    = TH.tMax; r["vib_warn"]    = TH.vibWarn;
  r["vib_max"]     = TH.vibMax;
  String out; serializeJson(r, out);
  sendCors(); server.send(200, "application/json", out);
}

/* Minimal built-in page, so the node is usable with no dashboard at all */
void handleRoot() {
  String p = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Synaptix PDM Node</title>"
    "<style>body{font-family:system-ui;background:#f6f8fb;color:#16202e;max-width:34rem;margin:2rem auto;padding:0 1rem}"
    "code{background:#e8edf5;padding:.15rem .35rem;border-radius:4px}"
    "a{color:#0b6ef3}.w{position:fixed;right:12px;bottom:10px;font-size:.7rem;color:#9aa7b8}</style>"
    "<h2>Synaptix PDM Motor Node</h2><p>Firmware is running. Open the dashboard and enter this device IP.</p>"
    "<ul><li><code>GET /api/status</code></li><li><code>GET /api/relay?state=on|off</code></li>"
    "<li><code>GET /api/reset</code></li><li><code>GET|POST /api/config</code></li></ul>"
    "<p><a href='/api/status'>View live JSON</a></p><div class=w>SYNAPTIX ROBOTICS</div>");
  sendCors();
  server.send(200, "text/html", p);
}

/* ===================================================================
 *  Setup
 * =================================================================== */
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\nSynaptix Robotics - Predictive Maintenance Node"));

  pinMode(PIN_RELAY, OUTPUT);
  relayWrite(false);                       // fail-safe: motor off at boot
#if BUZZER_FITTED
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
  #if BUZZER_BOOT_TEST
    buzzerWrite(true);  delay(150);   // setup() only - the server is not up yet
    buzzerWrite(false);
    Serial.println(F("[BUZZER] boot chirp sent. Silence here means wrong "
                     "polarity or a passive buzzer - see config.h."));
  #endif
#endif
  pinMode(PIN_VIBRATION, INPUT);
#if RESET_BTN_FITTED
  pinMode(PIN_RESET_BTN, INPUT_PULLUP);
#endif
#if VIB_EDGE_BOTH
  attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVibrationPulse, CHANGE);
#else
  attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVibrationPulse, FALLING);
#endif

#if I2C_NEEDED
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  /* Cap how long the master waits for a slave to release SCL. The core
     default is long enough that a floating line stalls ~0.8 s PER address,
     turning a 30 ms bus scan into a 100 s boot delay. 1 ms is far more than
     any device here needs. */
  Wire.setClock(100000);
  Wire.setClockStretchLimit(1000);
  scanI2C();
#else
  Serial.println(F("[I2C] no I2C devices configured - bus not started"));
#endif

  dht.begin();

#if USE_INA219
  inaOk = ina.begin();
  if (inaOk) {
  #if   INA219_RANGE == 1
    ina.setCalibration_32V_1A();
  #elif INA219_RANGE == 2
    ina.setCalibration_16V_400mA();
  #else
    ina.setCalibration_32V_2A();
  #endif
    Serial.printf("[INA219] ready at 0x%02X\n", INA219_ADDR);
  } else {
    Serial.println(F("[WARN] INA219 not found - check wiring/address. "
                     "Voltage and current will read as 'no sensor'."));
  }
#elif USE_ADS1115
  adsOk = ads.begin(ADS1115_ADDR);
  if (adsOk) { ads.setGain(GAIN_ONE); ads.setDataRate(RATE_ADS1115_250SPS); }
  else Serial.println(F("[WARN] ADS1115 not found - check wiring/address"));
  calibrateCurrentZero();
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("Connecting to Wi-Fi"));
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) { delay(500); Serial.print('.'); }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASS);
    Serial.printf("\nAP mode: %s  IP: %s\n", AP_FALLBACK_SSID,
                  WiFi.softAPIP().toString().c_str());
  }

  if (MDNS.begin(MDNS_HOST)) Serial.printf("mDNS: http://%s.local\n", MDNS_HOST);

  server.on("/",            HTTP_GET,     handleRoot);
  server.on("/api/status",  HTTP_GET,     handleStatus);
  server.on("/api/relay",   HTTP_GET,     handleRelay);
  server.on("/api/reset",   HTTP_GET,     handleReset);
  server.on("/api/buzzer",  HTTP_GET,     handleBuzzer);
  server.on("/api/config",  HTTP_GET,     handleConfig);
  server.on("/api/config",  HTTP_POST,    handleConfig);
  server.on("/api/status",  HTTP_OPTIONS, handleOptions);
  server.on("/api/config",  HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { sendCors(); server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();
  Serial.println(F("HTTP server on port 80"));

  delay(1200);
  relayWrite(true);                        // energise the motor
  lastSampleMs = millis();
}

/* ===================================================================
 *  Loop
 * =================================================================== */
void loop() {
  server.handleClient();
  MDNS.update();
  buzzerUpdate();

  /* 100 ms sub-window -> peak vibration density inside the second */
  if (millis() - lastSubWindowMs >= 100) {
    lastSubWindowMs = millis();
    uint32_t snapshot = vibPulses;
    uint32_t delta = snapshot - vibSubWindowCount;
    vibSubWindowCount = snapshot;
    if (delta > vibSubWindowPeak) vibSubWindowPeak = delta;
  }

  if (millis() - lastSampleMs >= SAMPLE_WINDOW_MS) {
    lastSampleMs = millis();
    vibSubWindowCount = 0;
    sampleAndClassify();
  }

#if RESET_BTN_FITTED
  /* Physical latch-reset button (hold ~1 s), timed without blocking.
     A blocking delay() here is fatal: if the pin ever sits LOW the loop
     stalls a full second per pass and server.handleClient() is starved,
     so the whole HTTP API stops answering while the board looks alive. */
  static uint32_t btnDownMs = 0;
  static bool     btnWarned = false;
  if (digitalRead(PIN_RESET_BTN) == LOW) {
    if (btnDownMs == 0) {
      btnDownMs = millis();
    } else if (millis() - btnDownMs >= 1000) {
      if (T.latched) clearLatch();   // clearLatch() silences the alarm
      btnDownMs = 0;
    }
    /* Held low for 10 s is not a press - it is a wiring fault. Say so once. */
    if (!btnWarned && btnDownMs && millis() - btnDownMs >= 10000) {
      btnWarned = true;
      Serial.println(F("[WARN] reset button pin (D3/GPIO0) is stuck LOW. "
                       "Unplug whatever is on D3; it is not a button press."));
    }
  } else {
    btnDownMs = 0;
    btnWarned = false;
  }
#endif
}
