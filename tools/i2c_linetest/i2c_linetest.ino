/* =====================================================================
 *  i2c_linetest.ino  —  Synaptix Robotics
 *
 *  Run this when the I2C scanner reports "bus fault" (error 4) on every
 *  address. That error means the ESP cannot drive the bus at all, so
 *  scanning for chips is pointless - the two WIRES are the problem.
 *
 *  This sketch ignores I2C entirely and tests D1/D2 as plain GPIO:
 *    - can each line idle HIGH?   (pull-ups present, nothing clamping)
 *    - are the two lines shorted? (only checked when both idle HIGH -
 *      a line already held low would read LOW whatever we drive, so
 *      the drag test cannot tell a short from a grounded line)
 *
 *  Serial Monitor: 115200 baud.
 * ===================================================================== */

#define SDA_PIN  4    // D2
#define SCL_PIN  5    // D1

bool idleHigh(uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  return digitalRead(pin) == HIGH;
}

/* Drive `driven` LOW and see whether `other` follows it down. */
bool dragsOther(uint8_t driven, uint8_t other) {
  pinMode(other, INPUT_PULLUP);
  pinMode(driven, OUTPUT);
  digitalWrite(driven, LOW);
  delay(5);
  bool followed = (digitalRead(other) == LOW);
  pinMode(driven, INPUT_PULLUP);
  delay(5);
  return followed;
}

void runTest() {
  Serial.println(F("\n\n=============================================="));
  Serial.println(F(" Synaptix Robotics - I2C line test"));
  Serial.println(F(" SDA = GPIO4 (D2)    SCL = GPIO5 (D1)"));
  Serial.println(F("==============================================\n"));

  bool sdaHigh = idleHigh(SDA_PIN);
  bool sclHigh = idleHigh(SCL_PIN);

  Serial.printf("  SDA (D2) idles: %s\n", sdaHigh ? "HIGH  ok" : "LOW   <-- STUCK");
  Serial.printf("  SCL (D1) idles: %s\n", sclHigh ? "HIGH  ok" : "LOW   <-- STUCK");

  bool testable = sdaHigh && sclHigh;
  bool shorted  = testable && dragsOther(SDA_PIN, SCL_PIN)
                           && dragsOther(SCL_PIN, SDA_PIN);

  if (testable)
    Serial.printf("  SDA and SCL shorted: %s\n\n", shorted ? "YES  <-- FAULT" : "no");
  else
    Serial.println(F("  Short test: SKIPPED - meaningless while a line is\n"
                     "              held low (it reads LOW regardless).\n"));

  Serial.println(F("----------------- VERDICT --------------------"));

  if (!sdaHigh && !sclHigh) {
    Serial.println(F("  BOTH lines are held LOW - something is tying them"));
    Serial.println(F("  to ground, against the internal pull-ups."));
    Serial.println(F("  In order of likelihood:"));
    Serial.println(F("   - both jumpers landed on the breadboard GND rail,"));
    Serial.println(F("     or in a row that already carries a GND wire"));
    Serial.println(F("   - an UNPOWERED module clamping the bus: every"));
    Serial.println(F("     module on SDA/SCL needs its VCC actually seated"));
    Serial.println(F("     (INA219 -> 3V3). An unpowered LCD backpack is"));
    Serial.println(F("     the classic offender - unplug it entirely."));
    Serial.println(F("   - solder bridge to GND on a module's header"));
    Serial.println();
    Serial.println(F("  NEXT: unplug BOTH jumpers from D1 and D2, leave the"));
    Serial.println(F("  ESP powered, and re-run this sketch."));
    Serial.println(F("    both HIGH -> fault is off-board (breadboard or"));
    Serial.println(F("                 a module). Re-add one wire at a time."));
    Serial.println(F("    still LOW -> fault is on the NodeMCU itself;"));
    Serial.println(F("                 move I2C to other pins in config.h."));
  }
  else if (shorted) {
    Serial.println(F("  SDA and SCL are electrically the SAME NODE."));
    Serial.println(F("   - both jumpers pushed into the SAME breadboard row"));
    Serial.println(F("   - solder bridge across two pins on a module header"));
    Serial.println(F("   - D1 and D2 bridged on the NodeMCU header"));
    Serial.println(F("  Separate them, then re-run."));
  }
  else if (!sdaHigh) {
    Serial.println(F("  SDA (D2) alone is stuck LOW - shorted to GND, or a"));
    Serial.println(F("  device is holding it. Unplug modules one at a time."));
  }
  else if (!sclHigh) {
    Serial.println(F("  SCL (D1) alone is stuck LOW - shorted to GND, or a"));
    Serial.println(F("  device is holding it. Unplug modules one at a time."));
  }
  else {
    Serial.println(F("  Both lines are healthy and independent."));
    Serial.println(F(""));
    Serial.println(F("  If the INA219 is CONNECTED right now, the"));
    Serial.println(F("  wiring is good and a bus fault points at weak"));
    Serial.println(F("  pull-ups: add 4.7k from SDA->3V3 and SCL->3V3."));
    Serial.println(F(""));
    Serial.println(F("  If you ran this with the lines UNPLUGGED, this"));
    Serial.println(F("  only clears the NodeMCU. Plug ONE wire back,"));
    Serial.println(F("  reset, and re-read: the wire that drags a line"));
    Serial.println(F("  LOW is the faulty branch."));
  }
  Serial.println(F("----------------------------------------------"));
}

void setup() {
  Serial.begin(115200);
  delay(500);
}

/* Repeats so the output cannot be missed when the Serial
   Monitor reconnects after a reset. */
void loop() {
  runTest();
  delay(3000);
}
