#include "cubecell_utils.h"

#include <Arduino.h>
#include <EEPROM.h>

#define SLEEP_EEPROM_SIZE 8
#define SLEEP_EEPROM_MAGIC 0xA5C3u
#define SLEEP_EEPROM_ADDR 0

struct SleepEeprom {
  uint16_t magic;
  uint16_t sleepSeconds;
};

bool loadPersistedSleepTime(uint32_t &dutyCycleMs) {
  EEPROM.begin(SLEEP_EEPROM_SIZE);
  SleepEeprom stored = {};
  EEPROM.get(SLEEP_EEPROM_ADDR, stored);
  if (stored.magic != SLEEP_EEPROM_MAGIC || stored.sleepSeconds == 0) {
    return false;
  }

  dutyCycleMs = (uint32_t)stored.sleepSeconds * 1000UL;
  Serial.printf("Restored duty cycle %lu ms (%u s)\r\n",
                (unsigned long)dutyCycleMs, stored.sleepSeconds);
  return true;
}

void persistSleepTime(uint16_t sleepSeconds) {
  if (sleepSeconds == 0) {
    return;
  }

  EEPROM.begin(SLEEP_EEPROM_SIZE);
  SleepEeprom stored = { SLEEP_EEPROM_MAGIC, sleepSeconds };
  EEPROM.put(SLEEP_EEPROM_ADDR, stored);
  if (EEPROM.commit()) {
    Serial.printf("Saved duty cycle %u s\r\n", sleepSeconds);
  } else {
    Serial.println(F("EEPROM: duty cycle save failed"));
  }
}
