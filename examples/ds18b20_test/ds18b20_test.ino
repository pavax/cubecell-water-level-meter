#include "Arduino.h"
#include "HT_SH1107Wire.h"

// Same Heltec OneWire path as cubecell_water_level.ino.
#define TEMP_PIN GPIO2
#define TEMP_CONVERT_WAIT_MS 100
#define VEXT_SETTLE_MS 500
#define LOOP_MS 500

#define TEMP_STATUS_OK 1
#define TEMP_STATUS_MISSING 0

#define ONEWIRE_SEARCH 0
#define ONEWIRE_CRC16 0
#include "OneWire.h"

OneWire tempWire(TEMP_PIN);
SH1107Wire display(0x3c, 500000, SDA, SCL, GEOMETRY_128_64, GPIO10);

char buffer[32];

uint8_t readDs18b20CentiC(int16_t &tempCentiC) {
  uint8_t data[9];
  tempCentiC = 0;

  if (!tempWire.reset()) {
    return TEMP_STATUS_MISSING;
  }
  tempWire.skip();
  tempWire.write(0x4E);
  tempWire.write(0x00);
  tempWire.write(0x00);
  tempWire.write(0x1F);

  if (!tempWire.reset()) {
    return TEMP_STATUS_MISSING;
  }
  tempWire.skip();
  tempWire.write(0x44);
  delay(TEMP_CONVERT_WAIT_MS);

  if (!tempWire.reset()) {
    return TEMP_STATUS_MISSING;
  }
  tempWire.skip();
  tempWire.write(0xBE);
  for (uint8_t i = 0; i < 9; i++) {
    data[i] = tempWire.read();
  }

  if (OneWire::crc8(data, 8) != data[8]) {
    return TEMP_STATUS_MISSING;
  }

  int16_t raw = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  raw &= ~7;
  tempCentiC = (int16_t)((raw * 100) / 16);
  return TEMP_STATUS_OK;
}

void showReading(uint8_t status, int16_t tempCentiC, uint16_t batteryMv) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, F("DS18B20 OneWire"));
  display.drawHorizontalLine(0, 14, 128);

  if (status == TEMP_STATUS_OK) {
    const int16_t absCenti = (tempCentiC < 0) ? (int16_t)(-tempCentiC) : tempCentiC;
    display.setFont(ArialMT_Plain_16);
    sprintf(buffer, "%s%d.%02u",
            (tempCentiC < 0) ? "-" : "",
            absCenti / 100,
            (unsigned)(absCenti % 100));
    display.drawString(64, 18, buffer);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 44, F("C"));
  } else {
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 24, F("no sensor"));
  }

  display.setFont(ArialMT_Plain_10);
  sprintf(buffer, "GPIO2  bat %u mV", batteryMv);
  display.drawString(64, 54, buffer);
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(VEXT_SETTLE_MS);

  display.init();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, F("DS18B20 OneWire"));
  display.display();

  Serial.println(F("DS18B20 Heltec OneWire test — Skip ROM, 9-bit"));
}

void loop() {
  const unsigned long t0 = millis();
  int16_t tempCentiC = 0;
  const uint8_t status = readDs18b20CentiC(tempCentiC);
  const unsigned long elapsed = millis() - t0;
  const uint16_t batteryMv = getBatteryVoltage();

  if (status == TEMP_STATUS_OK) {
    const int16_t absCenti = (tempCentiC < 0) ? (int16_t)(-tempCentiC) : tempCentiC;
    Serial.printf("temp=%d.%02u C  took=%lu ms  battery=%u mV\r\n",
                  tempCentiC / 100,
                  (unsigned)(absCenti % 100),
                  elapsed, batteryMv);
  } else {
    Serial.printf("no sensor  took=%lu ms  battery=%u mV\r\n", elapsed, batteryMv);
  }

  showReading(status, tempCentiC, batteryMv);
  delay(LOOP_MS);
}
