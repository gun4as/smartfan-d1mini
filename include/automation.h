#pragma once
#include <Arduino.h>
#include "config.h"
#include "fan_control.h"

// ── Automatizācijas režīmi ────────────────────────────────
enum AutoMode : uint8_t {
    AUTO_MANUAL   = 0,   // lietotājs kontrolē manuāli
    AUTO_SCHEDULE = 1,   // nedēļas grafiks (48 sloti × 7 dienas)
    AUTO_TEMP     = 2,   // temperatūras līkne
    AUTO_SERVER   = 3    // serveris vadā (lokālās auto izslēgtas)
};

// ── Grafika konstantes ────────────────────────────────────
#define SLOTS_PER_DAY   48    // katras 30 min
#define DAYS_PER_WEEK    7
#define SCHEDULE_SIZE   (SLOTS_PER_DAY * DAYS_PER_WEEK)  // 336 baiti

// ── Temperatūras līknes struktūra ─────────────────────────
struct TempCurve {
    uint8_t  sensorType;   // 0=DS18B20, 1=DHT22, 2=AHT20
    uint8_t  sensorIdx;    // DS18B20 indekss (0-7), ignorē citiem
    float    tempMin;      // zem šīs — speedMin%
    float    tempMax;      // virs šīs — speedMax%
    uint8_t  speedMin;     // minimālais % (default 0)
    uint8_t  speedMax;     // maksimālais % (default 100)
};

// ── Publiskās funkcijas ──────────────────────────────────
void     autoInit();          // ielādē no NVS
void     autoLoop();          // izsauc no main loop

AutoMode autoGetMode(uint8_t fanId);
void     autoSetMode(uint8_t fanId, AutoMode mode);
void     autoSaveMode(uint8_t fanId);

// Grafiks
uint8_t  autoGetSlot(uint8_t fanId, uint8_t day, uint8_t slot);
void     autoSetSlot(uint8_t fanId, uint8_t day, uint8_t slot, uint8_t speed);
void     autoSaveSchedule(uint8_t fanId);
String   autoGetScheduleJSON(uint8_t fanId);
void     autoSetScheduleFromJSON(uint8_t fanId, const String& json);

// Temperatūras līkne
TempCurve& autoGetTempCurve(uint8_t fanId);
void       autoSetTempCurve(uint8_t fanId, const TempCurve& curve);
void       autoSaveTempCurve(uint8_t fanId);

// Server mode
void     autoServerHeartbeat();         // serveris atsūta heartbeat
void     autoServerSetSpeed(uint8_t fanId, uint8_t speed);  // serveris iestata ātrumu
AutoMode autoGetPreviousMode(uint8_t fanId);  // iepriekšējais režīms pirms SERVER

// Palīgi
const char* autoModeStr(AutoMode mode);
