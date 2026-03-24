#include <Arduino.h>
#include "config.h"
#include "wifi_manager.h"
#include "ntp_time.h"
#include "web_server.h"
#include "device_manager.h"
#include "mqtt_client.h"
#include "automation.h"
#include "ota_updater.h"
#include "compat.h"

// Globālais debug flags
bool dbgEnabled = true;

void setup() {
    Serial.begin(115200);

    // Ielādēt debug iestatījumu no NVS
    {
        Preferences p;
        p.begin(NVS_NAMESPACE, true);
        dbgEnabled = p.getBool("debug", true);
        p.end();
    }

    DBGLN("\n=== Smart Fan D1 Mini ===");
    Serial.printf("[SYS] Debug: %s\n", dbgEnabled ? "ON" : "OFF");

    wifiInit();

    if (wifiIsConnected()) {
        ntpInit();
    }

    devInit();    // ielādē konfigurāciju un ieslēdz ierīces

    webInit();

    // MQTT prefiksa ģenerēšana (vienmēr, arī bez WiFi)
    mqttInitPrefix();

    // MQTT savienojums (tikai ja WiFi pieslēgts)
    if (devIsEnabled(DEV_MQTT) && wifiIsConnected()) {
        mqttInit();
    }

    // Automatizācija (pēc fanu un sensoru init)
    autoInit();

    // OTA atjaunināšana
    otaInit();

    DBGLN("=== Gatavs! ===");
}

void loop() {
    devLoop();    // visu ierīču apstrāde (fani, sensori)
    autoLoop();   // fanu automatizācija (grafiks / temperatūra)
    if (devIsEnabled(DEV_MQTT))    mqttLoop();
    otaLoop();    // OTA pārbaude reizi stundā
    webLoop();    // DNS captive portal

    // Seriālais monitors — reizi 5s
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        DBG("[SYS] %s | %s | Config: 0x%04X\n",
            ntpGetTime().c_str(),
            wifiGetIP().c_str(),
            devGetConfig());
        lastPrint = millis();
    }
}
