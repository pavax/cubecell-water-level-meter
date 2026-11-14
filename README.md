# CubeCell Wasserstand

Firmware für das Heltec CubeCell **HTCC-AB02** (EU868, OTAA, Port 2). Zwei Ultraschallsensoren **A0120AU** messen die Distanz und senden beide Werte in einem kurzen Paket.

## Ein Zyklus

1. Aufwachen, Batteriespannung lesen.
2. Mess-LED setzen, Sensorversorgung (Vext) einschalten.
3. Sensor 1 und Sensor 2 nacheinander per softSerial auslesen, danach Leck-Sonde.
4. Bei UI-Zyklus (Boot oder User-Button): OLED/RGB an (JOINING / SENDING / Messwerte).
5. Paket senden, Display/RGB aus, Vext aus, Schlaf (Standardintervall **30 min**).

Timer-Wakeups laufen **ohne** Display/RGB. Der User-Button weckt den Knoten sofort und aktiviert Display und LED nur für diesen einen Zyklus.

Ein Soft-Watchdog wird zu Beginn jedes Messzyklus (`prepareTxFrame`) auf **2 Stunden** gesetzt. Läuft in der Zeit kein neuer Zyklus an, setzt die Firmware den Knoten zurück (`HW_Reset`).

## Sensoren

Zwei **A0120AU** (UART Auto, 9600 8N1). Heltec `softSerial` hat nur einen globalen Empfangspfad — die Sensoren werden nacheinander gelesen (`begin` → Frames → `end`). Pro Sensor: **7 Samples** (150 ms Abstand), davon der **Median** in mm.

Aktivierung per Define in `cubecell_water_level.ino` (mindestens einer muss an sein):

```cpp
#define SENSOR1_ENABLED 1
#define SENSOR2_ENABLED 1
```

Deaktiviert (`0`): kein Auslesen, Distanz `0`, Status `0` (missing), Display zeigt `S1 off` / `S2 off`.

| Signal | Pin |
|---|---|
| Sensor 1 + 2 VCC | `Vext` |
| Sensor 1 + 2 GND | `GND` |
| Sensor 1 TX | `GPIO2` |
| Sensor 2 TX | `GPIO3` |
| SoftSerial TX (Dummy) | `GPIO1` |

Sensor-RX bleibt unbeschaltet (Auto-/Processed-Modus). Gesendet wird die Rohdistanz in mm; Umrechnung in Füllhöhe folgt später.

Beide Sensoren hängen gleichzeitig an Vext und können sich gegenseitig leicht stören. Falls die Werte unruhig sind, später Enable-FETs prüfen.

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

## Paket

11 Bytes, Port 2, big-endian.

| Bytes | Wert | bedeutet |
|---|---|---|
| 0–1 | Zähler | Sendungsnummer |
| 2–3 | Batterie, mV | z. B. 4234 → 4,234 V |
| 4–5 | Distanz Sensor 1, mm | Abstand Sensor → Oberfläche |
| 6–7 | Distanz Sensor 2, mm | Abstand Sensor → Oberfläche |
| 8 | Status Sensor 1 | `1` ok, `0` fehlt, `9` Timeout |
| 9 | Status Sensor 2 | `1` ok, `0` fehlt, `9` Timeout |
| 10 | Leck | `0` trocken, `1` nass |

Ein Beispiel-Decoder für The Things Stack / ChirpStack liegt unter [`examples/payload_decoder.js`](examples/payload_decoder.js).

## Downlink

| Port | Inhalt | Beispiel (Hex) |
|---|---|---|
| 2 | Uplink, 11 Bytes | — |
| 4 | Intervall in Sekunden (uint16, big-endian), gilt bis zum nächsten Reset bzw. bis erneut gesetzt | `0384` = 900 s |
| 9 | Soft-Reset (`HW_Reset`) | `01` |

Port 4 braucht mindestens zwei Bytes. Port 9 wertet nur den Port aus; ein beliebiges Byte reicht. Standardintervall nach Boot: 30 Minuten.

## Build

Board: **CubeCell-Board Plus (HTCC-AB02)**, Region EU868, OTAA, DevEUI aus ChipID.

```powershell
.\scripts\cubecell.ps1 -Action build
.\scripts\cubecell.ps1 -Action flash
```

`credentials_example.h` nach `credentials.h` kopieren und `appEui` / `appKey` setzen. Optional Port in `cubecell.local.json`: `{"port":"COM5"}`.

Der Build nutzt dieselbe CubeCell-Core-Installation wie `cubecell_air_quality` (`arduino-cli-cubecell`).
