#pragma once

// ═══════════════════════════════════════════════════════════
//  D1 Mini (ESP8266) — GPIO pinu karte
// ═══════════════════════════════════════════════════════════

// ── PWM Izejas ──────────────────────────────────────────────
#define PIN_FAN1_PWM    D1    // GPIO5 — Fan1 PWM (MOSFET)
#define PIN_FAN2_PWM    D2    // GPIO4 — Fan2 PWM (MOSFET)

// ── TACH Ieejas ────────────────────────────────────────────
#define PIN_FAN1_TACH   D5    // GPIO14 — 5.6kΩ pull-up + 100nF
#define PIN_FAN2_TACH   D6    // GPIO12 — 5.6kΩ pull-up + 100nF

// ── Releji ─────────────────────────────────────────────────
#define PIN_RELAY1      D7    // GPIO13 — Fan1 12V barošana
#define PIN_RELAY2      D8    // GPIO15 — Fan2 12V (⚠️ boot: jābūt LOW)

// ── Sensori ────────────────────────────────────────────────
#define PIN_DS18B20     D4    // GPIO2 — OneWire, 4.7kΩ pull-up (⚠️ boot: HIGH)
#define PIN_DHT22       D3    // GPIO0 — 4.7kΩ pull-up (⚠️ boot: HIGH)

// ── PWM konfigurācija ──────────────────────────────────────
#define PWM_FREQ        25000   // 25kHz – Intel 4-pin fan standarts
#define PWM_RES         8       // 8-bit = 0..255

// ── TACH debounce ──────────────────────────────────────────
#define TACH_DEBOUNCE_US  1000
#define RPM_MAX           7000

// (Nav displeja un enkodera — D1 Mini variants)

// ── WiFi AP (hotspot) ──────────────────────────────────────
#define AP_PASSWORD     "smartfan123"
#define AP_CHANNEL      1
#define AP_MAX_CONN     2

// ── NTP / Laika zona ──────────────────────────────────────
#define NTP_SERVER      "pool.ntp.org"
#define TZ_INFO_DEFAULT "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define NVS_KEY_TZ      "tz_info"

// ── DS18B20 multi-sensor ─────────────────────────────────────
#define DS18B20_MAX_SENSORS  8

// ── NVS ────────────────────────────────────────────────────
#define NVS_NAMESPACE   "smartfan"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_CONFIG  "dev_config"
// DS18B20 nosaukumi: ds_a0..ds_a7 (ROM addr blob), ds_n0..ds_n7 (name string)
// Fan ātrumi: fan_spd0, fan_spd1 (uint8_t 0-100)

// ── MQTT konfigurācija ──────────────────────────────────────
#define MQTT_PUBLISH_INTERVAL  10000   // publicēt reizi 10s
#define MQTT_RECONNECT_INTERVAL 5000   // mēģināt pieslēgties reizi 5s
#define MQTT_MAX_TOPIC_LEN     64
#define MQTT_DEFAULT_PORT      1883

// ── Web serveris ───────────────────────────────────────────
#define WEB_PORT        80

// ── Debug izvade ────────────────────────────────────────────
extern bool dbgEnabled;
#define DBG(fmt, ...) do { if (dbgEnabled) Serial.printf(fmt, ##__VA_ARGS__); } while(0)
#define DBGLN(msg)    do { if (dbgEnabled) Serial.println(msg); } while(0)

// ── Ierīču tipi (bitmask) ──────────────────────────────────
#define DEV_FAN1_4PIN   (1 << 0)   // 0x01
#define DEV_FAN2_4PIN   (1 << 1)   // 0x02
#define DEV_RELAY1      (1 << 2)   // 0x04 — Fan1 barošana
#define DEV_RELAY2      (1 << 3)   // 0x08 — Fan2 barošana
#define DEV_DS18B20     (1 << 4)   // 0x10
#define DEV_DHT22       (1 << 5)   // 0x20
// D1 Mini: nav AHT20 (I2C kopīgs ar PWM), nav OLED, nav enkodera
#define DEV_MQTT        (1 << 6)   // 0x40
