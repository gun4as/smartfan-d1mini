#pragma once
#include <Arduino.h>

void    ntpInit();
void    ntpSetTimezone(const String& posixTz);
String  ntpGetTimezone();
bool    ntpIsSynced();
String  ntpGetTime();       // "HH:MM:SS"
String  ntpGetDate();       // "DD.MM.YYYY"
time_t  ntpGetEpoch();      // epoch sekundes
