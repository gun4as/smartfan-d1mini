#include "device_manager.h"
#include "config.h"
#include "fan_control.h"
#include "ntp_time.h"
#include "compat.h"

// ── Sensoru bibliotēkas ─────────────────────────────────────
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

// ── Iekšējie mainīgie ──────────────────────────────────────
static uint16_t enabledDevices = 0;
static Preferences prefs;

// ── DS18B20 multi-sensor ────────────────────────────────────
static OneWire*            oneWire   = nullptr;
static DallasTemperature*  dallas    = nullptr;
static DS18B20Sensor       ds18Sensors[DS18B20_MAX_SENSORS];
static uint8_t             ds18Count = 0;
static uint32_t lastDS18Read = 0;

// ── DHT22 ──────────────────────────────────────────────────
static DHT* dht = nullptr;
static float dhtTemp = -127.0;
static float dhtHum  = -1.0;
static uint32_t lastDHTRead = 0;

// ── Sensoru nosaukumi ──────────────────────────────────────
static char dhtName[16] = "DHT22";

// ── Ierīces nosaukums ──────────────────────────────────────
static char deviceName[24] = "Smart Fan";

// ── Releji (2 gab — katram fanam savs) ─────────────────────
static bool relayState[2] = { false, false };
static const uint8_t relayPins[2] = { PIN_RELAY1, PIN_RELAY2 };

// ── DS18B20 NVS nosaukumu palīgfunkcijas ────────────────────

static void loadDS18Names() {
    prefs.begin(NVS_NAMESPACE, true);  // read-only
    for (uint8_t i = 0; i < ds18Count; i++) {
        // Mēģinām atrast šo ROM adresi saglabātajās
        bool found = false;
        uint8_t storedCount = prefs.getUChar("ds_cnt", 0);
        for (uint8_t s = 0; s < storedCount && s < DS18B20_MAX_SENSORS; s++) {
            char keyA[8];
            snprintf(keyA, sizeof(keyA), "ds_a%d", s);
            uint8_t storedAddr[8];
            size_t len = prefs.getBytes(keyA, storedAddr, 8);
            if (len == 8 && memcmp(storedAddr, ds18Sensors[i].addr, 8) == 0) {
                // Atrasts — ielādējam nosaukumu
                char keyN[8];
                snprintf(keyN, sizeof(keyN), "ds_n%d", s);
                String name = prefs.getString(keyN, "");
                if (name.length() > 0) {
                    strncpy(ds18Sensors[i].name, name.c_str(), 15);
                    ds18Sensors[i].name[15] = '\0';
                }
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(ds18Sensors[i].name, sizeof(ds18Sensors[i].name), "Sensors %d", i + 1);
        }
    }
    prefs.end();
}

static void saveDS18Names() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("ds_cnt", ds18Count);
    for (uint8_t i = 0; i < ds18Count; i++) {
        char keyA[8], keyN[8];
        snprintf(keyA, sizeof(keyA), "ds_a%d", i);
        snprintf(keyN, sizeof(keyN), "ds_n%d", i);
        prefs.putBytes(keyA, ds18Sensors[i].addr, 8);
        prefs.putString(keyN, ds18Sensors[i].name);
    }
    prefs.end();
}

// ═══════════════════════════════════════════════════════════
//  PUBLISKĀS FUNKCIJAS
// ═══════════════════════════════════════════════════════════

// ── Auto-detect sensori ──────────────────────────────────
static uint16_t autoDetectSensors() {
    uint16_t detected = 0;

    // I2C removed — D1 Mini I2C pins (D1/D2) shared with Fan PWM

    // OneWire scan — DS18B20
    OneWire ow(PIN_DS18B20);
    uint8_t addr[8];
    if (ow.search(addr)) {
        detected |= DEV_DS18B20;
        DBG("[AUTO] DS18B20 atrasts uz GPIO%d\n", PIN_DS18B20);
    }
    ow.reset_search();

    // DHT22 — mēģinām nolasīt (ja atbild — ir pieslēgts)
    DHT testDht(PIN_DHT22, DHT22);
    testDht.begin();
    delay(250);  // DHT vajag laiku
    float t = testDht.readTemperature();
    if (!isnan(t)) {
        detected |= DEV_DHT22;
        DBG("[AUTO] DHT22 atrasts uz GPIO%d (temp=%.1f)\n", PIN_DHT22, t);
    }

    return detected;
}

void devInit() {
    // ── 1. Ielādē config no NVS ─────────────────────────────
    prefs.begin(NVS_NAMESPACE, false);
    enabledDevices = prefs.getUShort(NVS_KEY_CONFIG, 0xFFFF);
    prefs.end();

    if (enabledDevices == 0xFFFF) {
        // Pirmais boots — auto-detect visu
        DBGLN("[DEV] Pirmais boots — auto-detect...");
        uint16_t detected = autoDetectSensors();
        // Fani vienmēr ieslēgti (izejas ir uz plates)
        enabledDevices = DEV_FAN1_4PIN | DEV_FAN2_4PIN | DEV_MQTT | detected;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putUShort(NVS_KEY_CONFIG, enabledDevices);
        prefs.end();
        DBG("[DEV] Auto-detect rezultāts: 0x%04X\n", enabledDevices);
    }

    // Releji automātiski seko faniem
    if (enabledDevices & DEV_FAN1_4PIN) enabledDevices |= DEV_RELAY1;
    else                                enabledDevices &= ~DEV_RELAY1;
    if (enabledDevices & DEV_FAN2_4PIN) enabledDevices |= DEV_RELAY2;
    else                                enabledDevices &= ~DEV_RELAY2;

    DBG("[DEV] Config bitmask: 0x%04X\n", enabledDevices);

    // ── 2. Safe init — neaktīvie pini ───────────────────────
    // 4-pin MOSFET: HIGH = MOSFET ON = PWM LOW = fans stop
    if (!(enabledDevices & DEV_FAN1_4PIN)) {
        pinMode(PIN_FAN1_PWM, OUTPUT); digitalWrite(PIN_FAN1_PWM, HIGH);
        DBG("[DEV] Fan1 GPIO%d = HIGH (disabled)\n", PIN_FAN1_PWM);
    }
    if (!(enabledDevices & DEV_FAN2_4PIN)) {
        pinMode(PIN_FAN2_PWM, OUTPUT); digitalWrite(PIN_FAN2_PWM, HIGH);
        DBG("[DEV] Fan2 GPIO%d = HIGH (disabled)\n", PIN_FAN2_PWM);
    }

    // Releji — vienmēr safe OFF sākumā
    pinMode(PIN_RELAY1, OUTPUT); digitalWrite(PIN_RELAY1, LOW);
    pinMode(PIN_RELAY2, OUTPUT); digitalWrite(PIN_RELAY2, LOW);
    relayState[0] = false;
    relayState[1] = false;

    // ── 3. Aktīvie fani ─────────────────────────────────────
    if (enabledDevices & DEV_FAN1_4PIN) {
        fanInit(0, FAN_4PIN);  // fanInit ieslēgs releju automātiski
    }
    if (enabledDevices & DEV_FAN2_4PIN) {
        fanInit(1, FAN_4PIN);
    }

    // ── Releju info ─────────────────────────────────────────
    if (enabledDevices & DEV_RELAY1) {
        DBG("[DEV] Relay1: GPIO%d (Fan1 barošana)\n", PIN_RELAY1);
    }
    if (enabledDevices & DEV_RELAY2) {
        DBG("[DEV] Relay2: GPIO%d (Fan2 barošana)\n", PIN_RELAY2);
    }

    // ── DS18B20 (multi-sensor) ──────────────────────────────
    if (enabledDevices & DEV_DS18B20) {
        oneWire = new OneWire(PIN_DS18B20);
        dallas  = new DallasTemperature(oneWire);
        dallas->begin();
        ds18Count = devDS18B20Scan();
        DBG("[DEV] DS18B20: GPIO%d, %d sensors\n", PIN_DS18B20, ds18Count);
    }

    // ── DHT22 ───────────────────────────────────────────────
    if (enabledDevices & DEV_DHT22) {
        dht = new DHT(PIN_DHT22, DHT22);
        dht->begin();
        DBG("[DEV] DHT22: GPIO%d\n", PIN_DHT22);
    }

    // ── Nosaukumi no NVS ───────────────────────────────────────
    {
        Preferences p;
        p.begin(NVS_NAMESPACE, true);
        String dn = p.getString("dht_name", "DHT22");
        String devN = p.getString("dev_name", "Smart Fan");
        p.end();
        strncpy(dhtName, dn.c_str(), 15); dhtName[15] = '\0';
        strncpy(deviceName, devN.c_str(), 23); deviceName[23] = '\0';
    }
}

void devLoop() {
    uint32_t now = millis();

    fanLoop();

    // DS18B20 — nolasa visus sensorus
    if ((enabledDevices & DEV_DS18B20) && dallas && (now - lastDS18Read > 2000)) {
        dallas->requestTemperatures();
        for (uint8_t i = 0; i < ds18Count; i++) {
            float t = dallas->getTempC(ds18Sensors[i].addr);
            ds18Sensors[i].valid = (t > -100.0);
            if (ds18Sensors[i].valid) ds18Sensors[i].temp = t;
        }
        lastDS18Read = now;
    }

    if ((enabledDevices & DEV_DHT22) && dht && (now - lastDHTRead > 2500)) {
        float t = dht->readTemperature();
        float h = dht->readHumidity();
        if (!isnan(t)) dhtTemp = t;
        if (!isnan(h)) dhtHum  = h;
        lastDHTRead = now;
    }
}

uint16_t devGetConfig() {
    return enabledDevices;
}

void devSetConfig(uint16_t mask) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUShort(NVS_KEY_CONFIG, mask);
    prefs.end();
    DBG("[DEV] Jauna config: 0x%04X — restartējam\n", mask);
    delay(500);
    ESP.restart();
}

bool devIsEnabled(uint16_t flag) {
    return (enabledDevices & flag) != 0;
}

// ── Status JSON ────────────────────────────────────────────
String devGetStatusJSON() {
    String j = "{";
    j += "\"name\":\"" + String(deviceName) + "\"";
    j += ",\"config\":" + String(enabledDevices);

    // Fani (2 gab)
    j += ",\"fans\":[";
    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (i > 0) j += ",";
        j += "{\"active\":" + String(fanIsActive(i) ? "true" : "false");
        j += ",\"rpm\":" + String(fanGetRPM(i));
        j += ",\"speed\":" + String(fanGetSpeed(i));
        j += ",\"relay\":" + String(relayState[i] ? "true" : "false") + "}";
    }
    j += "]";

    // DS18B20 — masīvs
    if (enabledDevices & DEV_DS18B20) {
        j += ",\"ds18b20\":[";
        for (uint8_t i = 0; i < ds18Count; i++) {
            if (i > 0) j += ",";
            j += "{\"addr\":\"" + devDS18B20AddrStr(ds18Sensors[i].addr) + "\"";
            j += ",\"name\":\"" + String(ds18Sensors[i].name) + "\"";
            j += ",\"temp\":" + String(ds18Sensors[i].temp, 1) + "}";
        }
        j += "]";
    }
    if (enabledDevices & DEV_DHT22) {
        j += ",\"dht22\":{\"name\":\"" + String(dhtName) + "\"";
        j += ",\"temp\":" + String(dhtTemp, 1);
        j += ",\"hum\":" + String(dhtHum, 1) + "}";
    }
    // Laiks
    j += ",\"time\":\"" + ntpGetTime() + "\"";
    j += ",\"date\":\"" + ntpGetDate() + "\"";

    j += "}";
    return j;
}

// ── Releju vadība ──────────────────────────────────────────
void relaySet(uint8_t id, bool on) {
    if (id > 1) return;
    // Pārbaudīt vai relejs ir ieslēgts konfigurācijā
    uint16_t flag = (id == 0) ? DEV_RELAY1 : DEV_RELAY2;
    if (!(enabledDevices & flag)) return;

    relayState[id] = on;
    digitalWrite(relayPins[id], on ? HIGH : LOW);
    DBG("[DEV] Relay%d: %s (GPIO%d)\n", id + 1, on ? "ON" : "OFF", relayPins[id]);
}

bool relayGet(uint8_t id) {
    if (id > 1) return false;
    return relayState[id];
}

// Fan ID → Relay ID mapping (Fan0 → Relay0, Fan1 → Relay1)
void relaySetForFan(uint8_t fanId, bool on) {
    if (fanId > 1) return;
    relaySet(fanId, on);
}

// ── DS18B20 multi-sensor API ──────────────────────────────
uint8_t devDS18B20Count() {
    return ds18Count;
}

const DS18B20Sensor* devDS18B20Get(uint8_t idx) {
    if (idx >= ds18Count) return nullptr;
    return &ds18Sensors[idx];
}

uint8_t devDS18B20Scan() {
    if (!dallas) return 0;

    dallas->begin();
    uint8_t count = dallas->getDeviceCount();
    if (count > DS18B20_MAX_SENSORS) count = DS18B20_MAX_SENSORS;

    for (uint8_t i = 0; i < count; i++) {
        dallas->getAddress(ds18Sensors[i].addr, i);
        ds18Sensors[i].temp  = -127.0;
        ds18Sensors[i].valid = false;
        snprintf(ds18Sensors[i].name, sizeof(ds18Sensors[i].name), "Sensors %d", i + 1);
    }
    ds18Count = count;

    // Ielādēt nosaukumus no NVS (salīdzina pēc ROM addr)
    loadDS18Names();

    return count;
}

void devDS18B20SetName(uint8_t idx, const char* name) {
    if (idx >= ds18Count) return;
    strncpy(ds18Sensors[idx].name, name, 15);
    ds18Sensors[idx].name[15] = '\0';
    saveDS18Names();
    DBG("[DEV] DS18B20[%d] name: %s\n", idx, ds18Sensors[idx].name);
}

String devDS18B20AddrStr(const uint8_t addr[8]) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3],
             addr[4], addr[5], addr[6], addr[7]);
    return String(buf);
}

// ── Sensoru getteri (display modulim) ─────────────────────
float devGetDHTTemp()     { return dhtTemp; }
float devGetDHTHum()      { return dhtHum; }

// ── Sensoru nosaukumi ─────────────────────────────────────
const char* devGetDHTName() { return dhtName; }

void devSetDHTName(const char* name) {
    strncpy(dhtName, name, 15); dhtName[15] = '\0';
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("dht_name", dhtName);
    p.end();
    DBG("[DEV] DHT22 name: %s\n", dhtName);
}

// ── Ierīces nosaukums ─────────────────────────────────────
const char* devGetName() { return deviceName; }

void devSetName(const char* name) {
    strncpy(deviceName, name, 23); deviceName[23] = '\0';
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("dev_name", deviceName);
    p.end();
    DBG("[DEV] Device name: %s\n", deviceName);
}
