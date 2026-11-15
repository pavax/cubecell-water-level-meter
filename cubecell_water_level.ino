#include "LoRaWan_APP.h"
#include "Arduino.h"
#include "credentials.h"
#include "softSerial.h"
#include "HT_SH1107Wire.h"

extern SH1107Wire display;

#define WAKE_UP_PIN USER_KEY
#define APP_TX_DUTY_MS (1000UL * 60 * 30)
#define CYCLE_WATCHDOG_MAX_MS (1000UL * 60 * 60 * 2)

// Two A0120AU ultrasonic sensors (UART auto, 9600 8N1), softSerial RX pins.
// Heltec softSerial uses static RX/TX state — read sensors one after another.
// Set to 0 to disable a sensor (payload status = missing, distance = 0).
#define SENSOR1_ENABLED 1
#define SENSOR2_ENABLED 0
#define SENSOR1_RX_PIN GPIO4
#define SENSOR2_RX_PIN GPIO3
#define SENSOR_TX_DUMMY GPIO1
#define SENSOR_UART_BAUD 9600
#define SENSOR_POWER_SETTLE_MS 150
#define SENSOR_SAMPLE_COUNT 7
#define SENSOR_SAMPLE_GAP_MS 150
#define SENSOR_SAMPLE_TIMEOUT_MS 800
#define SENSOR_READ_TIMEOUT_MS (SENSOR_SAMPLE_COUNT * (SENSOR_SAMPLE_TIMEOUT_MS + SENSOR_SAMPLE_GAP_MS))

// 2-wire leak probe: pin -> probe -> GND, external 470k–1M pull-up to Vext.
// Read only while Vext is on. LOW = wet, HIGH = dry.
#define LEAK_ENABLED 1
#define LEAK_PIN GPIO12
#define LEAK_SETTLE_MS 5

// DS18B20 (1-Wire): DQ -> TEMPERATURE_PIN, VDD -> Vext, GND -> GND, 4.7k DQ -> Vext.
// Skip ROM only (single sensor). 9-bit convert; margin over datasheet 93.75 ms.
// After Vext cold-start early readings are often POR (clone 25 °C / Maxim 85 °C):
// take 3 readings, keep the last. Reject 85.00 °C as unfinished convert.
#define TEMPERATURE_ENABLED 1
#define TEMPERATURE_PIN GPIO2
#define TEMPERATURE_CONVERT_WAIT_MS 200
#define TEMPERATURE_READ_COUNT 3
#define TEMPERATURE_RETRY_COUNT 3
#define TEMPERATURE_POR_MAXIM_CENTI 8500

#if !SENSOR1_ENABLED && !SENSOR2_ENABLED && !LEAK_ENABLED && !TEMPERATURE_ENABLED
#error "Enable at least one of SENSOR1_ENABLED / SENSOR2_ENABLED / LEAK_ENABLED / TEMPERATURE_ENABLED"
#endif

#define WATER_LEVEL_STATUS_OK 1
#define WATER_LEVEL_STATUS_MISSING 0
#define WATER_LEVEL_STATUS_TIMEOUT 9

#define LEAK_STATUS_DRY 0
#define LEAK_STATUS_WET 1
#define LEAK_STATUS_MISSING 0

#define TEMPERATURE_STATUS_OK 1
#define TEMPERATURE_STATUS_MISSING 0

#if TEMPERATURE_ENABLED
#define ONEWIRE_SEARCH 0
#define ONEWIRE_CRC16 0
#include "OneWire.h"
#endif

#define COLOR_MEASURE 0x005050  // cyan while measuring
#define DISPLAY_PAGE_MS 2000
// OLED needs Vext up before any display.init(); Heltec enableDisplay() inits immediately.
#define DISPLAY_VEXT_SETTLE_MS 100

uint16_t userChannelsMask[6] = { 0x00FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;
DeviceClass_t loraWanClass = LORAWAN_CLASS;
uint32_t appTxDutyCycle = APP_TX_DUTY_MS;
bool overTheAirActivation = LORAWAN_NETMODE;
bool loraWanAdr = LORAWAN_ADR;
bool keepNet = LORAWAN_NET_RESERVE;
bool isTxConfirmed = LORAWAN_UPLINKMODE;
uint8_t appPort = 2;
uint8_t confirmedNbTrials = 8;

static unsigned int batteryVoltage;
static unsigned int uptimeCount;
static volatile bool userKeyWakePending = false;

static TimerEvent_t cycleWatchdogTimer;
static char displayBuf[32];

#if TEMPERATURE_ENABLED
static OneWire temperatureWire(TEMPERATURE_PIN);
#endif

void enableDisplayAndRgb() {
  // Power OLED before Heltec enableDisplay()/displaySending() call display.init().
  // Init with Vext off can wedge I2C; then the whole UI cycle stays blank.
  digitalWrite(Vext, LOW);
  delay(DISPLAY_VEXT_SETTLE_MS);
  LoRaWAN.holdVext(true);
  LoRaWAN.enableDisplay();
  LoRaWAN.enableRgb();
}

void disableDisplayAndRgb() {
  LoRaWAN.disableDisplay();
  LoRaWAN.disableRgb();
}

void beginDisplayUpdate() {
  digitalWrite(Vext, LOW);
  delay(DISPLAY_VEXT_SETTLE_MS);
  display.init();
}

void showWelcomeScreen() {
  beginDisplayUpdate();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 8, F("Willkommen"));
  display.drawHorizontalLine(16, 30, 96);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 38, F("Water Level"));
  display.drawString(64, 52, F("CubeCell AB02"));
  display.display();
}

void drawCounterHeader() {
  char counterBuf[24];
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  sprintf(counterBuf, "Counter: %u", uptimeCount);
  display.drawString(64, 0, counterBuf);
  display.drawHorizontalLine(0, 14, 128);
}

void showMeasurePage(const char *title, const char *value, const char *unit) {
  display.init();
  display.clear();
  drawCounterHeader();
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 18, title);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 32, value);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 54, unit);
  display.display();
  delay(DISPLAY_PAGE_MS);
}

void showMeasuringScreen() {
  if (!LoRaWAN.isDisplayEnabled()) {
    return;
  }

  beginDisplayUpdate();
  display.clear();
  drawCounterHeader();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 28, F("Measuring..."));
  display.display();
}

void showMeasurePages(uint16_t distance1mm, uint8_t sensor1Status,
                      uint16_t distance2mm, uint8_t sensor2Status,
                      uint8_t leakStatus, int16_t temperatureCentiC, uint8_t temperatureStatus,
                      uint16_t batteryMv) {
  if (!LoRaWAN.isDisplayEnabled()) {
    return;
  }

  beginDisplayUpdate();

#if SENSOR1_ENABLED
  if (sensor1Status == WATER_LEVEL_STATUS_OK) {
    sprintf(displayBuf, "%u", distance1mm / 10);
    showMeasurePage("Sensor 1", displayBuf, "cm");
  } else {
    showMeasurePage("Sensor 1", "---", "cm");
  }
#endif

#if SENSOR2_ENABLED
  if (sensor2Status == WATER_LEVEL_STATUS_OK) {
    sprintf(displayBuf, "%u", distance2mm / 10);
    showMeasurePage("Sensor 2", displayBuf, "cm");
  } else {
    showMeasurePage("Sensor 2", "---", "cm");
  }
#endif

#if LEAK_ENABLED
  showMeasurePage("Leak", leakStatus == LEAK_STATUS_WET ? "WET" : "DRY", "");
#else
  (void)leakStatus;
#endif

#if TEMPERATURE_ENABLED
  if (temperatureStatus == TEMPERATURE_STATUS_OK) {
    const int16_t absCenti = (temperatureCentiC < 0) ? (int16_t)(-temperatureCentiC) : temperatureCentiC;
    sprintf(displayBuf, "%s%d.%02u",
            (temperatureCentiC < 0) ? "-" : "",
            absCenti / 100,
            (unsigned)(absCenti % 100));
    showMeasurePage("Temp", displayBuf, "C");
  } else {
    showMeasurePage("Temp", "---", "C");
  }
#else
  (void)temperatureCentiC;
  (void)temperatureStatus;
#endif

  sprintf(displayBuf, "%u.%02u", batteryMv / 1000, (batteryMv % 1000) / 10);
  showMeasurePage("Battery", displayBuf, "V");
}

void onCycleTimeoutWatchdog() {
  Serial.println(F("Watchdog: Cycle Timed Out"));
  delay(100);
  HW_Reset(0);
}

void resetCycleWatchdog() {
  Serial.println(F("Watchdog: reset"));
  TimerStop(&cycleWatchdogTimer);
  TimerSetValue(&cycleWatchdogTimer, CYCLE_WATCHDOG_MAX_MS);
  TimerStart(&cycleWatchdogTimer);
}

void putUint16(uint8_t offset, uint16_t value) {
  appData[offset] = highByte(value);
  appData[offset + 1] = lowByte(value);
}

void putInt16(uint8_t offset, int16_t value) {
  putUint16(offset, (uint16_t)value);
}

void powerSensorOn() {
  digitalWrite(Vext, LOW);
  delay(SENSOR_POWER_SETTLE_MS);
}

void powerSensorOff() {
  digitalWrite(Vext, HIGH);
  LoRaWAN.holdVext(false);
}

bool readA0120SampleMm(softSerial &ss, uint16_t &distanceMm) {
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
    const uint8_t expected = (uint8_t)((0xFF + dataH + dataL) & 0xFF);
    if (sum != expected) {
      continue;
    }

    distanceMm = ((uint16_t)dataH << 8) | dataL;
    return true;
  }

  return false;
}

uint16_t medianU16(uint16_t *values, uint8_t count) {
  for (uint8_t i = 1; i < count; i++) {
    uint16_t key = values[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  return values[count / 2];
}

// A0120AU: 0xFF, H, L, SUM; median of SENSOR_SAMPLE_COUNT samples (mm).
uint8_t readA0120Mm(uint8_t rxPin, uint16_t &distanceMm) {
  distanceMm = 0;

  softSerial ss(SENSOR_TX_DUMMY, rxPin);
  ss.begin(SENSOR_UART_BAUD);

  uint16_t samples[SENSOR_SAMPLE_COUNT];
  uint8_t sampleCount = 0;
  const unsigned long deadline = millis() + SENSOR_READ_TIMEOUT_MS;

  while (sampleCount < SENSOR_SAMPLE_COUNT && millis() < deadline) {
    uint16_t sampleMm = 0;
    if (readA0120SampleMm(ss, sampleMm)) {
      samples[sampleCount++] = sampleMm;
      delay(SENSOR_SAMPLE_GAP_MS);
    }
  }

  ss.end();

  if (sampleCount == 0) {
    return WATER_LEVEL_STATUS_TIMEOUT;
  }

  distanceMm = medianU16(samples, sampleCount);
  return WATER_LEVEL_STATUS_OK;
}

uint8_t readLeakStatus() {
  delay(LEAK_SETTLE_MS);
  return (digitalRead(LEAK_PIN) == LOW) ? LEAK_STATUS_WET : LEAK_STATUS_DRY;
}

// One Convert T + scratchpad read. Heltec OneWire (CubeCell). temperatureCentiC = 0.01 °C.
static bool convertAndReadTemperature(int16_t &temperatureCentiC) {
  uint8_t data[9];

  if (!temperatureWire.reset()) {
    return false;
  }
  temperatureWire.skip();
  temperatureWire.write(0x44); // Convert T (external power)
  delay(TEMPERATURE_CONVERT_WAIT_MS);

  if (!temperatureWire.reset()) {
    return false;
  }
  temperatureWire.skip();
  temperatureWire.write(0xBE); // Read Scratchpad
  for (uint8_t i = 0; i < 9; i++) {
    data[i] = temperatureWire.read();
  }

  if (OneWire::crc8(data, 8) != data[8]) {
    return false;
  }

  int16_t raw = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  raw &= ~7; // 9-bit unused LSBs
  temperatureCentiC = (int16_t)((raw * 100) / 16);

  // Maxim POR / unfinished convert after power-up.
  if (temperatureCentiC == TEMPERATURE_POR_MAXIM_CENTI) {
    return false;
  }

  return true;
}

static bool readTemperatureOnce(int16_t &temperatureCentiC) {
  for (uint8_t attempt = 0; attempt < TEMPERATURE_RETRY_COUNT; attempt++) {
    if (!temperatureWire.reset()) {
      delay(20);
      continue;
    }
    temperatureWire.skip();
    temperatureWire.write(0x4E); // Write Scratchpad
    temperatureWire.write(0x00); // TH
    temperatureWire.write(0x00); // TL
    temperatureWire.write(0x1F); // 9-bit

    if (convertAndReadTemperature(temperatureCentiC)) {
      return true;
    }
    delay(20);
  }
  return false;
}

// Skip ROM, 9-bit. Take TEMPERATURE_READ_COUNT readings after Vext on; keep the last.
uint8_t readTemperatureCentiC(int16_t &temperatureCentiC) {
  temperatureCentiC = 0;

  for (uint8_t i = 0; i < TEMPERATURE_READ_COUNT; i++) {
    int16_t sample = 0;
    if (!readTemperatureOnce(sample)) {
      return TEMPERATURE_STATUS_MISSING;
    }
    temperatureCentiC = sample;
  }
  return TEMPERATURE_STATUS_OK;
}

void prepareTxFrame(uint8_t port) {
  (void)port;

  resetCycleWatchdog();

  detachInterrupt(WAKE_UP_PIN);
  batteryVoltage = getBatteryVoltage();
  attachInterrupt(WAKE_UP_PIN, onUserKeyWake, RISING);

  Serial.printf("Measure uptime=%u battery=%u mV interval=%lu s display=%u\r\n",
                uptimeCount, batteryVoltage, (unsigned long)(appTxDutyCycle / 1000),
                LoRaWAN.isDisplayEnabled() ? 1 : 0);

  if (LoRaWAN.isRgbEnabled()) {
    turnOnRGB(COLOR_MEASURE, 0);
  }

  powerSensorOn();
  uint16_t distance1mm = 0;
  uint16_t distance2mm = 0;

#if SENSOR1_ENABLED
  uint8_t sensor1Status = readA0120Mm(SENSOR1_RX_PIN, distance1mm);
#else
  uint8_t sensor1Status = WATER_LEVEL_STATUS_MISSING;
#endif

#if SENSOR2_ENABLED
  uint8_t sensor2Status = readA0120Mm(SENSOR2_RX_PIN, distance2mm);
#else
  uint8_t sensor2Status = WATER_LEVEL_STATUS_MISSING;
#endif

#if LEAK_ENABLED
  uint8_t leakStatus = readLeakStatus();
#else
  uint8_t leakStatus = LEAK_STATUS_MISSING;
#endif

  int16_t temperatureCentiC = 0;
#if TEMPERATURE_ENABLED
  uint8_t temperatureStatus = readTemperatureCentiC(temperatureCentiC);
#else
  uint8_t temperatureStatus = TEMPERATURE_STATUS_MISSING;
#endif

  if (LoRaWAN.isRgbEnabled()) {
    turnOnRGB(0, 0);
  }

  if (LoRaWAN.isDisplayEnabled()) {
    showMeasurePages(distance1mm, sensor1Status, distance2mm, sensor2Status, leakStatus,
                     temperatureCentiC, temperatureStatus, batteryVoltage);
  }

  appDataSize = 14;
  putUint16(0, uptimeCount);
  putUint16(2, batteryVoltage);
  putUint16(4, distance1mm);
  putUint16(6, distance2mm);
  appData[8] = sensor1Status;
  appData[9] = sensor2Status;
  appData[10] = leakStatus;
  putInt16(11, temperatureCentiC);
  appData[13] = temperatureStatus;

  Serial.printf("TX #%u d1=%u mm s1=%u d2=%u mm s2=%u leak=%u temp=%d.%02u C st=%u bat=%u\r\n",
                uptimeCount, distance1mm, sensor1Status, distance2mm, sensor2Status, leakStatus,
                temperatureCentiC / 100,
                (unsigned)((temperatureCentiC < 0 ? -temperatureCentiC : temperatureCentiC) % 100),
                temperatureStatus, batteryVoltage);

  uptimeCount++;
}

void prepareBeforeSleep() {
  disableDisplayAndRgb();
  powerSensorOff();
  Serial.println(F("Sleep: Vext turned off"));
  delay(50);
}

void onUserKeyWake() {
  if (deviceState == DEVICE_STATE_SLEEP && digitalRead(WAKE_UP_PIN) == HIGH) {
    userKeyWakePending = true;
  }
}

void showBootColors() {
  turnOnRGB(COLOR_SEND, 500);
  turnOnRGB(COLOR_RXWINDOW2, 500);
  turnOnRGB(COLOR_RECEIVED, 500);
  turnOnRGB(COLOR_RXWINDOW1, 500);
  turnOnRGB(0, 0);
}

void setup() {
  Serial.begin(115200);
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(SENSOR_POWER_SETTLE_MS);

#if (AT_SUPPORT)
  enableAt();
#endif

  enableDisplayAndRgb();
  showWelcomeScreen();
  showBootColors();

  deviceState = DEVICE_STATE_INIT;
  LoRaWAN.ifskipjoin();

  userKeyWakePending = false;
  pinMode(WAKE_UP_PIN, INPUT_PULLUP);
  attachInterrupt(WAKE_UP_PIN, onUserKeyWake, RISING);
#if LEAK_ENABLED
  pinMode(LEAK_PIN, INPUT);
#endif

  TimerInit(&cycleWatchdogTimer, onCycleTimeoutWatchdog);
}

void loop() {
  switch (deviceState) {
    case DEVICE_STATE_INIT: {
#if (LORAWAN_DEVEUI_AUTO)
      LoRaWAN.generateDeveuiByChipID();
#endif
#if (AT_SUPPORT)
      getDevParam();
#endif
      printDevParam();
      LoRaWAN.init(loraWanClass, loraWanRegion);
      deviceState = DEVICE_STATE_JOIN;
      break;
    }
    case DEVICE_STATE_JOIN: {
      LoRaWAN.displayJoining();
      LoRaWAN.join();
      break;
    }
    case DEVICE_STATE_SEND: {
      if (LoRaWAN.isDisplayEnabled()) {
        LoRaWAN.displaySending();
        showMeasuringScreen();
      }
      prepareTxFrame(appPort);
      LoRaWAN.send();
      deviceState = DEVICE_STATE_CYCLE;
      break;
    }
    case DEVICE_STATE_CYCLE: {
      txDutyCycleTime = appTxDutyCycle + randr(0, APP_TX_DUTYCYCLE_RND);
      LoRaWAN.cycle(txDutyCycleTime);
      Serial.printf("Sleep %d s\r\n", (int)(txDutyCycleTime / 1000));
      prepareBeforeSleep();
      deviceState = DEVICE_STATE_SLEEP;
      break;
    }
    case DEVICE_STATE_SLEEP: {
      if (userKeyWakePending) {
        Serial.println(F("Wakeup: send now (UI on)"));
        enableDisplayAndRgb();
        LoRaWAN.txNextPacket();
        userKeyWakePending = false;
      } else {
        if (LoRaWAN.isDisplayEnabled()) {
          LoRaWAN.displayAck(false, false);
        }
        LoRaWAN.sleep();
      }
      break;
    }
    default: {
      deviceState = DEVICE_STATE_INIT;
      break;
    }
  }
}

void downLinkDataHandle(McpsIndication_t *mcpsIndication) {
  Serial.printf("+REV DATA: %s, RXSIZE: %d, PORT: %d\r\n",
                mcpsIndication->RxSlot ? "RXWIN2" : "RXWIN1",
                mcpsIndication->BufferSize, mcpsIndication->Port);
  Serial.print(F("+REV DATA: "));
  for (uint8_t i = 0; i < mcpsIndication->BufferSize; i++) {
    Serial.printf("%02X", mcpsIndication->Buffer[i]);
  }
  Serial.println();

  if (mcpsIndication->Port == 4 && mcpsIndication->BufferSize >= 2) {
    int newSleepTime = mcpsIndication->Buffer[1] | (mcpsIndication->Buffer[0] << 8);
    appTxDutyCycle = newSleepTime * 1000;
    saveDr();
    Serial.printf("Duty cycle %lu ms\r\n", (unsigned long)appTxDutyCycle);
  } else if (mcpsIndication->Port == 9) {
    Serial.println(F("Reset"));
    HW_Reset(0);
  }
}
