#pragma once
#include <Arduino.h>

void        wifiInit();
bool        wifiIsConnected();
String      wifiGetIP();
bool        wifiIsAP();         // vai darbojas AP režīmā
String      wifiGetAPName();    // AP SSID nosaukums
void        wifiReset();        // dzēst saglabātos credentials
