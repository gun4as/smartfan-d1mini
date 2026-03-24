#pragma once
#include <Arduino.h>

void     mqttInitPrefix();     // ģenerē unikālu prefiksu no MAC (vienmēr, arī bez WiFi)
void     mqttInit();           // ielādē config no NVS, pieslēdzas
void     mqttLoop();           // reconnect + publish + subscribe
bool     mqttIsConnected();    // vai pieslēgts brokerim
void     mqttPublishFan(uint8_t fanId);  // tūlīt publicēt fana stāvokli
void     mqttPublishRelay(uint8_t fanId, bool state);  // publicēt relay uz /set topiku

// MQTT config NVS glabāšana
struct MqttConfig {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[32];
    char prefix[32];    // topic prefikss, piem. "smartfan"
    bool enabled;
};

MqttConfig& mqttGetConfig();
void        mqttSaveConfig(const MqttConfig& cfg);

// OTA status publicēšana
void mqttPublishOtaStatus(const char* status);
