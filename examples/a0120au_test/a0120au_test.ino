#include "Arduino.h"
#include "softSerial.h"
#include "HT_SH1107Wire.h"

// Sensor TX -> CubeCell softSerial RX
#define SENSOR_RX_PIN GPIO3
#define SENSOR_TX_DUMMY GPIO5
#define SENSOR_UART_BAUD 9600
#define SENSOR_POWER_SETTLE_MS 150
#define SENSOR_SAMPLE_TIMEOUT_MS 800

SH1107Wire display(0x3c, 500000, SDA, SCL, GEOMETRY_128_64, GPIO10);

char buffer[32];

void showReading(uint16_t distanceMm, bool ok, uint16_t batteryMv) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, F("A0120AU"));
  display.drawHorizontalLine(0, 14, 128);

  if (ok) {
    display.setFont(ArialMT_Plain_24);
    sprintf(buffer, "%u", distanceMm);
    display.drawString(64, 18, buffer);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 44, F("mm"));
  } else {
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 24, F("timeout"));
  }

  display.setFont(ArialMT_Plain_10);
  sprintf(buffer, "bat %u mV", batteryMv);
  display.drawString(64, 54, buffer);
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(SENSOR_POWER_SETTLE_MS);

  display.init();
  //display.screenRotate(ANGLE_180_DEGREE);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, F("Sensor test"));
  display.display();

  Serial.printf("A0120AU test — RX on GPIO%u, Vext on\r\n", (unsigned)SENSOR_RX_PIN);
}

bool readSample(softSerial &ss, uint16_t &distanceMm) {
  distanceMm = 0;
  ss.flush();

  const unsigned long deadline = millis() + SENSOR_SAMPLE_TIMEOUT_MS;
  while (millis() < deadline) {
    if (ss.available() < 4) {
      delay(1);
      continue;
    }

    if (ss.read() != 0xFF) {
      continue;
    }

    unsigned long frameDeadline = millis() + 50;
    while (ss.available() < 3 && millis() < frameDeadline) {
      delay(1);
    }
    if (ss.available() < 3) {
      continue;
    }

    const uint8_t dataH = (uint8_t)ss.read();
    const uint8_t dataL = (uint8_t)ss.read();
    const uint8_t sum = (uint8_t)ss.read();
    if (((0xFF + dataH + dataL) & 0xFF) != sum) {
      continue;
    }

    distanceMm = ((uint16_t)dataH << 8) | dataL;
    return true;
  }

  return false;
}

void loop() {
  softSerial ss(SENSOR_TX_DUMMY, SENSOR_RX_PIN);
  ss.begin(SENSOR_UART_BAUD);

  uint16_t distanceMm = 0;
  const bool ok = readSample(ss, distanceMm);
  ss.end();

  const uint16_t batteryMv = getBatteryVoltage();

  if (ok) {
    Serial.printf("distance=%u mm battery=%u mV\r\n", distanceMm, batteryMv);
  } else {
    Serial.printf("timeout / no valid frame battery=%u mV\r\n", batteryMv);
  }
  showReading(distanceMm, ok, batteryMv);

  delay(500);
}
