#include "Arduino.h"
#include "HT_SH1107Wire.h"

// Same Heltec OneWire path as cubecell_water_level.ino.
// Cycles Vext each loop to reproduce cold-start POR (clone 25 °C / Maxim 85 °C).
#define TEMPERATURE_PIN GPIO2
#define TEMPERATURE_CONVERT_WAIT_MS 200
#define TEMPERATURE_READ_COUNT 3
#define TEMPERATURE_RETRY_COUNT 3
#define TEMPERATURE_POR_MAXIM_CENTI 8500
#define VEXT_SETTLE_MS 150
#define VEXT_OFF_MS 300
#define LOOP_MS 1000

#define TEMPERATURE_STATUS_OK 1
#define TEMPERATURE_STATUS_MISSING 0

#define ONEWIRE_SEARCH 0
#define ONEWIRE_CRC16 0
#include "OneWire.h"

OneWire temperatureWire(TEMPERATURE_PIN);
SH1107Wire display(0x3c, 500000, SDA, SCL, GEOMETRY_128_64, GPIO10);

char buffer[32];
uint16_t loopCount = 0;
uint16_t okCount = 0;
uint16_t failCount = 0;

static void powerSensorOn() {
  digitalWrite(Vext, LOW);
  delay(VEXT_SETTLE_MS);
}

static void powerSensorOff() {
  digitalWrite(Vext, HIGH);
  delay(VEXT_OFF_MS);
}

static bool convertAndReadTemperature(int16_t &temperatureCentiC) {
  uint8_t data[9];

  if (!temperatureWire.reset()) {
    return false;
  }
  temperatureWire.skip();
  temperatureWire.write(0x44);
  delay(TEMPERATURE_CONVERT_WAIT_MS);

  if (!temperatureWire.reset()) {
    return false;
  }
  temperatureWire.skip();
  temperatureWire.write(0xBE);
  for (uint8_t i = 0; i < 9; i++) {
    data[i] = temperatureWire.read();
  }

  if (OneWire::crc8(data, 8) != data[8]) {
    return false;
  }

  int16_t raw = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  raw &= ~7;
  temperatureCentiC = (int16_t)((raw * 100) / 16);
  return temperatureCentiC != TEMPERATURE_POR_MAXIM_CENTI;
}

static bool readTemperatureOnce(int16_t &temperatureCentiC) {
  for (uint8_t attempt = 0; attempt < TEMPERATURE_RETRY_COUNT; attempt++) {
    if (!temperatureWire.reset()) {
      delay(20);
      continue;
    }
    temperatureWire.skip();
    temperatureWire.write(0x4E);
    temperatureWire.write(0x00);
    temperatureWire.write(0x00);
    temperatureWire.write(0x1F);

    if (convertAndReadTemperature(temperatureCentiC)) {
      return true;
    }
    delay(20);
  }
  return false;
}

// Take TEMPERATURE_READ_COUNT readings after Vext on; keep the last.
uint8_t readTemperatureCentiC(int16_t &temperatureCentiC, int16_t samples[TEMPERATURE_READ_COUNT]) {
  temperatureCentiC = 0;

  for (uint8_t i = 0; i < TEMPERATURE_READ_COUNT; i++) {
    samples[i] = 0;
    if (!readTemperatureOnce(samples[i])) {
      return TEMPERATURE_STATUS_MISSING;
    }
    temperatureCentiC = samples[i];
  }
  return TEMPERATURE_STATUS_OK;
}

static void formatCenti(char *out, int16_t temperatureCentiC) {
  const int16_t absCenti = (temperatureCentiC < 0) ? (int16_t)(-temperatureCentiC) : temperatureCentiC;
  sprintf(out, "%s%d.%02u",
          (temperatureCentiC < 0) ? "-" : "",
          absCenti / 100,
          (unsigned)(absCenti % 100));
}

void showReading(uint8_t status, int16_t temperatureCentiC, uint16_t batteryMv) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, F("DS18B20 3rd read"));
  display.drawHorizontalLine(0, 14, 128);

  if (status == TEMPERATURE_STATUS_OK) {
    display.setFont(ArialMT_Plain_16);
    formatCenti(buffer, temperatureCentiC);
    strcat(buffer, " C");
    display.drawString(64, 18, buffer);
  } else {
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 18, F("no sensor"));
  }

  display.setFont(ArialMT_Plain_10);
  sprintf(buffer, "ok %u  fail %u", okCount, failCount);
  display.drawString(64, 40, buffer);
  sprintf(buffer, "GPIO2  %u mV", batteryMv);
  display.drawString(64, 52, buffer);
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(Vext, OUTPUT);
  powerSensorOn();

  display.init();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, F("DS18B20 test"));
  display.display();

  Serial.println(F("DS18B20 test: Vext cold-start, 3 readings, keep 3rd"));
}

void loop() {
  loopCount++;

  powerSensorOff();
  powerSensorOn();

  const unsigned long t0 = millis();
  int16_t temperatureCentiC = 0;
  int16_t samples[TEMPERATURE_READ_COUNT];
  const uint8_t status = readTemperatureCentiC(temperatureCentiC, samples);
  const unsigned long elapsed = millis() - t0;
  const uint16_t batteryMv = getBatteryVoltage();

  if (status == TEMPERATURE_STATUS_OK) {
    okCount++;
    char s0[12], s1[12], s2[12], kept[12];
    formatCenti(s0, samples[0]);
    formatCenti(s1, samples[1]);
    formatCenti(s2, samples[2]);
    formatCenti(kept, temperatureCentiC);
    Serial.printf("#%u OK temp=%s C  samples=%s,%s,%s  took=%lu ms bat=%u\r\n",
                  loopCount, kept, s0, s1, s2, elapsed, batteryMv);
  } else {
    failCount++;
    Serial.printf("#%u FAIL took=%lu ms bat=%u\r\n", loopCount, elapsed, batteryMv);
  }

  showReading(status, temperatureCentiC, batteryMv);
  delay(LOOP_MS);
}
