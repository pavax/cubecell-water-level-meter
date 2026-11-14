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
#define SENSOR1_ENABLED 0
#define SENSOR2_ENABLED 1
#define SENSOR1_RX_PIN GPIO2
#define SENSOR2_RX_PIN GPIO3
#define SENSOR_TX_DUMMY GPIO1
#define SENSOR_UART_BAUD 9600
#define SENSOR_POWER_SETTLE_MS 150
#define SENSOR_SAMPLE_COUNT 7
#define SENSOR_SAMPLE_GAP_MS 150
#define SENSOR_SAMPLE_TIMEOUT_MS 800
#define SENSOR_READ_TIMEOUT_MS (SENSOR_SAMPLE_COUNT * (SENSOR_SAMPLE_TIMEOUT_MS + SENSOR_SAMPLE_GAP_MS))

#if !SENSOR1_ENABLED && !SENSOR2_ENABLED
#error "Enable at least one of SENSOR1_ENABLED / SENSOR2_ENABLED"
#endif

#define WATER_LEVEL_STATUS_OK 1
#define WATER_LEVEL_STATUS_MISSING 0
#define WATER_LEVEL_STATUS_TIMEOUT 9

// 0xRRGGBB. Cyan while measuring, independent of the core's colors.
#define COLOR_MEASURE 0x005050
#define DISPLAY_PAGE_MS 3500

/*LoraWan channelsmask, default channels 0-7*/
uint16_t userChannelsMask[6] = { 0x00FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };

/*LoraWan region, select in arduino IDE tools*/
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;

/*LoraWan Class, Class A and Class C are supported*/
DeviceClass_t loraWanClass = LORAWAN_CLASS;

/*the application data transmission duty cycle. value in [ms].*/
uint32_t appTxDutyCycle = APP_TX_DUTY_MS;

/*OTAA or ABP*/
bool overTheAirActivation = LORAWAN_NETMODE;

/*ADR enable*/
bool loraWanAdr = LORAWAN_ADR;

/* set LORAWAN_Net_Reserve ON, the node could save the network info to flash, when node reset not need to join again */
bool keepNet = LORAWAN_NET_RESERVE;

/* Indicates if the node is sending confirmed or unconfirmed messages */
bool isTxConfirmed = LORAWAN_UPLINKMODE;

/* Application port */
uint8_t appPort = 2;

/* Number of confirmed uplink retries. The MAC lowers the datarate after repeated misses. */
uint8_t confirmedNbTrials = 8;

static unsigned int batteryVoltage;
static unsigned int uptimeCount;
static volatile bool accelWoke = false;

static TimerEvent_t cycleWatchdogTimer;
static char displayBuf[32];

void enableDisplayAndRgb() {
  LoRaWAN.enableDisplay();
  LoRaWAN.enableRgb();
}

void disableDisplayAndRgb() {
  LoRaWAN.disableDisplay();
  LoRaWAN.disableRgb();
}


void beginDisplayUpdate() {
  digitalWrite(Vext, LOW);
  delay(10);
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
  delay(3000);
}

void drawCounterHeader() {
  char counterBuf[24];
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  sprintf(counterBuf, "Counter: %u", uptimeCount);
  display.drawString(64, 0, counterBuf);
  display.drawHorizontalLine(0, 14, 128);
}

void showMeasurementPage(const char *title, const char *value, const char *unit) {
  display.init();
  display.clear();
  drawCounterHeader();
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 18, title);
  display.setFont(ArialMT_Plain_24);
  display.drawString(64, 30, value);
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

void showMeasurePages(uint16_t distance1Mm, uint8_t status1,
                      uint16_t distance2Mm, uint8_t status2,
                      uint16_t batteryMv) {
  if (!LoRaWAN.isDisplayEnabled()) {
    return;
  }

  beginDisplayUpdate();

#if SENSOR1_ENABLED
  if (status1 == WATER_LEVEL_STATUS_OK) {
    sprintf(displayBuf, "%u", distance1Mm / 10);
    showMeasurementPage("Sensor 1", displayBuf, "cm");
  } else {
    showMeasurementPage("Sensor 1", "---", "cm");
  }
#endif

#if SENSOR2_ENABLED
  if (status2 == WATER_LEVEL_STATUS_OK) {
    sprintf(displayBuf, "%u", distance2Mm / 10);
    showMeasurementPage("Sensor 2", displayBuf, "cm");
  } else {
    showMeasurementPage("Sensor 2", "---", "cm");
  }
#endif

  sprintf(displayBuf, "%u.%02u", batteryMv / 1000, (batteryMv % 1000) / 10);
  showMeasurementPage("Battery", displayBuf, "V");
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

void powerSensorOn() {
  digitalWrite(Vext, LOW);
  delay(SENSOR_POWER_SETTLE_MS);
}

void powerSensorOff() {
  digitalWrite(Vext, HIGH);
  LoRaWAN.holdVext(false);
}

// One valid A0120AU frame from an already-open softSerial. Returns true on success.
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
  // Insertion sort; count is small (SENSOR_SAMPLE_COUNT).
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

// A0120AU UART frame: 0xFF, DATA_H, DATA_L, SUM=(0xFF+H+L)&0xFF; distance in mm.
// Collects SENSOR_SAMPLE_COUNT samples and returns their median.
// softSerial must be opened/closed per call (shared static state).
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

void prepareTxFrame(uint8_t port) {
  (void)port;

  resetCycleWatchdog();

  detachInterrupt(WAKE_UP_PIN);
  batteryVoltage = getBatteryVoltage();
  attachInterrupt(WAKE_UP_PIN, onWakeUp, RISING);

  Serial.printf("Measure uptime=%u battery=%u mV interval=%lu s display=%u\r\n",
                uptimeCount, batteryVoltage, (unsigned long)(appTxDutyCycle / 1000),
                LoRaWAN.isDisplayEnabled() ? 1 : 0);

  if (LoRaWAN.isRgbEnabled()) {
    turnOnRGB(COLOR_MEASURE, 0);
  }

  powerSensorOn();
  uint16_t distance1Mm = 0;
  uint16_t distance2Mm = 0;
#if SENSOR1_ENABLED
  uint8_t status1 = readA0120Mm(SENSOR1_RX_PIN, distance1Mm);
#else
  uint8_t status1 = WATER_LEVEL_STATUS_MISSING;
#endif
#if SENSOR2_ENABLED
  uint8_t status2 = readA0120Mm(SENSOR2_RX_PIN, distance2Mm);
#else
  uint8_t status2 = WATER_LEVEL_STATUS_MISSING;
#endif

  if (LoRaWAN.isDisplayEnabled()) {
    showMeasurePages(distance1Mm, status1, distance2Mm, status2, batteryVoltage);
  }
  // Vext stays on until prepareBeforeSleep.

  // 10 bytes, Port 2, big-endian.
  appDataSize = 10;
  putUint16(0, uptimeCount);
  putUint16(2, batteryVoltage);
  putUint16(4, distance1Mm);
  putUint16(6, distance2Mm);
  appData[8] = status1;
  appData[9] = status2;

  Serial.printf("TX #%u d1=%u mm s1=%u d2=%u mm s2=%u bat=%u\r\n",
                uptimeCount, distance1Mm, status1, distance2Mm, status2, batteryVoltage);

  uptimeCount++;
}

void prepareBeforeSleep() {
  if (LoRaWAN.isDisplayEnabled()) {
    disableDisplayAndRgb();
  }
  powerSensorOff();
  Serial.println(F("Sleep: Vext turned off"));
  delay(50);
}

void onWakeUp() {
  if (deviceState == DEVICE_STATE_SLEEP && digitalRead(WAKE_UP_PIN) == HIGH) {
    accelWoke = true;
  }
}

void showBootColors() {
  turnOnRGB(COLOR_SEND, 400);
  turnOnRGB(COLOR_RXWINDOW2, 400);
  turnOnRGB(COLOR_RECEIVED, 400);
  turnOnRGB(COLOR_RXWINDOW1, 400);
  turnOffRGB();
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
  LoRaWAN.displayMcuInit();
  showWelcomeScreen();
  showBootColors();

  deviceState = DEVICE_STATE_INIT;
  LoRaWAN.ifskipjoin();

  accelWoke = false;
  pinMode(WAKE_UP_PIN, INPUT_PULLUP);
  attachInterrupt(WAKE_UP_PIN, onWakeUp, RISING);

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
      // water_meter.ino: always displayJoining before join()
      LoRaWAN.displayJoining();
      LoRaWAN.join();
      break;
    }
    case DEVICE_STATE_SEND: {
      // water_meter.ino: displaySending() before prepare — re-arms isDisplayOn/Vext.
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
      if (accelWoke) {
        Serial.println(F("Wakeup: send now (UI on)"));
        enableDisplayAndRgb();
        LoRaWAN.txNextPacket();
        accelWoke = false;
      } else {
        // water_meter.ino: displayAck in sleep path (no-op if no ACK / display off)
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
