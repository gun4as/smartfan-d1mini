#include "ota_updater.h"
#include "config.h"
#include "mqtt_client.h"
#include "compat.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
// Preferences via compat.h

// ESP8266 Updater API
#define UPDATE_ERROR_STR() Update.getErrorString().c_str()
#define UPDATE_ABORT()     Update.end(false)
#ifndef U_FS
  #define U_FS 100
#endif

// ── Firmware versija (mainās ar katru releasi) ──────────────
#define FW_VERSION "1.0.4"

#define HW_VARIANT "d1_mini"

// ── OTA konfigurācija ──────────────────────────────────────
#define OTA_CHECK_INTERVAL  3600000   // Pārbaudīt reizi stundā
#define OTA_TIMEOUT         30000     // HTTP timeout

static String otaServerUrl = "";      // No NVS vai MQTT
static uint32_t lastOtaCheck = 0;
static bool otaInProgress = false;
static String pendingFwUrl = "";      // Gaidošais firmware URL
static String pendingFsUrl = "";      // Gaidošais LittleFS URL
static String pendingVersion = "";    // Gaidošā versija
static String fsVersion = "";         // Pašreizējā LittleFS versija

// ═══════════════════════════════════════════════════════════
//  PUBLISKĀS FUNKCIJAS
// ═══════════════════════════════════════════════════════════

void otaInit() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    otaServerUrl = p.getString("ota_url", "");
    fsVersion = p.getString("fs_ver", "0.0.0");
    p.end();

    DBG("[OTA] FW: %s, FS: %s, Variant: %s\n", FW_VERSION, fsVersion.c_str(), HW_VARIANT);
    if (otaServerUrl.length() > 0) {
        DBG("[OTA] Server: %s\n", otaServerUrl.c_str());
    } else {
        DBGLN("[OTA] Server nav konfigurēts");
    }
}

String otaGetVersion() {
    return String(FW_VERSION);
}

String otaGetVariant() {
    return String(HW_VARIANT);
}

String otaGetServer() {
    return otaServerUrl;
}

// Iestatīt OTA servera URL (no MQTT vai web)
void otaSetServer(const String& url) {
    otaServerUrl = url;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("ota_url", url);
    p.end();
    DBG("[OTA] Server set: %s\n", url.c_str());
}

// ── Pārbaudīt vai ir jauna versija ────────────────────────
static String fsDownloadUrl = "";
static BearSSL::WiFiClientSecure secureClient;
static WiFiClient plainClient;

static void httpBegin(HTTPClient& http, const String& url) {
    if (url.startsWith("https")) {
        secureClient.setInsecure();
        secureClient.setBufferSizes(1024, 1024); // Samazināt no 16KB uz 1KB+1KB
        http.begin(secureClient, url);
    } else {
        http.begin(plainClient, url);
    }
    http.setTimeout(OTA_TIMEOUT);
}

// Chunked write ar watchdog barošanu
static size_t writeChunked(WiFiClient* stream, int contentLength) {
    uint8_t buf[512];
    size_t written = 0;
    unsigned long lastActivity = millis();

    // ESP8266: yield() katrā iterācijā baro watchdog

    while (written < (size_t)contentLength) {
        size_t avail = stream->available();
        if (avail) {
            size_t toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
            size_t rd = stream->readBytes(buf, toRead);
            if (rd > 0) {
                Update.write(buf, rd);
                written += rd;
                lastActivity = millis();
            }
            yield();
        } else {
            delay(1);
            if (millis() - lastActivity > 30000) {
                DBG("[OTA] Timeout pie %d/%d\n", written, contentLength);
                break;
            }
        }
    }

    return written;
}

static bool performFsUpdate(const String& url) {
    DBG("[OTA] LittleFS lejupielāde: %s\n", url.c_str());
    mqttPublishOtaStatus("updating_fs");

    HTTPClient http;
    httpBegin(http, url);
    int code = http.GET();

    if (code != 200) {
        DBG("[OTA] FS download failed: %d\n", code);
        http.end();
        mqttPublishOtaStatus(("error:fs_http_" + String(code)).c_str());
        return false;
    }

    int contentLength = http.getSize();
    DBG("[OTA] FS Content-Length: %d\n", contentLength);

    if (contentLength <= 0) {
        http.end();
        mqttPublishOtaStatus("error:fs_no_content_length");
        return false;
    }

    if (!Update.begin(contentLength, U_FS)) {
        DBG("[OTA] FS nav vietas: %s\n", UPDATE_ERROR_STR());
        http.end();
        mqttPublishOtaStatus(("error:fs_begin:" + String(UPDATE_ERROR_STR())).c_str());
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t written = writeChunked(stream, contentLength);
    http.end();

    if (written != (size_t)contentLength) {
        DBG("[OTA] FS rakstīti %d/%d\n", written, contentLength);
        UPDATE_ABORT();
        mqttPublishOtaStatus(("error:fs_write_" + String(written) + "/" + String(contentLength)).c_str());
        return false;
    }

    if (!Update.end(true)) {
        DBG("[OTA] FS kļūda: %s\n", UPDATE_ERROR_STR());
        mqttPublishOtaStatus(("error:fs_end:" + String(UPDATE_ERROR_STR())).c_str());
        return false;
    }

    DBGLN("[OTA] LittleFS atjaunināts!");

    // Saglabāt FS versiju NVS
    if (pendingVersion.length() > 0) {
        fsVersion = pendingVersion;
    } else {
        fsVersion = FW_VERSION;
    }
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("fs_ver", fsVersion);
    p.end();
    DBG("[OTA] FS version saved: %s\n", fsVersion.c_str());

    mqttPublishOtaStatus("fs_done");
    return true;
}

static bool checkForUpdate(String& downloadUrl) {
    if (otaServerUrl.length() == 0) return false;
    if (!WiFi.isConnected()) return false;

    // GET /api/ota/check?variant=xxx&version=xxx
    String checkUrl = otaServerUrl + "/api/ota/check?variant=" + HW_VARIANT + "&version=" + FW_VERSION + "&fs_version=" + fsVersion;
    DBG("[OTA] Check: %s\n", checkUrl.c_str());

    HTTPClient http;
    httpBegin(http, checkUrl);
    int code = http.GET();

    if (code != 200) {
        DBG("[OTA] Check failed: %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Vienkāršs JSON parse: {"update":true,"url":"...","version":"2.0.1","fs_url":"..."}
    if (body.indexOf("\"update\":true") < 0) {
        DBGLN("[OTA] Nav atjauninājumu");
        return false;
    }

    // Konstruēt download URL no mūsu zināmā servera URL
    downloadUrl = otaServerUrl + "/api/ota/download/" + HW_VARIANT;

    // Izvilkt versiju
    int verStart = body.indexOf("\"version\":\"");
    if (verStart >= 0) {
        verStart += 11;
        int verEnd = body.indexOf("\"", verStart);
        if (verEnd > verStart) {
            pendingVersion = body.substring(verStart, verEnd);
        }
    }

    // LittleFS URL — ja serveris atbildēja ka ir fs_url
    if (body.indexOf("\"fs_url\"") >= 0) {
        fsDownloadUrl = otaServerUrl + "/api/ota/download/d1_mini_littlefs";
        DBG("[OTA] LittleFS update: %s\n", fsDownloadUrl.c_str());
    }

    DBG("[OTA] Jauna versija pieejama: %s\n", pendingVersion.c_str());
    return true;
}

// ── Lejupielādēt un flashēt ───────────────────────────────
static bool performUpdate(const String& url) {
    DBG("[OTA] Lejupielāde: %s\n", url.c_str());
    otaInProgress = true;

    // Publicēt statusu uz MQTT
    mqttPublishOtaStatus("downloading");

    HTTPClient http;
    httpBegin(http, url);
    int code = http.GET();

    if (code != 200) {
        DBG("[OTA] Download failed: %d\n", code);
        http.end();
        otaInProgress = false;
        mqttPublishOtaStatus(("error:http_" + String(code)).c_str());
        return false;
    }

    int contentLength = http.getSize();
    DBG("[OTA] Content-Length: %d\n", contentLength);

    if (contentLength <= 0) {
        DBGLN("[OTA] Invalid content length");
        http.end();
        otaInProgress = false;
        mqttPublishOtaStatus("error:no_content_length");
        return false;
    }

    DBG("[OTA] Firmware izmērs: %d bytes\n", contentLength);

    if (!Update.begin(contentLength)) {
        DBG("[OTA] Nav vietas: %s\n", UPDATE_ERROR_STR());
        http.end();
        otaInProgress = false;
        mqttPublishOtaStatus(("error:no_space:" + String(UPDATE_ERROR_STR())).c_str());
        return false;
    }

    mqttPublishOtaStatus("flashing");

    WiFiClient* stream = http.getStreamPtr();
    size_t written = writeChunked(stream, contentLength);

    if (written != (size_t)contentLength) {
        DBG("[OTA] Rakstīti %d/%d bytes\n", written, contentLength);
        UPDATE_ABORT();
        http.end();
        otaInProgress = false;
        mqttPublishOtaStatus(("error:write_" + String(written) + "/" + String(contentLength)).c_str());
        return false;
    }

    if (!Update.end(true)) {
        DBG("[OTA] Finalize kļūda: %s\n", UPDATE_ERROR_STR());
        http.end();
        otaInProgress = false;
        mqttPublishOtaStatus("error");
        return false;
    }

    http.end();
    DBG("[OTA] Veiksmīgi! Restartē...\n");
    mqttPublishOtaStatus("rebooting");
    delay(1000);
    ESP.restart();
    return true;  // Nekad nesasniegs
}

// Tikai pārbaudīt — atgriež jauno versiju vai ""
String otaCheckOnly() {
    if (otaInProgress) return "";
    pendingFwUrl = "";
    pendingFsUrl = "";
    pendingVersion = "";
    fsDownloadUrl = "";

    String downloadUrl;
    if (checkForUpdate(downloadUrl)) {
        pendingFwUrl = downloadUrl;
        pendingFsUrl = fsDownloadUrl;
        return pendingVersion;
    }
    return "";
}

// Sākt atjaunināšanu (pēc checkOnly)
void otaStartUpdate() {
    if (otaInProgress) return;
    if (pendingFwUrl.length() == 0) return;

    // Vispirms LittleFS, tad firmware
    if (pendingFsUrl.length() > 0) {
        performFsUpdate(pendingFsUrl);
        pendingFsUrl = "";
        secureClient.stop();       // Atbrīvot TLS atmiņu
        delay(1000);               // Dot laiku GC
        DBG("[OTA] Free heap: %d\n", ESP.getFreeHeap());
    }
    performUpdate(pendingFwUrl);
}

void otaCheckNow() {
    if (otaInProgress) return;

    String downloadUrl;
    fsDownloadUrl = "";
    if (checkForUpdate(downloadUrl)) {
        // Vispirms LittleFS (web faili), tad firmware (kas restartē)
        if (fsDownloadUrl.length() > 0) {
            performFsUpdate(fsDownloadUrl);
            secureClient.stop();   // Atbrīvot TLS atmiņu
            delay(1000);
        }
        performUpdate(downloadUrl);
    }
    lastOtaCheck = millis();
}

void otaLoop() {
    if (otaInProgress) return;
    if (otaServerUrl.length() == 0) return;

    uint32_t now = millis();
    if (now - lastOtaCheck < OTA_CHECK_INTERVAL) return;

    otaCheckNow();
}
