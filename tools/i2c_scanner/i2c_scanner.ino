/* =====================================================================
 *  i2c_scanner.ino  —  Synaptix Robotics
 *  Diagnostic for the PDM motor node.
 *
 *  Flash this INSTEAD of firmware.ino when the
 *  firmware prints "[WARN] INA219 not found". It scans the I2C bus and
 *  tells you exactly which devices answered.
 *
 *  Serial Monitor: 115200 baud.
 * ===================================================================== */

#include <Wire.h>

#define PIN_I2C_SDA   4    // D2
#define PIN_I2C_SCL   5    // D1

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("================================================"));
  Serial.println(F(" Synaptix Robotics - I2C bus scanner"));
  Serial.println(F(" SDA = GPIO4 (D2)   SCL = GPIO5 (D1)"));
  Serial.println(F("================================================"));
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
}

void loop() {
  uint8_t  found = 0;
  uint16_t busErrors = 0;
  Serial.println(F("\nScanning 0x01 - 0x7E ..."));

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      found++;
      Serial.printf("  FOUND 0x%02X", addr);
      switch (addr) {
        case 0x27:
        case 0x3F: Serial.print(F("  <- PCF8574 (LCD backpack - not used "
                                  "by this project)")); break;
        case 0x40:
        case 0x41:
        case 0x44:
        case 0x45: Serial.print(F("  <- INA219 current sensor")); break;
        case 0x48:
        case 0x4B: Serial.print(F("  <- ADS1115 ADC (no longer used)")); break;
        case 0x68: Serial.print(F("  <- MPU6050 / DS3231")); break;
        case 0x3C: Serial.print(F("  <- SSD1306 OLED")); break;
      }
      Serial.println();
    } else if (err == 4) {
      busErrors++;           // counted, summarised below
    }
  }

  Serial.println(F("------------------------------------------------"));
  if (busErrors > 100) {
    Serial.printf("  BUS FAULT on %u of 126 addresses.\n", busErrors);
    Serial.println(F("  This is NOT a missing-chip problem. The ESP cannot"));
    Serial.println(F("  drive the bus at all - a line is stuck low, or SDA"));
    Serial.println(F("  and SCL are shorted together."));
    Serial.println(F("  Run tools/i2c_linetest to find out which."));
  }
  else if (found == 0) {
    Serial.println(F("  NO DEVICES FOUND."));
    Serial.println(F("  The bus itself is the problem, not the chips:"));
    Serial.println(F("   1. SDA and SCL swapped?  SDA->D2, SCL->D1"));
    Serial.println(F("   2. INA219 VCC on 3V3 and actually seated?"));
    Serial.println(F("   3. GND of every board tied to NodeMCU GND?"));
    Serial.println(F("   4. Jumper wire broken - swap it and retry."));
  } else {
    Serial.printf("  %u device(s) responded.\n", found);
    Serial.println(F("  0x40 present -> INA219 ok, keep"));
    Serial.println(F("                  CURRENT_SENSOR_FITTED 1."));
    Serial.println(F("  no 0x40      -> check the INA219 address pads, or"));
    Serial.println(F("                  set CURRENT_SENSOR_FITTED 0 to run"));
    Serial.println(F("                  without current measurement."));
  }
  Serial.println(F("------------------------------------------------"));

  delay(5000);
}
