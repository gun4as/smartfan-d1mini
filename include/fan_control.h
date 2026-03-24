#pragma once
#include <Arduino.h>

#define FAN_COUNT 2

enum FanType { FAN_NONE = 0, FAN_4PIN };

void     fanInit(uint8_t id, FanType type);   // id = 0,1
void     fanStop(uint8_t id);
void     fanLoop();                            // RPM aprēķins visiem
void     fanSetSpeed(uint8_t id, uint8_t percent);
uint8_t  fanGetSpeed(uint8_t id);
uint32_t fanGetRPM(uint8_t id);
bool     fanIsActive(uint8_t id);
void     fanSetSpeedAuto(uint8_t id, uint8_t percent);  // no automatizācijas — bez NVS
void     fanSaveNow(uint8_t id);           // piespiedu NVS saglabāšana
void     fanSetManualOff(uint8_t id, bool off);  // manuāls izslēgts karogs
bool     fanIsManualOff(uint8_t id);
void     fanSetMinPwm(uint8_t id, uint8_t percent);
void     fanSetMaxPwm(uint8_t id, uint8_t percent);
uint8_t  fanGetMinPwm(uint8_t id);
uint8_t  fanGetMaxPwm(uint8_t id);
