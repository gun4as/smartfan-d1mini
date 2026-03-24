#include "mqtt_client.h"
#include "config.h"
#include "device_manager.h"
#include "fan_control.h"
#include "automation.h"
#include "ntp_time.h"
#include "ota_updater.h"
#include "compat.h"
#include <PubSubClient.h>
// Preferences via compat.h

// ── Iekšējie mainīgie ──────────────────────────────────────
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static MqttConfig   mqttCfg;
static uint32_t     lastPublish   = 0;
static uint32_t     lastReconnect = 0;

// Karogi priekš atliktās publicēšanas (nevar publicēt no callback)
static volatile bool pendingPublish[FAN_COUNT] = {false, false};

// ── Publicēt automatizācijas konfigurāciju ───────────────
static void mqttPublishAutoConfig(uint8_t fanId) {
    if (!mqtt.connected()) return;
    String prefix = String(mqttCfg.prefix);
    String f = prefix + "/fan" + String(fanId + 1);

    // Grafiks — 7 dienas, katra kā CSV
    for (uint8_t day = 0; day < 7; day++) {
        String csv;
        for (uint8_t s = 0; s < SLOTS_PER_DAY; s++) {
            if (s > 0) csv += ',';
            csv += String(autoGetSlot(fanId, day, s));
        }
        mqtt.publish((f + "/schedule/day/" + String(day)).c_str(), csv.c_str());
    }

    // Temperatūras līkne — JSON
    TempCurve& tc = autoGetTempCurve(fanId);
    char json[128];
    snprintf(json, sizeof(json),
        "{\"sensor\":%d,\"idx\":%d,\"tmin\":%.1f,\"tmax\":%.1f,\"smin\":%d,\"smax\":%d}",
        tc.sensorType, tc.sensorIdx, tc.tempMin, tc.tempMax, tc.speedMin, tc.speedMax);
    mqtt.publish((f + "/temp-curve").c_str(), json);

    DBG("[MQTT] Fan%d config published\n", fanId + 1);
}

// ── Apstrādāt grafika dienu (CSV payload) ───────────────
static void handleScheduleDay(uint8_t fanId, uint8_t day, const char* csv) {
    uint8_t slots[SLOTS_PER_DAY];
    memset(slots, 0, sizeof(slots));

    // Parsēt CSV: "0,50,50,0,..."
    int idx = 0;
    const char* p = csv;
    while (*p && idx < SLOTS_PER_DAY) {
        slots[idx++] = (uint8_t)constrain(atoi(p), 0, 100);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }

    for (uint8_t s = 0; s < SLOTS_PER_DAY; s++) {
        autoSetSlot(fanId, day, s, slots[s]);
    }
    autoSaveSchedule(fanId);
    DBG("[MQTT] Fan%d schedule day %d set\n", fanId + 1, day);
}

// ── Apstrādāt temperatūras līkni (JSON payload) ─────────
static void handleTempCurve(uint8_t fanId, const char* json) {
    TempCurve tc = autoGetTempCurve(fanId);

    // Vienkāršs JSON parsēšana (bez bibliotēkas)
    const char* p;
    if ((p = strstr(json, "\"sensor\":")))  tc.sensorType = atoi(p + 9);
    if ((p = strstr(json, "\"idx\":")))     tc.sensorIdx  = atoi(p + 6);
    if ((p = strstr(json, "\"tmin\":")))    tc.tempMin    = atof(p + 7);
    if ((p = strstr(json, "\"tmax\":")))    tc.tempMax    = atof(p + 7);
    if ((p = strstr(json, "\"smin\":")))    tc.speedMin   = atoi(p + 7);
    if ((p = strstr(json, "\"smax\":")))    tc.speedMax   = atoi(p + 7);

    autoSetTempCurve(fanId, tc);
    DBG("[MQTT] Fan%d temp curve set: %.1f-%.1f C, %d-%d%%\n",
        fanId + 1, tc.tempMin, tc.tempMax, tc.speedMin, tc.speedMax);
}

// ── MQTT callback — saņem komandas ────────────────────────
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Ignorēt tukšas ziņas (notīrīti retained topiki)
    if (length == 0) return;

    // Lielāks buferis automatizācijas datiem
    char msg[256];
    unsigned int copyLen = (length < 255) ? length : 255;
    memcpy(msg, payload, copyLen);
    msg[copyLen] = '\0';

    String t = String(topic);
    int val = atoi(msg);

    String prefix = String(mqttCfg.prefix);

    // ── Fan ātrums ──
    if (t == prefix + "/fan1/set") {
        fanSetSpeed(0, constrain(val, 0, 100));
        pendingPublish[0] = true;
        DBG("[MQTT] Fan1 set %d%%\n", val);
    } else if (t == prefix + "/fan2/set") {
        fanSetSpeed(1, constrain(val, 0, 100));
        pendingPublish[1] = true;
        DBG("[MQTT] Fan2 set %d%%\n", val);
    }
    // ── Releji ── (tāpat kā web_server.cpp handleRelay)
    else if (t == prefix + "/relay1/set") {
        bool newState = (val > 0);
        if (relayGet(0) != newState) {  // ignorēt ja jau sakrīt (pašu ziņa)
            relaySet(0, newState);
            fanSetManualOff(0, !newState);
            pendingPublish[0] = true;
            DBG("[MQTT] Relay1 set %s\n", newState ? "ON" : "OFF");
        }
    } else if (t == prefix + "/relay2/set") {
        bool newState = (val > 0);
        if (relayGet(1) != newState) {  // ignorēt ja jau sakrīt (pašu ziņa)
            relaySet(1, newState);
            fanSetManualOff(1, !newState);
            pendingPublish[1] = true;
            DBG("[MQTT] Relay2 set %s\n", newState ? "ON" : "OFF");
        }
    }
    // ── Režīms ──
    else if (t == prefix + "/fan1/mode/set") {
        if (val >= 0 && val <= 3) {
            autoSetMode(0, (AutoMode)val);
            pendingPublish[0] = true;
            DBG("[MQTT] Fan1 mode -> %s\n", autoModeStr((AutoMode)val));
        }
    } else if (t == prefix + "/fan2/mode/set") {
        if (val >= 0 && val <= 3) {
            autoSetMode(1, (AutoMode)val);
            pendingPublish[1] = true;
            DBG("[MQTT] Fan2 mode -> %s\n", autoModeStr((AutoMode)val));
        }
    }
    // ── Grafika diena ── {prefix}/fan{1|2}/schedule/day/{0-6}
    else if (t.startsWith(prefix + "/fan1/schedule/day/")) {
        int day = t.substring(t.lastIndexOf('/') + 1).toInt();
        if (day >= 0 && day < 7) handleScheduleDay(0, day, msg);
    } else if (t.startsWith(prefix + "/fan2/schedule/day/")) {
        int day = t.substring(t.lastIndexOf('/') + 1).toInt();
        if (day >= 0 && day < 7) handleScheduleDay(1, day, msg);
    }
    // ── Temperatūras līkne ── {prefix}/fan{1|2}/temp-curve/set
    else if (t == prefix + "/fan1/temp-curve/set") {
        handleTempCurve(0, msg);
    } else if (t == prefix + "/fan2/temp-curve/set") {
        handleTempCurve(1, msg);
    }
    // ── Config pieprasījums ── {prefix}/fan{1|2}/config/get
    else if (t == prefix + "/fan1/config/get") {
        mqttPublishAutoConfig(0);
    } else if (t == prefix + "/fan2/config/get") {
        mqttPublishAutoConfig(1);
    }

    // ── Server heartbeat ── {prefix}/server/heartbeat
    else if (t == prefix + "/server/heartbeat") {
        autoServerHeartbeat();
        DBG("[MQTT] Server heartbeat received\n");
    }
    // ── Server fan speed ── {prefix}/server/fan{1|2}/set
    else if (t == prefix + "/server/fan1/set") {
        autoServerSetSpeed(0, constrain(val, 0, 100));
        pendingPublish[0] = true;
    }
    else if (t == prefix + "/server/fan2/set") {
        autoServerSetSpeed(1, constrain(val, 0, 100));
        pendingPublish[1] = true;
    }

    // ── Server timezone ── {prefix}/server/timezone
    else if (t == prefix + "/server/timezone") {
        { char tmp[length + 1]; memcpy(tmp, payload, length); tmp[length] = '\0'; ntpSetTimezone(String(tmp)); }
        DBG("[MQTT] Timezone: %.*s\n", length, payload);
    }

    // ── OTA komanda ── {prefix}/ota/check
    else if (t == prefix + "/ota/check") {
        DBGLN("[MQTT] OTA check pieprasīts");
        otaCheckNow();
    }
    // ── OTA server URL ── {prefix}/ota/server/set
    else if (t == prefix + "/ota/server/set") {
        extern void otaSetServer(const String& url);
        otaSetServer(String(msg));
    }

    // Manuāla ātruma maiņa caur MQTT → pārslēdz uz MANUAL
    if (t == prefix + "/fan1/set" || t == prefix + "/fan2/set") {
        uint8_t fid = (t == prefix + "/fan1/set") ? 0 : 1;
        if (autoGetMode(fid) != AUTO_MANUAL) {
            autoSetMode(fid, AUTO_MANUAL);
            DBG("[MQTT] Fan%d -> MANUAL (manual override)\n", fid + 1);
        }
    }
}

// ── Pieslēgties brokerim ──────────────────────────────────
static bool mqttConnect() {
    if (mqttCfg.host[0] == '\0') return false;

    String clientId = "smartfan-" + WiFi.macAddress();
    clientId.replace(":", "");

    // Last Will Testament — Mosquitto publicēs "offline" ja ESP atvienojas
    String willTopic = String(mqttCfg.prefix) + "/status";

    bool ok;
    if (mqttCfg.user[0] != '\0') {
        ok = mqtt.connect(clientId.c_str(), mqttCfg.user, mqttCfg.pass,
                          willTopic.c_str(), 1, true, "offline");
    } else {
        ok = mqtt.connect(clientId.c_str(), NULL, NULL,
                          willTopic.c_str(), 1, true, "offline");
    }

    if (ok) {
        DBG("[MQTT] Piesledzies: %s:%d\n", mqttCfg.host, mqttCfg.port);

        // Subscribe uz komandu topiciem
        String prefix = String(mqttCfg.prefix);
        mqtt.subscribe((prefix + "/fan1/set").c_str());
        mqtt.subscribe((prefix + "/fan2/set").c_str());
        mqtt.subscribe((prefix + "/relay1/set").c_str());
        mqtt.subscribe((prefix + "/relay2/set").c_str());
        mqtt.subscribe((prefix + "/fan1/mode/set").c_str());
        mqtt.subscribe((prefix + "/fan2/mode/set").c_str());
        // Automatizācijas config caur MQTT
        mqtt.subscribe((prefix + "/fan1/schedule/day/+").c_str());
        mqtt.subscribe((prefix + "/fan2/schedule/day/+").c_str());
        mqtt.subscribe((prefix + "/fan1/temp-curve/set").c_str());
        mqtt.subscribe((prefix + "/fan2/temp-curve/set").c_str());
        mqtt.subscribe((prefix + "/fan1/config/get").c_str());
        mqtt.subscribe((prefix + "/fan2/config/get").c_str());
        // Server mode komandas
        mqtt.subscribe((prefix + "/server/heartbeat").c_str());
        mqtt.subscribe((prefix + "/server/fan1/set").c_str());
        mqtt.subscribe((prefix + "/server/fan2/set").c_str());
        mqtt.subscribe((prefix + "/server/timezone").c_str());
        // OTA komandas
        mqtt.subscribe((prefix + "/ota/check").c_str());
        mqtt.subscribe((prefix + "/ota/server/set").c_str());
    } else {
        DBG("[MQTT] Kluda: %d\n", mqtt.state());
    }
    return ok;
}

// ── Tūlīt publicēt konkrēta fana stāvokli ────────────────
void mqttPublishFan(uint8_t fanId) {
    if (!mqtt.connected()) return;
    if (fanId >= FAN_COUNT || !fanIsActive(fanId)) return;

    String prefix = String(mqttCfg.prefix);
    String f = prefix + "/fan" + String(fanId + 1);
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", fanGetSpeed(fanId));
    mqtt.publish((f + "/speed").c_str(), buf, true);

    snprintf(buf, sizeof(buf), "%lu", fanGetRPM(fanId));
    mqtt.publish((f + "/rpm").c_str(), buf, true);

    snprintf(buf, sizeof(buf), "%d", relayGet(fanId) ? 1 : 0);
    mqtt.publish((f + "/relay").c_str(), buf, true);

    mqtt.publish((f + "/mode").c_str(), autoModeStr(autoGetMode(fanId)), true);
}

// ── Publicēt relay stāvokli uz komandu topiku (lai serveris redz) ──
void mqttPublishRelay(uint8_t fanId, bool state) {
    if (!mqtt.connected()) return;
    if (fanId >= FAN_COUNT) return;

    String prefix = String(mqttCfg.prefix);
    String topic = prefix + "/relay" + String(fanId + 1) + "/set";
    mqtt.publish(topic.c_str(), state ? "1" : "0", false);  // bez retain
    DBG("[MQTT] Publish: %s = %d\n", topic.c_str(), state ? 1 : 0);
}

// ── Publicēt visu statusu ─────────────────────────────────
static void mqttPublish() {
    if (!mqtt.connected()) return;

    String prefix = String(mqttCfg.prefix);
    char buf[16];

    // Aktīvo fanu skaits
    uint8_t activeFans = 0;
    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (fanIsActive(i)) activeFans++;
    }
    snprintf(buf, sizeof(buf), "%d", activeFans);
    mqtt.publish((prefix + "/fan_count").c_str(), buf, true);

    // Fani
    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (!fanIsActive(i)) continue;
        String f = prefix + "/fan" + String(i + 1);

        snprintf(buf, sizeof(buf), "%d", fanGetSpeed(i));
        mqtt.publish((f + "/speed").c_str(), buf, true);

        snprintf(buf, sizeof(buf), "%lu", fanGetRPM(i));
        mqtt.publish((f + "/rpm").c_str(), buf, true);

        snprintf(buf, sizeof(buf), "%d", relayGet(i) ? 1 : 0);
        mqtt.publish((f + "/relay").c_str(), buf, true);

        // Automatizācijas režīms
        mqtt.publish((f + "/mode").c_str(), autoModeStr(autoGetMode(i)), true);
    }

    // DS18B20 (bez retain — lai izslēgti sensori nepalieku brokerī)
    if (devIsEnabled(DEV_DS18B20)) {
        uint8_t cnt = devDS18B20Count();
        for (uint8_t i = 0; i < cnt; i++) {
            const DS18B20Sensor* s = devDS18B20Get(i);
            if (!s || !s->valid) continue;
            String t = prefix + "/ds18b20/" + String(s->name);
            snprintf(buf, sizeof(buf), "%.1f", s->temp);
            mqtt.publish(t.c_str(), buf, false);
        }
    }

    // DHT22 (bez retain)
    if (devIsEnabled(DEV_DHT22)) {
        float t = devGetDHTTemp();
        float h = devGetDHTHum();
        String dhtTopic = prefix + "/" + String(devGetDHTName());
        if (t > -100) {
            snprintf(buf, sizeof(buf), "%.1f", t);
            mqtt.publish((dhtTopic + "/temp").c_str(), buf, false);
        }
        if (h >= 0) {
            snprintf(buf, sizeof(buf), "%.1f", h);
            mqtt.publish((dhtTopic + "/hum").c_str(), buf, false);
        }
    }

    // Sensoru saraksts — JSON ar nosaukumiem un (type,idx) kartējumu
    // Lai automācijas UI var rādīt reālos nosaukumus
    {
        String json = "[";
        bool first = true;
        if (devIsEnabled(DEV_DS18B20)) {
            uint8_t cnt = devDS18B20Count();
            for (uint8_t i = 0; i < cnt; i++) {
                const DS18B20Sensor* s = devDS18B20Get(i);
                if (!s || !s->valid) continue;
                if (!first) json += ",";
                json += "{\"t\":0,\"i\":" + String(i) + ",\"n\":\"" + String(s->name) + "\"}";
                first = false;
            }
        }
        if (devIsEnabled(DEV_DHT22)) {
            if (!first) json += ",";
            json += "{\"t\":1,\"i\":0,\"n\":\"" + String(devGetDHTName()) + "\"}";
            first = false;
        }
        json += "]";
        mqtt.publish((prefix + "/sensors").c_str(), json.c_str(), true);
    }

    // Online status
    mqtt.publish((prefix + "/status").c_str(), "online", true);

    // Firmware info
    mqtt.publish((prefix + "/fw_version").c_str(), otaGetVersion().c_str(), true);
    mqtt.publish((prefix + "/hw_variant").c_str(), otaGetVariant().c_str(), true);
}

// ── OTA status publicēšana ────────────────────────────────
void mqttPublishOtaStatus(const char* status) {
    if (!mqtt.connected()) return;
    String topic = String(mqttCfg.prefix) + "/ota/status";
    mqtt.publish(topic.c_str(), status, false);
    DBG("[OTA] MQTT status: %s\n", status);
}

// ═══════════════════════════════════════════════════════════
//  PUBLISKĀS FUNKCIJAS
// ═══════════════════════════════════════════════════════════

void mqttInitPrefix() {
    // Ģenerēt unikālu prefiksu no MAC (izsauc vienmēr, arī bez WiFi)
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String prefix = prefs.getString("mqtt_pfx", "");
    prefs.end();

    if (prefix.length() == 0 || prefix == "sf_000000") {
        char buf[16];
#ifdef ESP8266
        uint32_t chipId = ESP.getChipId();
        snprintf(buf, sizeof(buf), "sf_%06X", chipId);
#else
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(buf, sizeof(buf), "sf_%02X%02X%02X", mac[3], mac[4], mac[5]);
#endif
        prefix = String(buf);
        Preferences wprefs;
        wprefs.begin(NVS_NAMESPACE, false);
        wprefs.putString("mqtt_pfx", prefix);
        wprefs.end();
        DBG("[MQTT] Auto-prefix no MAC: %s\n", buf);
    }

    strncpy(mqttCfg.prefix, prefix.c_str(), 31); mqttCfg.prefix[31] = '\0';
    DBG("[MQTT] Prefix: %s\n", mqttCfg.prefix);
}

void mqttInit() {
    // Ielādēt config no NVS (prefix jau iestatīts mqttInitPrefix)
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String host   = prefs.getString("mqtt_host", "");
    mqttCfg.port  = prefs.getUShort("mqtt_port", MQTT_DEFAULT_PORT);
    String user   = prefs.getString("mqtt_user", "");
    String pass   = prefs.getString("mqtt_pass", "");
    prefs.end();

    DBG("[MQTT] NVS read: host='%s' port=%d user='%s'\n", host.c_str(), mqttCfg.port, user.c_str());

    strncpy(mqttCfg.host, host.c_str(), 63);    mqttCfg.host[63] = '\0';
    strncpy(mqttCfg.user, user.c_str(), 31);    mqttCfg.user[31] = '\0';
    strncpy(mqttCfg.pass, pass.c_str(), 31);    mqttCfg.pass[31] = '\0';
    mqttCfg.enabled = devIsEnabled(DEV_MQTT);

    if (mqttCfg.host[0] == '\0') {
        DBGLN("[MQTT] Nav konfigurēts (nav host)");
        return;
    }

    mqtt.setServer(mqttCfg.host, mqttCfg.port);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);

    DBG("[MQTT] Init: %s:%d prefix=%s\n",
        mqttCfg.host, mqttCfg.port, mqttCfg.prefix);
}

void mqttLoop() {
    if (!mqttCfg.enabled || mqttCfg.host[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    uint32_t now = millis();

    if (!mqtt.connected()) {
        if (now - lastReconnect > MQTT_RECONNECT_INTERVAL) {
            lastReconnect = now;
            mqttConnect();
        }
        return;
    }

    mqtt.loop();

    // Apstrādāt atliktās publicēšanas (no callback karogiem)
    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (pendingPublish[i]) {
            pendingPublish[i] = false;
            mqttPublishFan(i);
        }
    }

    if (now - lastPublish > MQTT_PUBLISH_INTERVAL) {
        mqttPublish();
        lastPublish = now;
    }
}

bool mqttIsConnected() {
    return mqtt.connected();
}

MqttConfig& mqttGetConfig() {
    return mqttCfg;
}

void mqttSaveConfig(const MqttConfig& cfg) {
    mqttCfg = cfg;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("mqtt_host", cfg.host);
    prefs.putUShort("mqtt_port", cfg.port);
    prefs.putString("mqtt_user", cfg.user);
    prefs.putString("mqtt_pass", cfg.pass);
    prefs.putString("mqtt_pfx",  cfg.prefix);
    prefs.end();

    DBG("[MQTT] Config saved: %s:%d\n", cfg.host, cfg.port);

    // Pārkonfigurēt
    if (cfg.host[0] != '\0') {
        mqtt.setServer(cfg.host, cfg.port);
        mqtt.disconnect();
    }
}
