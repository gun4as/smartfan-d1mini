#pragma once
#include <Arduino.h>

void otaInit();          // Iestatīt OTA parametrus
void otaLoop();          // Periodiski pārbaudīt atjauninājumus
void otaCheckNow();      // Pārbaudīt un atjaunināt tūlīt (MQTT komanda)
String otaCheckOnly();   // Tikai pārbaudīt — atgriež jauno versiju vai ""
void otaStartUpdate();   // Sākt atjaunināšanu (pēc checkOnly)
String otaGetVersion();  // Atgriezt pašreizējo firmware versiju
String otaGetVariant();  // Atgriezt hw variantu
String otaGetServer();   // Atgriezt OTA servera URL
void otaSetServer(const String& url);  // Iestatīt OTA servera URL
