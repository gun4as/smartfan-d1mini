#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "fan_control.h"
#include "ntp_time.h"
#include "device_manager.h"
#include "mqtt_client.h"
#include "automation.h"
#include "ota_updater.h"

#include "compat.h"
// AsyncTCP via compat.h
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
// Preferences via compat.h

static AsyncWebServer server(WEB_PORT);
static DNSServer      dnsServer;
static bool           dnsRunning = false;

// ── Captive portal HTML (AP režīmā) ───────────────────────
static const char CAPTIVE_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="lv">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Fan — WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;
     justify-content:center;padding:40px 16px}
.card{background:#16213e;border-radius:16px;padding:32px;width:100%;
      max-width:380px;box-shadow:0 4px 24px #0004}
h1{font-size:20px;margin-bottom:8px;color:#00d4aa}
p{font-size:13px;color:#aaa;margin-bottom:24px}
label{font-size:13px;color:#aaa;display:block;margin-bottom:6px}
input[type=text],input[type=password]{width:100%;padding:12px;
    background:#0f3460;border:1px solid #1a4080;border-radius:8px;
    color:#eee;font-size:15px;margin-bottom:16px;outline:none}
input:focus{border-color:#00d4aa}
button{display:block;width:100%;padding:14px;background:#00d4aa;
       color:#1a1a2e;font-size:15px;font-weight:600;border:none;
       border-radius:10px;cursor:pointer}
button:active{opacity:.8}
.ok{text-align:center;color:#00d4aa;margin-top:20px;display:none}
</style>
</head>
<body>
<div class="card">
  <h1>&#127744; Smart Fan</h1>
  <p>Ievadi sava WiFi tikla datus</p>
  <div id="scan-area" style="margin-bottom:16px">
    <label>WiFi tikli</label>
    <div id="wifi-list" style="font-size:13px;color:#aaa">Mekle tiklus...</div>
  </div>
  <form id="f">
    <label>WiFi nosaukums (SSID)</label>
    <input type="text" id="ssid" name="ssid" required autocomplete="off">
    <label>Parole</label>
    <input type="password" id="pass" name="pass" autocomplete="off">
    <button type="submit">Saglabat un pieslegt</button>
  </form>
  <div class="ok" id="ok">Saglabats! Restartejas...</div>
</div>
<script>
function scanWifi(){
  fetch('/api/wifi-scan').then(function(r){return r.json()}).then(function(d){
    if(d.scanning){setTimeout(scanWifi,2000);return;}
    var nets=d.networks||[];
    if(nets.length===0){document.getElementById('wifi-list').textContent='Nav atrasts neviens tikls';return;}
    var seen={};var html='';
    for(var i=0;i<nets.length;i++){
      var n=nets[i];if(seen[n.ssid])continue;seen[n.ssid]=1;
      var sig=n.rssi>-50?'&#9679;&#9679;&#9679;':n.rssi>-70?'&#9679;&#9679;&#9675;':'&#9679;&#9675;&#9675;';
      var lock=n.enc?'&#128274;':'';
      html+='<div style="padding:8px 10px;background:#0f3460;border-radius:8px;margin-bottom:6px;cursor:pointer;display:flex;justify-content:space-between;align-items:center" onclick="pickWifi(\''+n.ssid+'\')">';
      html+='<span style="color:#eee">'+n.ssid+'</span>';
      html+='<span style="font-size:11px;color:#aaa">'+sig+' '+lock+' '+n.rssi+'dBm</span>';
      html+='</div>';
    }
    html+='<div style="text-align:center;margin-top:8px"><a style="color:#00d4aa;cursor:pointer;font-size:12px" onclick="scanWifi()">Meklet velreiz</a></div>';
    document.getElementById('wifi-list').innerHTML=html;
  });
}
function pickWifi(ssid){
  document.getElementById('ssid').value=ssid;
  document.getElementById('pass').focus();
}
scanWifi();
document.getElementById('f').addEventListener('submit',function(e){
  e.preventDefault();
  var s=document.getElementById('ssid').value;
  var p=document.getElementById('pass').value;
  fetch('/api/wifi-save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))
    .then(function(){
      document.getElementById('f').style.display='none';
      document.getElementById('scan-area').style.display='none';
      document.getElementById('ok').style.display='block';
    });
});
</script>
</body>
</html>
)rawhtml";

// ── Atliktais restart (ļauj async response nosūtīties) ────
static uint16_t pendingConfigMask = 0;
static uint32_t pendingRestartAt = 0;
static bool pendingIsRestart = false;

// ── WiFi saglabāšana un restart ────────────────────────────
static void handleWifiSave(AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid")) {
        req->send(400, "text/plain", "Nav SSID");
        return;
    }
    String ssid = req->getParam("ssid")->value();
    String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    prefs.end();

    DBG("[WEB] WiFi saglabāts: %s\n", ssid.c_str());
    req->send(200, "text/plain", "OK");
    pendingRestartAt = millis() + 1500;
    pendingIsRestart = true;
}

// ── Status API — visi dati no device_manager ───────────────
static void handleStatus(AsyncWebServerRequest* req) {
    String json = devGetStatusJSON();
    // Pievienojam WiFi info
    json.remove(json.length() - 1); // noņem pēdējo }
    json += ",\"ip\":\"" + wifiGetIP() + "\"";
    json += ",\"ap\":" + String(wifiIsAP() ? "true" : "false");
    json += ",\"synced\":" + String(ntpIsSynced() ? "true" : "false");
    json += ",\"tz\":\"" + ntpGetTimezone() + "\"";
    // Automatizācijas režīms katram fanam
    json += ",\"autoMode\":[";
    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (i > 0) json += ",";
        json += String((uint8_t)autoGetMode(i));
    }
    json += "]";
    json += ",\"minPwm\":[" + String(fanGetMinPwm(0)) + "," + String(fanGetMinPwm(1)) + "]";
    json += ",\"maxPwm\":[" + String(fanGetMaxPwm(0)) + "," + String(fanGetMaxPwm(1)) + "]";
    json += "}";
    req->send(200, "application/json", json);
}

// ── Min/Max PWM iestatīšana ────────────────────────────────
static void handleSetPwmLimits(AsyncWebServerRequest* req) {
    if (!req->hasParam("fan")) { req->send(400, "text/plain", "Nav fan"); return; }
    uint8_t fan = req->getParam("fan")->value().toInt();
    if (fan < 1 || fan > FAN_COUNT) { req->send(400, "text/plain", "Bad fan"); return; }
    uint8_t id = fan - 1;
    if (req->hasParam("min")) fanSetMinPwm(id, req->getParam("min")->value().toInt());
    if (req->hasParam("max")) fanSetMaxPwm(id, req->getParam("max")->value().toInt());
    req->send(200, "application/json", "{\"ok\":true,\"minPwm\":" + String(fanGetMinPwm(id)) + ",\"maxPwm\":" + String(fanGetMaxPwm(id)) + "}");
}

// ── Fana ātruma iestatīšana ────────────────────────────────
static void handleSetSpeed(AsyncWebServerRequest* req) {
    if (req->hasParam("speed")) {
        int id  = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
        int spd = req->getParam("speed")->value().toInt();
        // Manuāla ātruma maiņa → pārslēdz uz MANUAL režīmu
        if (autoGetMode((uint8_t)id) != AUTO_MANUAL) {
            autoSetMode((uint8_t)id, AUTO_MANUAL);
            DBG("[WEB] Fan%d -> MANUAL (manual speed override)\n", id + 1);
        }
        fanSetSpeed((uint8_t)id, (uint8_t)spd);
        fanSaveNow((uint8_t)id);  // Vienmēr saglabāt NVS no web
        DBG("[WEB] Fan%d speed=%d%% (NVS saved)\n", id + 1, spd);
        mqttPublishFan((uint8_t)id);
    }
    req->send(200, "text/plain", "OK");
}

// ── Releja vadība ──────────────────────────────────────────
static void handleRelay(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    if (req->hasParam("state")) {
        String s = req->getParam("state")->value();
        bool turnOn = (s == "1" || s == "on" || s == "true");
        relaySet((uint8_t)id, turnOn);
        if (turnOn) {
            fanSetManualOff((uint8_t)id, false);  // ieslēdz — noņem manualOff
        } else {
            fanSetManualOff((uint8_t)id, true);    // izslēdz — uzstāda manualOff
        }
        DBG("[WEB] Relay%d -> %s (manualOff=%d)\n", id + 1,
            turnOn ? "ON" : "OFF", !turnOn);
        // Publicēt uz relay/set topiku (serveris nolasa) + fan stāvokli
        mqttPublishRelay((uint8_t)id, turnOn);
        mqttPublishFan((uint8_t)id);
    }
    req->send(200, "text/plain", relayGet((uint8_t)id) ? "ON" : "OFF");
}

// ── Konfigurācijas API ─────────────────────────────────────
static void handleGetConfig(AsyncWebServerRequest* req) {
    String json = "{\"config\":" + String(devGetConfig()) + ",\"variant\":\"" + otaGetVariant() + "\"}";
    req->send(200, "application/json", json);
}

static void handleSetConfig(AsyncWebServerRequest* req) {
    if (req->hasParam("mask")) {
        pendingConfigMask = (uint16_t)req->getParam("mask")->value().toInt();
        pendingRestartAt = millis() + 1000; // Restart pēc 1s (ļauj nosūtīt response)
        req->send(200, "text/plain", "OK — restartējam");
    } else {
        req->send(400, "text/plain", "Nav mask parametra");
    }
}

// ── DS18B20 scan ──────────────────────────────────────────
static void handleDS18B20Scan(AsyncWebServerRequest* req) {
    uint8_t count = devDS18B20Scan();
    String json = "{\"count\":" + String(count) + ",\"sensors\":[";
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        const DS18B20Sensor* s = devDS18B20Get(i);
        json += "{\"addr\":\"" + devDS18B20AddrStr(s->addr) + "\"";
        json += ",\"name\":\"" + String(s->name) + "\"}";
    }
    json += "]}";
    req->send(200, "application/json", json);
}

// ── DS18B20 nosaukumi ─────────────────────────────────────
static void handleDS18B20Names(AsyncWebServerRequest* req) {
    uint8_t count = devDS18B20Count();
    String json = "{\"count\":" + String(count) + ",\"sensors\":[";
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        const DS18B20Sensor* s = devDS18B20Get(i);
        json += "{\"addr\":\"" + devDS18B20AddrStr(s->addr) + "\"";
        json += ",\"name\":\"" + String(s->name) + "\"}";
    }
    json += "]}";
    req->send(200, "application/json", json);
}

// ── DS18B20 nosaukuma iestatīšana ─────────────────────────
static void handleDS18B20SetName(AsyncWebServerRequest* req) {
    if (!req->hasParam("idx") || !req->hasParam("name")) {
        req->send(400, "text/plain", "Trukst idx vai name");
        return;
    }
    uint8_t idx = (uint8_t)req->getParam("idx")->value().toInt();
    String name = req->getParam("name")->value();
    if (idx >= devDS18B20Count()) {
        req->send(400, "text/plain", "Nepareizs idx");
        return;
    }
    name.trim();
    if (name.length() > 15) name = name.substring(0, 15);
    devDS18B20SetName(idx, name.c_str());
    req->send(200, "text/plain", "OK");
}

// ── MQTT config API ───────────────────────────────────────
static void handleMqttGet(AsyncWebServerRequest* req) {
    MqttConfig& c = mqttGetConfig();
    String json = "{\"host\":\"" + String(c.host) + "\"";
    json += ",\"port\":" + String(c.port);
    json += ",\"user\":\"" + String(c.user) + "\"";
    json += ",\"prefix\":\"" + String(c.prefix) + "\"";
    json += ",\"connected\":" + String(mqttIsConnected() ? "true" : "false");
    json += "}";
    req->send(200, "application/json", json);
}

static void handleMqttSet(AsyncWebServerRequest* req) {
    MqttConfig cfg = mqttGetConfig();
    if (req->hasParam("host")) {
        String h = req->getParam("host")->value();
        strncpy(cfg.host, h.c_str(), 63); cfg.host[63] = '\0';
    }
    if (req->hasParam("port")) {
        cfg.port = (uint16_t)req->getParam("port")->value().toInt();
    }
    if (req->hasParam("user")) {
        String u = req->getParam("user")->value();
        strncpy(cfg.user, u.c_str(), 31); cfg.user[31] = '\0';
    }
    if (req->hasParam("pass")) {
        String p = req->getParam("pass")->value();
        strncpy(cfg.pass, p.c_str(), 31); cfg.pass[31] = '\0';
    }
    // prefix ir read-only (ģenerēts no MAC) — neļauj mainīt
    mqttSaveConfig(cfg);
    req->send(200, "text/plain", "OK");
}

// ── Automatizācijas API ───────────────────────────────────────

// GET /api/auto?id=0 → viss automation status fanam
static void handleAutoGet(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    if (id >= FAN_COUNT) { req->send(400, "text/plain", "Bad id"); return; }

    AutoMode mode = autoGetMode((uint8_t)id);
    TempCurve& tc = autoGetTempCurve((uint8_t)id);

    String json = "{\"mode\":" + String((uint8_t)mode);
    json += ",\"modeStr\":\"" + String(autoModeStr(mode)) + "\"";
    json += ",\"schedule\":" + autoGetScheduleJSON((uint8_t)id);
    json += ",\"temp\":{";
    json += "\"sensorType\":" + String(tc.sensorType);
    json += ",\"sensorIdx\":" + String(tc.sensorIdx);
    json += ",\"tempMin\":" + String(tc.tempMin, 1);
    json += ",\"tempMax\":" + String(tc.tempMax, 1);
    json += ",\"speedMin\":" + String(tc.speedMin);
    json += ",\"speedMax\":" + String(tc.speedMax);
    json += "}}";
    req->send(200, "application/json", json);
}

// GET /api/auto-mode?id=0&mode=1
static void handleAutoMode(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    if (!req->hasParam("mode")) { req->send(400, "text/plain", "Nav mode"); return; }
    int mode = req->getParam("mode")->value().toInt();
    if (mode < 0 || mode > 2) { req->send(400, "text/plain", "Bad mode"); return; }

    autoSetMode((uint8_t)id, (AutoMode)mode);
    DBG("[WEB] Fan%d auto mode -> %s\n", id + 1, autoModeStr((AutoMode)mode));
    mqttPublishFan((uint8_t)id);
    req->send(200, "text/plain", autoModeStr((AutoMode)mode));
}

// GET /api/auto-schedule?id=0 → grafika JSON
static void handleAutoScheduleGet(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    req->send(200, "application/json", autoGetScheduleJSON((uint8_t)id));
}

// POST /api/auto-schedule?id=0  body = JSON [[...48...], ...×7]
static String scheduleBody;
static uint8_t scheduleTargetFan = 0;

static void handleAutoSchedulePost(AsyncWebServerRequest* req) {
    autoSetScheduleFromJSON(scheduleTargetFan, scheduleBody);
    DBG("[WEB] Fan%d schedule saved\n", scheduleTargetFan + 1);
    req->send(200, "text/plain", "OK");
    scheduleBody = "";
}

static void handleAutoScheduleBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        scheduleTargetFan = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
        scheduleBody = "";
        scheduleBody.reserve(total + 1);
    }
    for (size_t i = 0; i < len; i++) {
        scheduleBody += (char)data[i];
    }
}

// GET /api/auto-temp?id=0 → temp līknes JSON
static void handleAutoTempGet(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    TempCurve& tc = autoGetTempCurve((uint8_t)id);

    String json = "{\"sensorType\":" + String(tc.sensorType);
    json += ",\"sensorIdx\":" + String(tc.sensorIdx);
    json += ",\"tempMin\":" + String(tc.tempMin, 1);
    json += ",\"tempMax\":" + String(tc.tempMax, 1);
    json += ",\"speedMin\":" + String(tc.speedMin);
    json += ",\"speedMax\":" + String(tc.speedMax);
    json += "}";
    req->send(200, "application/json", json);
}

// GET /api/auto-temp-set?id=0&sensor=0&idx=0&tmin=20&tmax=35&smin=0&smax=100
static void handleAutoTempSet(AsyncWebServerRequest* req) {
    int id = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    if (id >= FAN_COUNT) { req->send(400, "text/plain", "Bad id"); return; }

    TempCurve tc = autoGetTempCurve((uint8_t)id);

    if (req->hasParam("sensor"))  tc.sensorType = (uint8_t)req->getParam("sensor")->value().toInt();
    if (req->hasParam("idx"))     tc.sensorIdx  = (uint8_t)req->getParam("idx")->value().toInt();
    if (req->hasParam("tmin"))    tc.tempMin    = req->getParam("tmin")->value().toFloat();
    if (req->hasParam("tmax"))    tc.tempMax    = req->getParam("tmax")->value().toFloat();
    if (req->hasParam("smin"))    tc.speedMin   = (uint8_t)req->getParam("smin")->value().toInt();
    if (req->hasParam("smax"))    tc.speedMax   = (uint8_t)req->getParam("smax")->value().toInt();

    autoSetTempCurve((uint8_t)id, tc);
    DBG("[WEB] Fan%d temp curve: sensor=%d idx=%d tmin=%.1f tmax=%.1f smin=%d smax=%d\n",
        id + 1, tc.sensorType, tc.sensorIdx, tc.tempMin, tc.tempMax, tc.speedMin, tc.speedMax);
    req->send(200, "text/plain", "OK");
}

// ── Ierīces nosaukums ─────────────────────────────────────────
static void handleDeviceName(AsyncWebServerRequest* req) {
    String json = "{\"name\":\"" + String(devGetName()) + "\"}";
    req->send(200, "application/json", json);
}

static void handleDeviceSetName(AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) {
        req->send(400, "text/plain", "Trukst name");
        return;
    }
    String name = req->getParam("name")->value();
    name.trim();
    if (name.length() > 23) name = name.substring(0, 23);
    if (name.length() == 0) name = "Smart Fan";
    devSetName(name.c_str());
    req->send(200, "text/plain", "OK");
}

// ── Sensoru nosaukumi ─────────────────────────────────────────
static void handleSensorNames(AsyncWebServerRequest* req) {
    String json = "{";
    json += "\"dht22\":\"" + String(devGetDHTName()) + "\"";
    json += "}";
    req->send(200, "application/json", json);
}

static void handleSensorSetName(AsyncWebServerRequest* req) {
    if (!req->hasParam("type") || !req->hasParam("name")) {
        req->send(400, "text/plain", "Trukst type vai name");
        return;
    }
    String type = req->getParam("type")->value();
    String name = req->getParam("name")->value();
    name.trim();
    if (name.length() > 15) name = name.substring(0, 15);

    if (type == "dht22") {
        devSetDHTName(name.c_str());
    } else {
        req->send(400, "text/plain", "Nezinams tips");
        return;
    }
    req->send(200, "text/plain", "OK");
}

// ── Debug toggle ────────────────────────────────────────────
static void handleDebugGet(AsyncWebServerRequest* req) {
    String json = "{\"enabled\":" + String(dbgEnabled ? "true" : "false") + "}";
    req->send(200, "application/json", json);
}

static void handleDebugSet(AsyncWebServerRequest* req) {
    if (req->hasParam("on")) {
        bool on = req->getParam("on")->value() == "1";
        dbgEnabled = on;
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        p.putBool("debug", on);
        p.end();
        DBG("[WEB] Debug -> %s\n", on ? "ON" : "OFF");
    }
    req->send(200, "text/plain", dbgEnabled ? "ON" : "OFF");
}

// ── WiFi reset ─────────────────────────────────────────────
static void handleWifiReset(AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "WiFi reset");
    delay(500);
    wifiReset();
}

// ── Factory reset ──────────────────────────────────────────
static void handleFactoryReset(AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Factory reset");
    DBGLN("[SYS] FACTORY RESET — dzēšu visu NVS...");
    delay(500);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();  // Dzēš VISAS atslēgas no NVS namespace
    prefs.end();
    DBGLN("[SYS] NVS notīrīts. Restartējos...");
    delay(200);
    ESP.restart();
}

// ═══════════════════════════════════════════════════════════
void webInit() {
    if (!LittleFS.begin()) {
        DBGLN("[WEB] LittleFS kļūda!");
    } else {
        DBGLN("[WEB] LittleFS OK");
    }

    if (wifiIsAP()) {
        dnsServer.start(53, "*", WiFi.softAPIP());
        dnsRunning = true;
        DBGLN("[WEB] DNS captive portal startēts");

        server.on("/api/wifi-save", HTTP_GET, handleWifiSave);
        server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_FAILED) {
                WiFi.scanNetworks(true); // async scan
                req->send(200, "application/json", "{\"scanning\":true}");
                return;
            }
            if (n == WIFI_SCAN_RUNNING) {
                req->send(200, "application/json", "{\"scanning\":true}");
                return;
            }
            String json = "{\"networks\":[";
            for (int i = 0; i < n; i++) {
                if (i > 0) json += ",";
                json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                        ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false") + "}";
            }
            json += "]}";
            WiFi.scanDelete();
            req->send(200, "application/json", json);
        });
        server.onNotFound([](AsyncWebServerRequest* req) {
            req->send_P(200, "text/html", CAPTIVE_HTML);
        });
    } else {
        // API endpointi
        server.on("/api/status",     HTTP_GET, handleStatus);
        server.on("/api/set",        HTTP_GET, handleSetSpeed);
        server.on("/api/pwm-limits", HTTP_GET, handleSetPwmLimits);
        server.on("/api/relay",      HTTP_GET, handleRelay);
        server.on("/api/config",     HTTP_GET, handleGetConfig);
        server.on("/api/config-set", HTTP_GET, handleSetConfig);
        server.on("/api/wifi-reset",        HTTP_GET, handleWifiReset);
        server.on("/api/factory-reset",     HTTP_GET, handleFactoryReset);
        server.on("/api/ds18b20-scan",      HTTP_GET, handleDS18B20Scan);
        server.on("/api/ds18b20-names",     HTTP_GET, handleDS18B20Names);
        server.on("/api/ds18b20-set-name",  HTTP_GET, handleDS18B20SetName);
        server.on("/api/mqtt",              HTTP_GET, handleMqttGet);
        server.on("/api/mqtt-set",          HTTP_GET, handleMqttSet);
        server.on("/api/debug",             HTTP_GET, handleDebugGet);
        server.on("/api/prefs-dump",        HTTP_GET, [](AsyncWebServerRequest* req) {
            if (LittleFS.exists("/prefs/smartfan.json")) {
                File f = LittleFS.open("/prefs/smartfan.json", "r");
                String content = f.readString();
                f.close();
                req->send(200, "application/json", content);
            } else {
                req->send(200, "text/plain", "File not found");
            }
        });
        server.on("/api/debug-set",         HTTP_GET, handleDebugSet);
        server.on("/api/sensor-names",      HTTP_GET, handleSensorNames);
        server.on("/api/sensor-set-name",   HTTP_GET, handleSensorSetName);
        server.on("/api/device-name",       HTTP_GET, handleDeviceName);
        server.on("/api/device-set-name",   HTTP_GET, handleDeviceSetName);

        // Automatizācija
        server.on("/api/auto",              HTTP_GET, handleAutoGet);
        server.on("/api/auto-mode",         HTTP_GET, handleAutoMode);
        server.on("/api/auto-schedule",     HTTP_GET, handleAutoScheduleGet);
        server.on("/api/auto-schedule",     HTTP_POST, handleAutoSchedulePost, NULL, handleAutoScheduleBody);
        server.on("/api/auto-temp",         HTTP_GET, handleAutoTempGet);
        server.on("/api/auto-temp-set",     HTTP_GET, handleAutoTempSet);

        // Timezone
        server.on("/api/timezone", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(200, "application/json", "{\"tz\":\"" + ntpGetTimezone() + "\"}");
        });
        server.on("/api/timezone-set", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (req->hasParam("tz")) {
                ntpSetTimezone(req->getParam("tz")->value());
                req->send(200, "application/json", "{\"ok\":true}");
            } else {
                req->send(400, "application/json", "{\"error\":\"missing tz\"}");
            }
        });

        // OTA
        server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest* req) {
            String json = "{\"version\":\"" + otaGetVersion() + "\"";
            json += ",\"variant\":\"" + otaGetVariant() + "\"";
            json += ",\"server\":\"" + otaGetServer() + "\"";
            json += ",\"mqtt\":" + String(mqttIsConnected() ? "true" : "false");
            json += "}";
            req->send(200, "application/json", json);
        });

        server.on("/api/ota-set-server", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!req->hasParam("url")) { req->send(400, "text/plain", "Trukst url"); return; }
            String url = req->getParam("url")->value();
            otaSetServer(url);
            req->send(200, "text/plain", "OK");
        });

        server.on("/api/ota-check", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (otaGetServer().length() == 0) {
                req->send(400, "text/plain", "OTA serveris nav konfigurēts");
                return;
            }
            String newVer = otaCheckOnly();
            if (newVer.length() > 0) {
                req->send(200, "application/json", "{\"available\":true,\"version\":\"" + newVer + "\"}");
            } else {
                req->send(200, "application/json", "{\"available\":false}");
            }
        });

        server.on("/api/ota-update", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(200, "text/plain", "Atjauninu...");
            otaStartUpdate();
        });

        // LittleFS statiskie faili
        server.serveStatic("/", LittleFS, "/")
            .setDefaultFile("index.html")
            .setCacheControl("no-cache, no-store, must-revalidate");
    }

    delay(500);
    server.begin();
    DBGLN("[WEB] Serveris startēts");
}

void webLoop() {
    if (dnsRunning) {
        dnsServer.processNextRequest();
    }
    // Atliktais restart pēc config/wifi save (ļauj HTTP response nosūtīties)
    if (pendingRestartAt > 0 && millis() >= pendingRestartAt) {
        pendingRestartAt = 0;
        if (pendingIsRestart) {
            ESP.restart();
        } else {
            devSetConfig(pendingConfigMask);
        }
    }
}
