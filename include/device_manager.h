#pragma once
#include <Arduino.h>
#include "config.h"

// ── DS18B20 multi-sensor struktūra ──────────────────────────
struct DS18B20Sensor {
    uint8_t addr[8];    // ROM adrese
    char    name[16];   // lietotāja nosaukums (max 15 chars)
    float   temp;       // pēdējais rādījums
    bool    valid;      // sensors atbildēja
};

void     devInit();                     // ielādē config no NVS, ieslēdz ierīces
void     devLoop();                     // visu aktīvo ierīču loop
uint16_t devGetConfig();                // tekošais bitmask
void     devSetConfig(uint16_t mask);   // saglabā NVS + restart
bool     devIsEnabled(uint16_t flag);   // vai ierīce ieslēgta?
String   devGetStatusJSON();            // viss status JSON formātā

// Releju vadība (id: 0=Relay1/GPIO14, 1=Relay2/GPIO27)
void     relaySet(uint8_t id, bool on);
bool     relayGet(uint8_t id);

// Izsauc fan_control — automātiski pārslēdz fana releju
void     relaySetForFan(uint8_t fanId, bool on);

// DS18B20 multi-sensor API
uint8_t              devDS18B20Count();
const DS18B20Sensor* devDS18B20Get(uint8_t idx);
uint8_t              devDS18B20Scan();   // skenē busu, ielādē nosaukumus no NVS
void                 devDS18B20SetName(uint8_t idx, const char* name);
String               devDS18B20AddrStr(const uint8_t addr[8]);

// Sensoru vērtību getteri (display modulim)
float    devGetDHTTemp();
float    devGetDHTHum();

// Sensoru nosaukumi (DHT22)
const char* devGetDHTName();
void        devSetDHTName(const char* name);

// Ierīces nosaukums
const char* devGetName();
void        devSetName(const char* name);
