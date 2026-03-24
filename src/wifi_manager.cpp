#include "wifi_manager.h"
#include "config.h"
#include "compat.h"
// Preferences via compat.h

static Preferences prefs;
static bool apMode = false;
static String apName;

// ── Ģenerē AP nosaukumu ar chip ID ─────────────────────────
static String makeAPName() {
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i += 8) {
        chipId |= ((espGetMac() >> (40 - i)) & 0xff) << i;
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "SmartFan-%04X", (uint16_t)(chipId & 0xFFFF));
    return String(buf);
}

// ── WiFi inicializācija ────────────────────────────────────
void wifiInit() {
    // read-write lai izveidotu namespace ja neeksistē
    prefs.begin(NVS_NAMESPACE, false);
    String ssid = prefs.isKey(NVS_KEY_SSID) ? prefs.getString(NVS_KEY_SSID, "") : "";
    String pass = prefs.isKey(NVS_KEY_PASS) ? prefs.getString(NVS_KEY_PASS, "") : "";
    prefs.end();

    if (ssid.length() == 0) {
        // Nav saglabātu credentials → AP režīms
        apMode = true;
        apName = makeAPName();
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apName.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
        DBG("[WIFI] AP režīms: %s (parole: %s)\n", apName.c_str(), AP_PASSWORD);
        DBG("[WIFI] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        return;
    }

    // Mēģinam savienoties
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    DBG("[WIFI] Savienojas ar: %s", ssid.c_str());

    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500);
        Serial.print(".");
        tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        DBG("\n[WIFI] Savienots! IP: %s\n", WiFi.localIP().toString().c_str());
        apMode = false;
    } else {
        // Neizdevās → AP režīms
        DBGLN("\n[WIFI] Neizdevās savienoties — startējam AP");
        apMode = true;
        apName = makeAPName();
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apName.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
        DBG("[WIFI] AP: %s | IP: %s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
    }
}

bool wifiIsConnected() {
    return !apMode && WiFi.status() == WL_CONNECTED;
}

String wifiGetIP() {
    if (apMode) return WiFi.softAPIP().toString();
    return WiFi.localIP().toString();
}

bool wifiIsAP() {
    return apMode;
}

String wifiGetAPName() {
    return apName;
}

void wifiReset() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove(NVS_KEY_SSID);
    prefs.remove(NVS_KEY_PASS);
    prefs.end();
    DBGLN("[WIFI] Credentials dzēsti — restartējam");
    delay(500);
    ESP.restart();
}
