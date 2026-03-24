# Smart Fan D1 Mini — ESP8266 variants

Wemos D1 Mini (ESP8266) kontrolieris ventilātoru vadībai ar MQTT, WiFi web UI, OTA atjauninājumiem, temperatūras sensoriem un automatizāciju.

Lētais un kompaktais variants bez displeja un enkodera.

## Iespējas

- **ESP8266 D1 Mini** — kompakts, lēts, 4MB flash
- **OTA atjauninājumi** — firmware + web failu (LittleFS) atjaunināšana no servera
- **Sensoru auto-detect** — OneWire (DS18B20), DHT22
- **2 ventilatori** — PWM ātruma vadība + relay barošanas atslēgšana
- **Tachometrs** — RPM mērīšana ar interrupt
- **Sensori** — DS18B20 (līdz 8 gab.), DHT22
- **MQTT** — publicē stāvokli, pieņem komandas, OTA trigeri, auto-prefikss no ChipID
- **Web UI** — konfigurācija, real-time dashboard, automatizācija (LittleFS)
- **4 automatizācijas režīmi** — Manuāli, Grafiks, Temperatūra, Serveris
- **SERVER režīms** — servera noteikumi prioritāte, heartbeat ar 2 min timeout
- **Min PWM kalibrācija** — konfigurējams minimālais PWM% katram fanam
- **WiFi scanner** — captive portal rāda pieejamo tīklu sarakstu
- **NTP** — laika sinhronizācija ar timezone atbalstu

## GPIO pinu karte (D1 Mini)

| Funkcija | D1 Mini pins | GPIO | Piezīmes |
|----------|-------------|------|----------|
| Fan1 PWM | **D1** | GPIO5 | MOSFET IRLML2502 |
| Fan2 PWM | **D2** | GPIO4 | MOSFET IRLML2502 |
| Fan1 TACH | **D5** | GPIO14 | 5.6kΩ pull-up + 100nF |
| Fan2 TACH | **D6** | GPIO12 | 5.6kΩ pull-up + 100nF |
| Relay1 (Fan1) | **D7** | GPIO13 | 12V barošana |
| Relay2 (Fan2) | **D8** | GPIO15 | 12V barošana (⚠️ boot pin) |
| DS18B20 | **D4** | GPIO2 | OneWire, 4.7kΩ pull-up (⚠️ boot pin) |
| DHT22 | **D3** | GPIO0 | 4.7kΩ pull-up (⚠️ boot pin) |

### ⚠️ Boot pinu piezīmes

| GPIO | Boot prasība | Risinājums |
|------|-------------|-----------|
| GPIO0 (D3) | HIGH bootā | DHT22 pull-up nodrošina HIGH |
| GPIO2 (D4) | HIGH bootā | DS18B20 pull-up nodrošina HIGH |
| GPIO15 (D8) | LOW bootā | Relay2 default OFF = LOW ✓ |

### Pinu diagramma

```
              D1 Mini
          ┌─────────────┐
     RST ─┤ RST     TX  ├─ (Serial)
      A0 ─┤ A0      RX  ├─ (Serial)
   (brīv)─┤ D0      D1  ├─ Fan1 PWM
   DHT22 ─┤ D3      D2  ├─ Fan2 PWM
 DS18B20 ─┤ D4      D5  ├─ Fan1 TACH
     GND ─┤ GND     D6  ├─ Fan2 TACH
  Relay1 ─┤ D7      D7  ├─ Relay1
  Relay2 ─┤ D8      3V3 ├─ (3.3V)
    3.3V ─┤ 3V3     GND ├─ GND
     5V  ─┤ 5V      5V  ├─ (5V in)
          └─────────────┘
```

## Prasības

- [PlatformIO](https://platformio.org/) (VS Code extension vai CLI)
- Wemos D1 Mini (ESP8266, 4MB flash)
- Micro-USB kabelis

## Pirmā palaišana

### 1. Klonēt

```bash
git clone -b esp8266-d1mini https://github.com/gun4as/smartfan-esp32.git smartfan-d1mini
cd smartfan-d1mini
```

### 2. Konfigurēt COM portu

`platformio.ini` — nomainīt uz savu COM portu:
```ini
upload_port = COM7
monitor_port = COM7
```

### 3. Kompilēt un augšuplādēt

```bash
# Firmware
pio run -e d1_mini -t upload

# Web UI faili (LittleFS)
pio run -e d1_mini -t uploadfs
```

### 4. Pieslēgties WiFi

D1 Mini startē **AP režīmā**:
- Tīkls: `SmartFan-XXXX` (XXXX = ChipID)
- Parole: `smartfan123`
- Adrese: `192.168.4.1`

Captive portal automātiski skenē WiFi tīklus — izvēlies savu tīklu, ievadi paroli.

### 5. Konfigurēt ierīci

Pārlūkā atver D1 Mini **IP adresi** → **Uzstādījumi**:

1. **Sensori** — atzīmēt pieslēgtos sensorus (DS18B20, DHT22)
2. **MQTT** — ieslēgt, ievadīt servera IP, portu, firmas lietotāju/paroli
3. **Min PWM** — kalibrēt minimālo PWM% katram fanam
4. **Saglabāt un restartēt**

## Atšķirības no ESP32 versijas

| Funkcija | ESP32 | D1 Mini |
|----------|-------|---------|
| Procesors | Dual-core 240MHz | Single-core 80MHz |
| RAM | 520KB | 80KB |
| Flash | 4MB | 4MB |
| WiFi | 802.11 b/g/n | 802.11 b/g/n |
| GPIO | 34 | 11 (izmantojami 8) |
| PWM | Hardware LEDC | Software analogWrite |
| Displejs | OLED SSD1306 | Nav (nav brīvu GPIO) |
| Enkoderis | KY-040 | Nav |
| I2C (AHT20) | Atsevišķi pini | ❌ Nav (pini kopīgi ar PWM) |
| OTA | HTTPS + TLS | HTTP (BearSSL) |
| Preferences | NVS (flash) | LittleFS JSON emulācija |
| Cena | ~5-8 EUR | ~2-3 EUR |

## Flash izkārtojums (4MB)

```
eagle.flash.4m2m.ld:
  Sketch:   ~1MB (firmware)
  OTA:      ~1MB (atjauninājums)
  LittleFS: ~2MB (web faili + preferences)
```

Pašreizējais firmware: **RAM 60.8%, Flash 52.1%** — daudz vietas turpmākam kodam.

## MQTT topiku struktūra

Prefikss tiek automātiski ģenerēts no ChipID: `sf_XXYYZZ`

### Publicē (D1 Mini → serveris)

| Topiks | Vērtība | Retain |
|--------|---------|--------|
| `{prefix}/status` | `online` / `offline` | Jā |
| `{prefix}/fan1/speed` | `0`..`100` (%) | Jā |
| `{prefix}/fan1/rpm` | `0`..`7000` | Jā |
| `{prefix}/fan1/relay` | `0` / `1` | Jā |
| `{prefix}/fan1/mode` | `MANUAL` / `SCHEDULE` / `TEMP` / `SERVER` | Jā |
| `{prefix}/fan2/...` | (tāpat kā fan1) | Jā |
| `{prefix}/sensors` | JSON (`{"dht_temp":22.5,...}`) | Jā |
| `{prefix}/fw_version` | `1.0.0` | Jā |
| `{prefix}/hw_variant` | `d1_mini` | Jā |

### Pieņem (serveris → D1 Mini)

| Topiks | Vērtība | Apraksts |
|--------|---------|----------|
| `{prefix}/fan1/set` | `0`..`100` | Iestatīt ātrumu |
| `{prefix}/fan2/set` | `0`..`100` | Iestatīt ātrumu |
| `{prefix}/relay1/set` | `0` / `1` | Ieslēgt/izslēgt relay |
| `{prefix}/relay2/set` | `0` / `1` | Ieslēgt/izslēgt relay |
| `{prefix}/ota/check` | (jebkas) | Pārbaudīt OTA atjauninājumu |
| `{prefix}/server/heartbeat` | `1` | Servera heartbeat (30s) |
| `{prefix}/server/fan1/set` | `0`..`100` | Servera fana ātrums |
| `{prefix}/server/fan2/set` | `0`..`100` | Servera fana ātrums |
| `{prefix}/timezone/set` | POSIX TZ | Iestatīt laika zonu |

## Bibliotēkas

| Bibliotēka | Lietojums |
|------------|-----------|
| ESPAsyncTCP | Async TCP savienojumi (ESP8266) |
| ESPAsyncWebServer | HTTP serveris + WebSocket |
| PubSubClient | MQTT klients |
| OneWire | DS18B20 komunikācija |
| DallasTemperature | DS18B20 API |
| DHT sensor library | DHT22 sensors |
| Adafruit Unified Sensor | Sensoru abstrakcija |
| ArduinoJson | Preferences emulācija (LittleFS JSON) |

## Projekta struktūra

```
smartfan-d1mini/
├── platformio.ini              # D1 Mini build konfigurācija
├── include/
│   ├── compat.h                # ESP8266/ESP32 saderības slānis
│   ├── config.h                # GPIO pini, konstantes
│   ├── wifi_manager.h          # WiFi AP/STA
│   ├── mqtt_client.h           # MQTT klients
│   ├── web_server.h            # AsyncWebServer + LittleFS
│   ├── device_manager.h        # Fanu, sensoru, releju vadība
│   ├── fan_control.h           # PWM + TACH
│   ├── automation.h            # Grafiks + temp režīms
│   ├── ntp_time.h              # NTP laika sinhronizācija
│   ├── ota_updater.h           # OTA firmware atjauninājumi
│   ├── display.h               # Tukšs stub (nav displeja)
│   └── encoder.h               # Tukšs stub (nav enkodera)
├── src/
│   ├── main.cpp                # Setup + loop
│   ├── wifi_manager.cpp        # AP/STA, captive portal
│   ├── mqtt_client.cpp         # MQTT connect, publish, subscribe
│   ├── web_server.cpp          # HTTP API + WiFi scanner
│   ├── device_manager.cpp      # Ierīču init + sensoru auto-detect
│   ├── fan_control.cpp         # analogWrite PWM + RPM mērīšana
│   ├── automation.cpp          # Grafiks + temperatūras automatizācija
│   ├── ntp_time.cpp            # NTP
│   └── ota_updater.cpp         # OTA check + download + flash
└── data/                       # LittleFS (web UI)
    ├── index.html              # Dashboard
    ├── config.html             # Uzstādījumi + OTA
    ├── auto.html               # Automatizācija
    ├── style.css               # CSS
    └── app.js                  # Frontend JS
```

## Saistītie projekti

- [smartfan-esp32 (master)](https://github.com/gun4as/smartfan-esp32) — ESP32 versija ar OLED displeju un enkoderi
- [smartfan-server](https://github.com/gun4as/smartfan_server) — Node.js serveris ar dashboard, multi-tenant, OTA, InfluxDB, Nord Pool integrācija
