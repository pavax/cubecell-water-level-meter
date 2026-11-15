# CubeCell Wasserstand

Firmware für das Heltec CubeCell **HTCC-AB02** (EU868, OTAA, Port 2). Ultraschall (**A0120AU**), Leck-Sonde und **DS18B20**-Temperatur in einem kurzen Paket.

## Ein Zyklus

1. Aufwachen, Batteriespannung lesen.
2. Mess-LED setzen, Sensorversorgung (Vext) einschalten.
3. Sensor 1 und Sensor 2 nacheinander per softSerial, danach Leck-Sonde und DS18B20.
4. Bei UI-Zyklus (Boot oder User-Button): OLED/RGB an (JOINING / SENDING / Messwerte).
5. Paket senden, Display/RGB aus, Vext aus, Schlaf (Standardintervall **30 min**).

Timer-Wakeups laufen **ohne** Display/RGB. Der User-Button weckt den Knoten sofort und aktiviert Display und LED nur für diesen einen Zyklus.

Ein Soft-Watchdog wird zu Beginn jedes Messzyklus (`prepareTxFrame`) auf **2 Stunden** gesetzt. Läuft in der Zeit kein neuer Zyklus an, setzt die Firmware den Knoten zurück (`HW_Reset`).

## Sensoren

Zwei **A0120AU** (UART Auto, 9600 8N1). Heltec `softSerial` hat nur einen globalen Empfangspfad — die Sensoren werden nacheinander gelesen (`begin` → Frames → `end`). Pro Sensor: **7 Samples** (150 ms Abstand), davon der **Median** in mm.

Aktivierung per Define in `cubecell_water_level.ino` (mindestens ein Sensor):

```cpp
#define SENSOR1_ENABLED 0
#define SENSOR2_ENABLED 1
#define LEAK_ENABLED 1
#define TEMPERATURE_ENABLED 1
```

Deaktiviert (`0`): kein Auslesen, Distanz/Temp `0`, Status `0` (missing).

| Signal | Pin |
|---|---|
| Sensor 1 + 2 / DS18B20 VCC | `Vext` |
| Sensor 1 + 2 / DS18B20 GND | `GND` |
| Sensor 1 TX | `GPIO4` |
| Sensor 2 TX | `GPIO3` |
| SoftSerial TX (Dummy) | `GPIO1` |
| DS18B20 DQ | `GPIO2` |
| DS18B20 Pull-up 4,7 kΩ | `GPIO2` → `Vext` |

Sensor-RX bleibt unbeschaltet (Auto-/Processed-Modus). Gesendet wird die Rohdistanz in mm; Umrechnung in Füllhöhe folgt später.

Beide Ultraschallsensoren hängen gleichzeitig an Vext und können sich gegenseitig leicht stören. Falls die Werte unruhig sind, später Enable-FETs prüfen.

### Leck-Sonde

Zweiadrige Leitfähigkeits-Sonde mit externem Pull-up nach **Vext** (nicht VDD). Nur bei eingeschaltetem Vext lesen; im Sleep ist Vext aus → keine Elektrolyse.

```cpp
#define LEAK_ENABLED 1
#define LEAK_PIN GPIO12
```

| Signal | Pin |
|---|---|
| Sonde Sense | `GPIO12` |
| Sonde GND | `GND` |
| Pull-up 470 kΩ–1 MΩ | `GPIO12` → `Vext` |

### DS18B20

Ein Sensor am Bus (Heltec `OneWire`, Skip ROM, kein Search). 9‑Bit-Auflösung, Convert-Wartezeit ~200 ms. Nach Vext-Kaltstart dreimal messen und das dritte Reading nehmen (frühe Werte oft Clone-POR **25 °C**); Maxim-POR **85 °C** gilt als Fehler. Retries bei Bus/CRC-Fehler; sonst Status `missing`.

```cpp
#define TEMPERATURE_ENABLED 1
#define TEMPERATURE_PIN GPIO2
```

## Paket

14 Bytes, Port 2, big-endian.

| Bytes | Wert | bedeutet |
|---|---|---|
| 0–1 | Zähler | Sendungsnummer |
| 2–3 | Batterie, mV | z. B. 4234 → 4,234 V |
| 4–5 | Distanz Sensor 1, mm | Abstand Sensor → Oberfläche |
| 6–7 | Distanz Sensor 2, mm | Abstand Sensor → Oberfläche |
| 8 | Status Sensor 1 | `1` ok, `0` fehlt, `9` Timeout |
| 9 | Status Sensor 2 | `1` ok, `0` fehlt, `9` Timeout |
| 10 | Leck | `0` trocken, `1` nass |
| 11–12 | Temperatur, 0,01 °C | int16, z. B. 2450 → 24,50 °C |
| 13 | Status Temp | `1` ok, `0` fehlt |

Ein Beispiel-Decoder für The Things Stack / ChirpStack liegt unter [`examples/payload_decoder.js`](examples/payload_decoder.js) (akzeptiert auch Legacy 11/10 Bytes).

## Downlink

| Port | Inhalt | Beispiel (Hex) |
|---|---|---|
| 2 | Uplink, 14 Bytes | — |
| 4 | Intervall in Sekunden (uint16, big-endian), wird in EEPROM gespeichert und überlebt Power-Cycle | siehe unten |
| 9 | Soft-Reset (`HW_Reset`) | `01` |

Port-4-Beispiele (big-endian):

| Hex | Sekunden | Intervall |
|---|---|---|
| `0384` | 900 | 15 min |
| `0708` | 1800 | 30 min |
| `0E10` | 3600 | 1 h |

Port 4 braucht mindestens zwei Bytes; `0000` wird ignoriert. Port 9 wertet nur den Port aus; ein beliebiges Byte reicht. Ohne gespeicherten Wert: Standardintervall 30 Minuten.

## Build

Board: **CubeCell-Board Plus (HTCC-AB02)**, Region EU868, OTAA, DevEUI aus ChipID.

```powershell
.\scripts\cubecell.ps1 -Action build
.\scripts\cubecell.ps1 -Action flash
```

`credentials_example.h` nach `credentials.h` kopieren und `appEui` / `appKey` setzen. Optional Port in `cubecell.local.json`: `{"port":"COM5"}`.

Der Build nutzt dieselbe CubeCell-Core-Installation wie `cubecell_air_quality` (`arduino-cli-cubecell`).
