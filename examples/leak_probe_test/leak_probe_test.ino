#include "Arduino.h"
#include "HT_SH1107Wire.h"

// 2-wire leak probe:
//   Draht A -> LEAK_PIN
//   Draht B -> GND
//
// Metall-Kurzschluss: ~0 Ω → interner Pull-up (~50 kΩ) reicht.
// Wasser: oft 100 kΩ–1 MΩ → Pin bleibt bei INPUT_PULLUP auf HIGH.
// Deshalb: Lade-/Entlade-Messung (hi-Z), optional externer schwacher Pull-up.
//
// Externer Pull-up: 470k–1M von LEAK_PIN nach Vext (nicht VDD).
// Nur messen wenn Vext an ist (LOW = on). Im Sleep Vext aus → keine Elektrolyse.

#define LEAK_PIN GPIO12

#define SENSE_INTERNAL_PULL 0
#define SENSE_DISCHARGE 1
#define SENSE_EXTERNAL_PULL 2

// SENSE_DISCHARGE: kein Extra-Bauteil. SENSE_EXTERNAL_PULL: 470k–1M nach Vext.
#define SENSE_MODE SENSE_EXTERNAL_PULL
#define VEXT_SETTLE_MS 20

// Unter diesem Entlade-Zeitwert (µs) gilt als nass. Serial zeigt den Rohwert zum Kalibrieren.
#define WET_DISCHARGE_US 8000
#define DISCHARGE_TIMEOUT_US 50000
#define CHARGE_US 100
#define LOOP_MS 400

SH1107Wire display(0x3c, 500000, SDA, SCL, GEOMETRY_128_64, GPIO10);

char buffer[32];

bool readInternalPullup() {
  pinMode(LEAK_PIN, INPUT_PULLUP);
  delay(5);
  const bool wet = (digitalRead(LEAK_PIN) == LOW);
  pinMode(LEAK_PIN, INPUT);
  return wet;
}

bool readExternalPullup() {
  // Externer 470k–1M nach Vext; kein interner Pull-up.
  digitalWrite(Vext, LOW);
  delay(VEXT_SETTLE_MS);
  pinMode(LEAK_PIN, INPUT);
  delay(5);
  return digitalRead(LEAK_PIN) == LOW;
}

// Pin auf HIGH laden, dann hi-Z: Wasser entlädt gegen GND → schneller LOW.
uint16_t measureDischargeUs() {
  pinMode(LEAK_PIN, OUTPUT);
  digitalWrite(LEAK_PIN, HIGH);
  delayMicroseconds(CHARGE_US);

  pinMode(LEAK_PIN, INPUT);
  const unsigned long t0 = micros();
  while (digitalRead(LEAK_PIN) == HIGH) {
    if ((micros() - t0) >= DISCHARGE_TIMEOUT_US) {
      return DISCHARGE_TIMEOUT_US;
    }
  }
  return (uint16_t)(micros() - t0);
}

bool readDischargeWet(uint16_t &dischargeUs) {
  dischargeUs = measureDischargeUs();
  pinMode(LEAK_PIN, INPUT);
  return dischargeUs < WET_DISCHARGE_US;
}

void showReading(bool wet, uint16_t dischargeUs, bool pullWet, uint16_t batteryMv) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, F("Leak probe"));
  display.drawHorizontalLine(0, 14, 128);

  display.setFont(ArialMT_Plain_24);
  display.drawString(64, 16, wet ? F("WET") : F("DRY"));

  display.setFont(ArialMT_Plain_10);
#if SENSE_MODE == SENSE_DISCHARGE
  sprintf(buffer, "t=%u us  pup=%s", dischargeUs, pullWet ? "W" : "D");
#else
  sprintf(buffer, "GPIO%u  pup=%s", (unsigned)LEAK_PIN, pullWet ? "W" : "D");
#endif
  display.drawString(64, 44, buffer);
  sprintf(buffer, "bat %u mV", batteryMv);
  display.drawString(64, 54, buffer);
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(150);

  pinMode(LEAK_PIN, INPUT);

  display.init();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, F("Leak test"));
  display.display();

  Serial.println(F("Leak probe test"));
  Serial.printf("  pin=GPIO%u  other wire=GND\r\n", (unsigned)LEAK_PIN);
#if SENSE_MODE == SENSE_DISCHARGE
  Serial.printf("  mode=discharge  wet if t<%u us\r\n", (unsigned)WET_DISCHARGE_US);
#elif SENSE_MODE == SENSE_EXTERNAL_PULL
  Serial.println(F("  mode=external pull-up 470k-1M to Vext"));
#else
  Serial.println(F("  mode=internal pull-up (often too strong for water)"));
#endif
  Serial.println(F("  tip: tap water; distilled is almost insulating"));
}

void loop() {
  const bool pullWet = readInternalPullup();
  uint16_t dischargeUs = 0;
  bool wet = false;

#if SENSE_MODE == SENSE_DISCHARGE
  wet = readDischargeWet(dischargeUs);
#elif SENSE_MODE == SENSE_EXTERNAL_PULL
  wet = readExternalPullup();
#else
  wet = pullWet;
#endif

  const uint16_t batteryMv = getBatteryVoltage();

  Serial.printf(
      "leak=%s  discharge=%u us  internal_pull=%s  bat=%u mV\r\n",
      wet ? "WET" : "DRY",
      dischargeUs,
      pullWet ? "WET" : "DRY",
      batteryMv);

  showReading(wet, dischargeUs, pullWet, batteryMv);
  delay(LOOP_MS);
}
